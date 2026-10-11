"""Prototype stand-in for the LTX Alpha Gen worker. No model, no torch.

Same process contract the real worker will have (run by job.py, one run per job,
so the real model loads once per job):
  argv : --in <shm file> --chunks s:l,s:l,... --out-prefix <path> [--seconds S per chunk]
         [--simulate none|oom|crash|hang] [--precision bf16|fp8]
  stdin: kept open by the job; EOF means the job is gone -> exit.
  stdout, one line per message:
    info <text>
    progress <0..1> <text>        free-form progress (before the first chunk)
    stage <chunk> <name>          converting | preparing | denoising | decoding
    step <chunk> <k> <n>          denoising step k of n finished
    chunk_done <i>                <out-prefix>_<i>.bin holds chunk i's mattes
    done
    error <kind> <text>           kind: oom | input | internal
The "matte" is the Rec.709 luminance, computed on padded frames and cropped back.
"""

from __future__ import annotations

import argparse
import os
import signal
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402

from ltxag_worker import padding, transport  # noqa: E402

DENOISE_STEPS = 8  # recipe 1 (distilled) step count


def say(*parts: object) -> None:
    sys.stdout.write(" ".join(str(p) for p in parts) + "\n")
    sys.stdout.flush()


def watch_parent() -> None:
    """Exit as soon as our parent closes stdin (it died or cancelled)."""
    sys.stdin.read()
    os._exit(3)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="inp", required=True)
    ap.add_argument("--chunks", required=True)
    ap.add_argument("--out-prefix", required=True)
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--simulate", default="none", choices=["none", "oom", "crash", "hang"])
    ap.add_argument("--precision", default="bf16")
    args = ap.parse_args()
    chunks = [tuple(int(v) for v in c.split(":")) for c in args.chunks.split(",")]

    threading.Thread(target=watch_parent, daemon=True).start()
    say("info", f"python={sys.executable} {sys.version.split()[0]} pid={os.getpid()}")
    say("info", "env", " ".join(f"{k}={'set' if os.environ.get(k) else 'unset'}"
                               for k in ("PYTHONHOME", "PYTHONPATH", "LD_LIBRARY_PATH")))
    say("info", f"precision={args.precision} simulate={args.simulate} chunks={len(chunks)}")

    step_s = args.seconds / (DENOISE_STEPS + 2)

    say("progress", 0.0, "loading model (simulated)")
    time.sleep(step_s)
    if args.simulate == "oom":
        say("error", "oom", "CUDA out of memory (simulated)")
        return 2
    if args.simulate == "crash":
        os.kill(os.getpid(), signal.SIGSEGV)
    if args.simulate == "hang":
        while True:
            time.sleep(1)

    for i, (start, length) in enumerate(chunks):
        label = f"chunk {i + 1}/{len(chunks)}"
        try:
            frames = transport.read_frames_range(args.inp, start, length)
        except Exception as exc:  # noqa: BLE001 - report any input problem
            say("error", "input", str(exc).replace("\n", " "))
            return 2
        n, h, w, _ = frames.shape
        padded, (top, left) = padding.pad_spatial(frames)
        padded = padding.pad_temporal(padded)
        say("info", f"{label}: in {n}x{w}x{h} -> model {padded.shape[0]}x{padded.shape[2]}x{padded.shape[1]}")
        say("stage", i, "denoising")
        for s in range(DENOISE_STEPS):
            time.sleep(step_s)
            say("step", i, s + 1, DENOISE_STEPS)
        say("stage", i, "decoding")
        rgb = padded.astype(np.float32)
        luma = np.clip(rgb[..., 0] * 0.2126 + rgb[..., 1] * 0.7152 + rgb[..., 2] * 0.0722, 0.0, 1.0)
        transport.write_mattes(f"{args.out_prefix}_{i}.bin", padding.crop(luma, n, top, left, h, w))
        say("chunk_done", i)
    say("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
