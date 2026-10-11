"""LTX-2.5 Alpha Gen worker (the real one). Runs in the LTX-2 venv, launched by job.py.

Same process contract as dummy_worker.py (see its docstring), plus:
  --model-dir DIR      expects diffusion_models/ text_encoders/ vae/ loras/ latent_upscale_models/
  --embeddings FILE    cached prompt embeddings (safetensors). Built on first use if missing.
  --seed N             default 1234 (the model card's sample seed)

Recipe 1 (decision 15/16): the model-card IC-LoRA recipe, stage 1 only at native resolution
(`ICLoraPipeline`, skip_stage_2, width/height passed as 2x), empty prompt, LoRA strength 1.0.
Two injections into the pinned LTX-2 code (commit 9ec55f9):
  - ic_lora.SequentialVideoFrameSource -> MemoryFrameSource: reference frames come from the
    shim's /dev/shm input instead of a video file (no 8-bit re-encode).
  - pipeline.prompt_encoder -> CachedPromptEncoder: the empty-prompt embedding is loaded from
    disk, so Gemma is not loaded per job (decision 9; see CLAUDE.md for why the final
    embedding is cached).
Frames arrive display-referred in [0,1] (the shim converts colour); the model was trained on
8-bit video, so they are scaled to [0,255] floats, the range the pipeline's reader produces.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import threading
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
# Same allocator setting as the measured CLI runs; must be set before torch initialises CUDA.
os.environ.setdefault("PYTORCH_CUDA_ALLOC_CONF", "expandable_segments:True")

import numpy as np  # noqa: E402

from ltxag_worker import padding, transport  # noqa: E402

MEMORY_REF = "mem://ltxag-reference"
SEED_DEFAULT = 1234
FRAME_RATE = 24.0  # only feeds the model's timing embedding; mattes do not depend on it here


def say(*parts: object) -> None:
    sys.stdout.write(" ".join(str(p) for p in parts) + "\n")
    sys.stdout.flush()


def watch_parent() -> None:
    sys.stdin.read()
    os._exit(3)


class MemoryFrameSource:
    """Drop-in for ltx_pipelines.chunks.export.SequentialVideoFrameSource (take/close)."""

    frames = None  # torch tensor (N, H, W, 3) float in [0,255] on the device, set per chunk

    def __init__(self, path: str, device) -> None:  # noqa: ANN001 - torch.device
        if path != MEMORY_REF:
            raise ValueError(f"MemoryFrameSource only serves {MEMORY_REF}, got {path}")

    def take(self, start: int, count: int):
        f = MemoryFrameSource.frames
        return iter([f[i : i + 1] for i in range(start, min(start + count, f.shape[0]))])

    def close(self) -> None:
        pass


class CachedPromptEncoder:
    """Stands in for PromptEncoder: returns the stored empty-prompt embedding."""

    def __init__(self, output) -> None:  # noqa: ANN001 - EmbeddingsProcessorOutput
        self.output = output

    def __call__(self, prompts, **_kwargs):  # noqa: ANN001
        if list(prompts) != [""]:
            raise ValueError("cached embedding only covers the empty prompt")
        return [self.output]


def file_id(path: str) -> str:
    st = os.stat(path)
    return f"{os.path.basename(path)}:{st.st_size}"


def embeddings_ident(transformer: str, text_encoder: str) -> str:
    return f"{file_id(transformer)}|{file_id(text_encoder)}"


def build_embeddings(path: str, transformer: str, text_encoder: str, video_vae: str) -> int:
    """Child-process mode: run Gemma once for the empty prompt, save, exit.

    Runs in its own process because Gemma's ~26 GB were not fully returned before the
    transformer loaded when this ran in-process (smoke test: OOM at 94 GB vs 45.5 GB without).
    """
    import torch
    from safetensors.torch import save_file

    from ltx_pipelines.utils.blocks import PromptEncoder
    from ltx_pipelines.utils.model_paths import ModelPaths

    t0 = time.time()
    model_paths = ModelPaths.from_split(transformer_path=transformer, text_encoder_path=text_encoder,
                                        video_vae_path=video_vae)
    encoder = PromptEncoder(model_paths, torch.bfloat16, torch.device("cuda"))
    with torch.inference_mode():
        (out,) = encoder([""])
    tensors = {"video_encoding": out.video_encoding, "attention_mask": out.attention_mask}
    if out.audio_encoding is not None:
        tensors["audio_encoding"] = out.audio_encoding
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = f"{path}.tmp{os.getpid()}"
    save_file({k: v.contiguous().cpu() for k, v in tensors.items()}, tmp,
              metadata={"source": embeddings_ident(transformer, text_encoder), "prompt": ""})
    os.replace(tmp, path)
    say("info", f"prompt embedding built in {time.time() - t0:.1f} s: {path} "
                f"(video_encoding {tuple(out.video_encoding.shape)} {out.video_encoding.dtype})")
    return 0


def load_or_build_embeddings(device, path: str, paths: dict):  # noqa: ANN001 - torch.device
    from safetensors import safe_open
    from safetensors.torch import load_file

    from ltx_core.text_encoders.gemma.embeddings_processor import EmbeddingsProcessorOutput

    ident = embeddings_ident(paths["transformer"], paths["text_encoder"])
    current = False
    if os.path.exists(path):
        with safe_open(path, framework="pt") as f:
            current = (f.metadata() or {}).get("source") == ident
        if not current:
            say("info", "prompt embedding cache was built for other model files; rebuilding")
    if not current:
        say("progress", 0.0, "building prompt cache (one-time, loads Gemma)")
        rc = subprocess.run([sys.executable, "-I", "-B", os.path.abspath(__file__), "--build-embeddings", path,
                             "--transformer", paths["transformer"], "--text-encoder", paths["text_encoder"],
                             "--video-vae", paths["video_vae"]], stdin=subprocess.DEVNULL).returncode
        if rc != 0 or not os.path.exists(path):
            raise RuntimeError(f"building the prompt embedding cache failed (exit {rc})")
    t = load_file(path, device=str(device))
    say("info", f"prompt embedding loaded from {path}")
    return EmbeddingsProcessorOutput(t["video_encoding"], t.get("audio_encoding"), t["attention_mask"])


def main() -> int:  # noqa: PLR0915 - one linear job
    if "--build-embeddings" in sys.argv:
        bp = argparse.ArgumentParser()
        bp.add_argument("--build-embeddings", required=True)
        bp.add_argument("--transformer", required=True)
        bp.add_argument("--text-encoder", required=True)
        bp.add_argument("--video-vae", required=True)
        b = bp.parse_args()
        return build_embeddings(b.build_embeddings, b.transformer, b.text_encoder, b.video_vae)
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="inp", required=True)
    ap.add_argument("--chunks", required=True)
    ap.add_argument("--out-prefix", required=True)
    ap.add_argument("--model-dir", required=True)
    ap.add_argument("--embeddings", required=True)
    ap.add_argument("--precision", default="bf16", choices=["bf16", "fp8"])
    ap.add_argument("--seed", type=int, default=SEED_DEFAULT)
    ap.add_argument("--seconds", default=None, help="ignored (dummy worker option)")
    ap.add_argument("--simulate", default="none", help="ignored (dummy worker option)")
    args = ap.parse_args()
    chunks = [tuple(int(v) for v in c.split(":")) for c in args.chunks.split(",")]
    threading.Thread(target=watch_parent, daemon=True).start()

    t_start = time.time()
    say("info", f"python={sys.executable} {sys.version.split()[0]} pid={os.getpid()}")
    say("progress", 0.0, "loading torch")
    try:
        import torch

        import ltx_pipelines.ic_lora as ic_lora
        from ltx_core.loader import LTXV_LORA_COMFY_RENAMING_MAP, LoraPathStrengthAndSDOps
        from ltx_pipelines.utils.model_paths import ModelPaths
        from ltx_pipelines.utils.quantization_factory import QuantizationKind
    except Exception as exc:  # noqa: BLE001
        say("error", "internal", f"cannot import the LTX runtime: {exc!r}")
        traceback.print_exc(file=sys.stdout)
        return 2
    say("info", f"torch {torch.__version__} cuda {torch.version.cuda} "
                f"gpu {torch.cuda.get_device_name(0) if torch.cuda.is_available() else 'NONE'}")
    if not torch.cuda.is_available():
        say("error", "internal", "CUDA is not available to the worker")
        return 2

    m = args.model_dir
    paths = {
        "transformer": f"{m}/diffusion_models/ltx-2.5-22b-distilled-transformer-bf16.safetensors",
        "text_encoder": f"{m}/text_encoders/gemma4-12b-with-proj-ltx-2.5-bf16.safetensors",
        "video_vae": f"{m}/vae/ltx-2.5-video-vae-bf16.safetensors",
        "audio_vae": f"{m}/vae/ltx-2.5-audio-vae-bf16.safetensors",
        "upsampler": f"{m}/latent_upscale_models/ltx-2.5-latent-spatial-upscaler-x2-bf16-1.0.safetensors",
        "lora": f"{m}/loras/ltx-2.5-22b-ic-lora-alpha-gen-0.9.safetensors",
    }
    missing = [k for k, p in paths.items() if not os.path.exists(p)]
    if missing:
        say("error", "input", f"model files missing in {m}: {', '.join(missing)}")
        return 2

    try:
        # Before the pipeline exists, so a cache build (child process, Gemma) has the GPU to itself.
        embedding = load_or_build_embeddings(torch.device("cuda"), args.embeddings, paths)
        ic_lora.SequentialVideoFrameSource = MemoryFrameSource  # injection 1
        model_paths = ModelPaths.from_split(
            transformer_path=paths["transformer"], text_encoder_path=paths["text_encoder"],
            video_vae_path=paths["video_vae"], audio_vae_path=paths["audio_vae"])
        quant = QuantizationKind("fp8-cast").to_policy(paths["transformer"]) if args.precision == "fp8" else None
        pipeline = ic_lora.ICLoraPipeline(
            model_paths=model_paths, spatial_upsampler_path=paths["upsampler"],
            loras=[LoraPathStrengthAndSDOps(paths["lora"], 1.0, LTXV_LORA_COMFY_RENAMING_MAP)],
            quantization=quant)
        pipeline.prompt_encoder = CachedPromptEncoder(embedding)  # injection 2
        torch.cuda.empty_cache()
        say("info", f"pipeline ready in {time.time() - t_start:.1f} s (precision {args.precision})")

        torch.cuda.reset_peak_memory_stats()
        device = pipeline.device
        for i, (start, length) in enumerate(chunks):
            label = f"chunk {i + 1}/{len(chunks)}"
            t0 = time.time()
            say("progress", round(i / len(chunks), 4), f"{label}: preparing")
            frames = transport.read_frames_range(args.inp, start, length)  # (n,h,w,3) fp16 in [0,1]
            n, h, w, _ = frames.shape
            padded, (top, left) = padding.pad_spatial(frames)
            padded = padding.pad_temporal(padded)
            np_, hp, wp = padded.shape[0], padded.shape[1], padded.shape[2]
            MemoryFrameSource.frames = (torch.from_numpy(np.ascontiguousarray(padded)).to(device)
                                        .float().clamp_(0.0, 1.0).mul_(255.0))
            say("info", f"{label}: in {n}x{w}x{h} -> model {np_}x{wp}x{hp}")
            say("progress", round((i + 0.1) / len(chunks), 4), f"{label}: generating")
            with torch.inference_mode():
                out = pipeline(prompt="", seed=args.seed, height=hp * 2, width=wp * 2, num_frames=np_,
                               frame_rate=FRAME_RATE, images=[], video_conditioning=[(MEMORY_REF, 1.0)],
                               skip_stage_2=True)
                video = torch.cat([v.to("cpu") for v in out.video], dim=0)  # (F,H,W,C)
            if i == 0:
                say("info", f"decoded video {tuple(video.shape)} {video.dtype} "
                            f"min {float(video.min()):.3f} max {float(video.max()):.3f}")
            v = video.float()
            if video.dtype == torch.uint8 or float(v.max()) > 1.5:
                v = v / 255.0
            matte = v.mean(dim=-1).clamp_(0.0, 1.0).numpy()  # grayscale matte: average channels
            if matte.shape[1:] != (hp, wp):
                raise RuntimeError(f"decoded size {matte.shape[1:]} != model size {(hp, wp)}")
            transport.write_mattes(f"{args.out_prefix}_{i}.bin", padding.crop(matte, n, top, left, h, w))
            MemoryFrameSource.frames = None
            say("info", f"{label}: done in {time.time() - t0:.1f} s, "
                        f"peak allocated {torch.cuda.max_memory_allocated() / 2**30:.1f} GB")
            say("chunk_done", i)
            say("progress", round((i + 1) / len(chunks), 4), f"{label}: done")
    except torch.cuda.OutOfMemoryError as exc:
        say("error", "oom", f"CUDA out of memory: {str(exc).splitlines()[0]}")
        return 2
    except Exception as exc:  # noqa: BLE001 - report everything, with traceback, to the job log
        say("error", "internal", f"{type(exc).__name__}: {str(exc).splitlines()[0] if str(exc) else ''}")
        traceback.print_exc(file=sys.stdout)
        return 2
    say("info", f"job finished in {time.time() - t_start:.1f} s")
    say("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
