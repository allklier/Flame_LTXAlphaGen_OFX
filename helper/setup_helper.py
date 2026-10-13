"""Setup: install the LTX runtime and the models, then mark the user's setup ready.

Launched detached by the plugin's Download button (Setup tab). Standard library only: it runs
with the system python3 (Rocky 9: 3.9, macOS: 3.9 from the Command Line Tools) before any venv
exists.

  python3 -I -B setup_helper.py --model-dir D --cache-root R [--existing E [--link]] [--no-ui]
  HF_TOKEN in the environment (decision 13: never on argv, never stored). Only needed when a
  file has to be downloaded.

Steps (decisions 2/7/9/13/20/51), all pinned by model_manifest.json next to this file:
  1 runtime   <D>/runtime: uv (SHA-256 verified), LTX-2 source at the pinned commit,
              `uv sync --frozen --no-dev --extra natten`, extra wheels (OpenColorIO), CUDA smoke test
              (macOS: the darwin uv build and an MPS smoke test; the natten extra is Linux-only in
              the lockfile's markers, so it installs nothing there)
  2 models    for each file: already in <D> and verified -> keep; found by exact name under
              --existing and verified -> COPY (hashing while copying), or with --link a symlink to
              it after hashing it in place (decision 66); else download from HF
              (hashing while downloading). Verification = size + SHA-256 from the manifest.
  3 fp16      macOS only (decision 67): if a matmul probe finds fp16 at least 1.2x faster than
              bf16 on this Mac, an fp16 copy of the transformer in <D> (also when the models are
              linked); config transformer_fp16 = 1 or 0. Never fails Setup.
  3 prompt    build the empty-prompt embedding cache (worker --build-embeddings, own process)
  4 config    model_dir, ltx_python, runtime_commit, models_ready in ~/.config/ltx-alphagen
Progress and Cancel in a window (ltxag_worker/oslayer.py). Everything is logged (ltxag_worker/log.py).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
import threading
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
WORKER_DIR = os.path.join(HERE, "..", "worker")
sys.path.insert(0, WORKER_DIR)

from ltxag_worker import oslayer  # noqa: E402
from ltxag_worker.log import Logger  # noqa: E402

CONFIG = os.path.expanduser("~/.config/ltx-alphagen/config.conf")
CHUNK = 8 << 20
TITLE = "LTX Alpha Gen - setup"
log = Logger("setup")


class Cancelled(Exception):
    pass


class SetupError(Exception):
    pass


class Progress:
    """The setup progress window: text + percentage; cancel detection."""

    def __init__(self, enabled: bool) -> None:
        self.win = oslayer.ProgressWindow(TITLE, 520, enabled)
        self.lock = threading.Lock()  # the step and the size watcher both write
        self.done = False

    def update(self, frac: float, text: str) -> None:
        if self.cancelled():
            raise Cancelled()
        if self.win.shown:
            with self.lock:
                try:
                    self.win.show(frac, text)
                except BrokenPipeError as exc:
                    raise Cancelled() from exc

    def cancelled(self) -> bool:
        return not self.done and self.win.gone()

    def close(self) -> None:
        self.done = True
        self.win.close()

    @staticmethod
    def message(kind: str, text: str, enabled: bool) -> None:
        oslayer.message(kind, TITLE, text, 520, enabled)


def human(n: float) -> str:
    return f"{n / 1e9:.1f} GB" if n >= 1e9 else f"{n / 1e6:.0f} MB"


def stream_copy(src, dst_path: str, total: int, sha_expected: str, prog: Progress, label: str,
                frac0: float, frac1: float) -> None:
    """Copy a readable stream to dst_path.part while hashing; verify; then rename."""
    part = dst_path + ".part"
    os.makedirs(os.path.dirname(dst_path), exist_ok=True)
    h = hashlib.sha256()
    done, last = 0, 0.0
    t0 = time.time()
    with open(part, "wb") as out:
        while True:
            buf = src.read(CHUNK)
            if not buf:
                break
            out.write(buf)
            h.update(buf)
            done += len(buf)
            if time.time() - last > 0.5:
                rate = done / max(0.001, time.time() - t0)
                prog.update(frac0 + (frac1 - frac0) * done / max(1, total),
                            f"{label}: {human(done)} of {human(total)} ({human(rate)}/s)")
                last = time.time()
    if done != total or h.hexdigest() != sha_expected:
        os.unlink(part)
        raise SetupError(f"{os.path.basename(dst_path)}: size/hash mismatch "
                         f"({done} bytes, sha256 {h.hexdigest()[:16]}…)")
    os.replace(part, dst_path)


def dir_bytes(path: str) -> int:
    """Bytes under path, each inode once (uv hardlinks between its cache and the venv)."""
    seen, total = set(), 0
    for dirpath, _dirs, files in os.walk(path):
        for name in files:
            try:
                st = os.lstat(os.path.join(dirpath, name))
            except OSError:
                continue
            if (st.st_dev, st.st_ino) not in seen:
                seen.add((st.st_dev, st.st_ino))
                total += st.st_size
    return total


def sha256_file(path: str, prog: Progress, label: str, frac0: float, frac1: float) -> str:
    h = hashlib.sha256()
    total = os.path.getsize(path)
    done, last = 0, 0.0
    with open(path, "rb") as f:
        while True:
            buf = f.read(CHUNK)
            if not buf:
                break
            h.update(buf)
            done += len(buf)
            if time.time() - last > 0.5:
                prog.update(frac0 + (frac1 - frac0) * done / max(1, total), f"{label}: verifying {human(done)}")
                last = time.time()
    return h.hexdigest()


def verified_marker(path: str) -> str:
    return path + ".verified"


def already_verified(path: str, entry: dict) -> bool:
    """A previous run hashed this exact file (same size and mtime) and it matched."""
    try:
        with open(verified_marker(path)) as f:
            rec = json.load(f)
        st = os.stat(path)
        return rec == {"sha256": entry["sha256"], "size": st.st_size, "mtime": int(st.st_mtime)}
    except (OSError, ValueError):
        return False


def mark_verified(path: str, entry: dict) -> None:
    st = os.stat(path)
    with open(verified_marker(path), "w") as f:
        json.dump({"sha256": entry["sha256"], "size": st.st_size, "mtime": int(st.st_mtime)}, f)


def find_existing(root: str, name: str, size: int) -> list[str]:
    hits = []
    for dirpath, _dirs, files in os.walk(root):
        if name in files:
            p = os.path.join(dirpath, name)
            try:
                if os.path.getsize(p) == size:
                    hits.append(p)
            except OSError:
                pass
    return hits


def download(url: str, token: str, dst: str, entry: dict, prog: Progress, label: str, f0: float, f1: float,
             repo: str) -> None:
    req = urllib.request.Request(url, headers={"Authorization": f"Bearer {token}"} if token else {})
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            stream_copy(resp, dst, entry["size"], entry["sha256"], prog, label, f0, f1)
    except urllib.error.HTTPError as exc:
        if exc.code in (401, 403):
            raise SetupError(
                f"Hugging Face refused the download (HTTP {exc.code}). Check your token, and accept the "
                f"model terms with your account at https://huggingface.co/{repo}") from exc
        raise SetupError(f"download failed: HTTP {exc.code} for {url}") from exc
    except urllib.error.URLError as exc:
        raise SetupError(f"download failed: {exc.reason}") from exc


def run(cmd: list[str], env: dict, label: str, prog: Progress, frac: float, cwd: str | None = None,
        retries: int = 0, watch: tuple | None = None) -> None:
    """Run a step; network-bound steps pass retries>0 (PyPI timed out once in testing).
    watch = (folder, expected bytes, frac at end): show the folder's growth as "x of ~y GB"
    (uv prints no progress when it is not on a terminal)."""
    for attempt in range(retries + 1):
        try:
            return _run_once(cmd, env, label, prog, frac, cwd, watch)
        except SetupError as exc:
            if attempt == retries:
                raise
            log.warn(f"{label}: attempt {attempt + 1} failed ({exc}); retrying in 10 s")
            prog.update(frac, f"{label}: network problem, retrying...")
            time.sleep(10)


def _run_once(cmd: list[str], env: dict, label: str, prog: Progress, frac: float, cwd: str | None,
              watch: tuple | None = None) -> None:
    log.info(f"{label}: {' '.join(cmd)}")
    prog.update(frac, label)
    proc = subprocess.Popen(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    stop, cancelled = threading.Event(), threading.Event()

    def watcher() -> None:
        # Also catches Cancel while the command prints nothing (uv is silent for minutes).
        folder, expected, frac_end = watch if watch else ("", 0, frac)
        last_log = time.time()
        while not stop.wait(2.0):
            if prog.cancelled():
                cancelled.set()
                proc.kill()
                return
            if not watch:
                continue
            size = dir_bytes(folder)
            text = f"{label}: {human(size)} of ~{human(expected)}"
            try:
                prog.update(frac + (frac_end - frac) * min(1.0, size / max(1, expected)), text)
            except Cancelled:
                continue
            if time.time() - last_log > 15:
                log.info(text)
                last_log = time.time()

    t = threading.Thread(target=watcher, daemon=True)
    t.start()
    tail: list[str] = []
    try:
        for line in proc.stdout:
            line = line.rstrip("\n")
            tail = (tail + [line])[-20:]
            log.debug(f"{label}: {line}")
            if prog.cancelled():
                proc.kill()
                raise Cancelled()
        proc.wait()
    finally:
        stop.set()
        t.join()
    if cancelled.is_set():
        raise Cancelled()
    if watch:
        log.info(f"{label}: {human(dir_bytes(watch[0]))} in {watch[0]} (expected ~{human(watch[1])})")
    if proc.returncode != 0:
        log.error(f"{label} failed (exit {proc.returncode}):\n" + "\n".join(tail))
        # uv ends with a "hint:" line; the useful part is its "error:" line and the last "cause:".
        errs = [t.strip() for t in tail if t.strip().startswith("error:")]
        causes = [t.strip() for t in tail if t.strip().startswith("cause:")]
        why = " ".join(errs[:1] + causes[-1:]) or (tail[-1] if tail else "")
        raise SetupError(f"{label} failed (exit {proc.returncode}): {why}")


def find_c_compiler() -> str:
    """What Triton will use to build its small C helpers at run time ($CC, else gcc, else clang;
    triton/runtime/build.py:26-30). Empty if there is none.
    macOS: no Triton (Linux-only in the lockfile); mps-sdpa compiles its Objective-C++ bridge on
    first use (mps_sdpa/_cpp/__init__.py, torch cpp_extension: clang++ + ninja) and the progress
    window is Swift (ltxag_worker/oslayer.py), so the Command Line Tools are required. /usr/bin/clang
    is a stub even without them; xcrun finds the real tool or fails (man xcode-select)."""
    if sys.platform == "darwin":
        try:
            r = subprocess.run(["/usr/bin/xcrun", "--find", "clang++"], capture_output=True, text=True,
                               timeout=60)
        except (OSError, subprocess.SubprocessError):
            return ""
        return r.stdout.strip() if r.returncode == 0 else ""
    return os.environ.get("CC") or shutil.which("gcc") or shutil.which("clang") or ""


def install_runtime(man: dict, rt: str, prog: Progress) -> str:
    """Returns the venv python path."""
    env = dict(os.environ, UV_CACHE_DIR=f"{rt}/uv-cache", UV_PYTHON_INSTALL_DIR=f"{rt}/python",
               UV_NO_PROGRESS="1")
    for k in ("PYTHONHOME", "PYTHONPATH", "VIRTUAL_ENV", "CONDA_PREFIX", "LD_LIBRARY_PATH"):
        env.pop(k, None)
    os.makedirs(f"{rt}/bin", exist_ok=True)
    uv = f"{rt}/bin/uv"
    u = man["uv"]
    asset = u[sys.platform]  # "linux" or "darwin"
    if not (os.path.exists(uv) and subprocess.run([uv, "--version"], capture_output=True, text=True)
            .stdout.split()[1:2] == [u["version"]]):
        prog.update(0.01, f"runtime: downloading uv {u['version']}")
        tgz = f"{rt}/bin/uv.tar.gz"
        with urllib.request.urlopen(asset["url"], timeout=60) as resp:
            stream_copy(resp, tgz, asset["size"], asset["sha256"], prog, "uv", 0.01, 0.02)
        with tarfile.open(tgz) as t:
            for member in t.getmembers():
                if os.path.basename(member.name) in ("uv", "uvx") and member.isfile():
                    member.name = os.path.basename(member.name)
                    t.extract(member, f"{rt}/bin")
        os.unlink(tgz)
        log.info(f"uv {u['version']} installed (sha256 verified)")
    src = f"{rt}/ltx2"
    c = man["ltx2"]
    marker = f"{src}/.ltxag-commit"
    if not (os.path.exists(marker) and open(marker).read().strip() == c["commit"]):
        prog.update(0.03, f"runtime: downloading LTX-2 source {c['commit'][:7]}")
        shutil.rmtree(src, ignore_errors=True)
        tgz = f"{rt}/ltx2.tar.gz"
        with urllib.request.urlopen(c["url"], timeout=120) as resp, open(tgz, "wb") as out:
            done, last = 0, 0.0  # GitHub archives are generated on the fly: no total size
            for buf in iter(lambda: resp.read(CHUNK), b""):
                out.write(buf)
                done += len(buf)
                if time.time() - last > 0.5:
                    prog.update(0.03, f"runtime: downloading LTX-2 source {c['commit'][:7]}: {human(done)}")
                    last = time.time()
        with tarfile.open(tgz) as t:
            top = t.getmembers()[0].name.split("/")[0]
            t.extractall(rt)  # GitHub archive of a pinned commit
        os.rename(f"{rt}/{top}", src)
        os.unlink(tgz)
        with open(marker, "w") as f:
            f.write(c["commit"])
        log.info(f"LTX-2 source {c['commit']} unpacked")
    # Lightricks' repo has no lockfile; ours pins the exact tested environment (manifest note).
    shutil.copyfile(os.path.join(HERE, c["lockfile"]), f"{src}/uv.lock")
    # uv's own Python at the tested version, never a system one: LTX-2 only asks for >= 3.10, so a
    # system Python 3.12 got used on one user's machine, and its missing headers (no -devel
    # package) broke Triton's C build at the prompt cache step. uv's Python ships its headers.
    cache_bytes = man["runtime"]["uv_cache_bytes_darwin" if sys.platform == "darwin" else "uv_cache_bytes"]
    run([uv, "sync", "--frozen", "--no-dev", "--extra", "natten", "--managed-python",
         "--python", man["runtime"]["python"]], env, "runtime: Python packages",
        prog, 0.05, cwd=src, retries=2, watch=(f"{rt}/uv-cache", cache_bytes, 0.08))
    py = f"{src}/.venv/bin/python"
    run([uv, "pip", "install", "--python", py, *man["pip_extra"]], env, "runtime: installing OpenColorIO",
        prog, 0.08, retries=2)
    if sys.platform == "darwin":  # decision 65: PyTorch-MPS; the check touches no GPU memory
        run([py, "-I", "-B", "-c",
             "import os, sys, sysconfig, torch, PyOpenColorIO, ltx_pipelines.ic_lora; "
             "assert torch.backends.mps.is_available(), 'no MPS'; "
             "h = os.path.join(sysconfig.get_paths()['include'], 'Python.h'); "
             "assert os.path.exists(h), 'no Python headers at ' + h + ' (mps-sdpa needs them)'; "
             "print(sys.version.split()[0], torch.__version__, 'MPS')"],
            env, "runtime: checking Python, torch + MPS", prog, 0.09)
        return py
    run([py, "-I", "-B", "-c",
         "import os, sys, sysconfig, torch, PyOpenColorIO, ltx_pipelines.ic_lora; "
         "assert torch.cuda.is_available(), 'no CUDA'; "
         "h = os.path.join(sysconfig.get_paths()['include'], 'Python.h'); "
         "assert os.path.exists(h), 'no Python headers at ' + h + ' (Triton needs them)'; "
         "print(sys.version.split()[0], torch.__version__, torch.version.cuda, torch.cuda.get_device_name(0))"],
        env, "runtime: checking Python, torch + CUDA", prog, 0.09)
    return py


def gather_models(man: dict, model_dir: str, existing: str, token: str, prog: Progress,
                  link: bool = False) -> dict:
    files = man["files"]
    total = sum(f["size"] for f in files)
    base, done = 0.10, 0
    summary = {"kept": [], "copied": [], "linked": [], "downloaded": []}
    for entry in files:
        dst = os.path.join(model_dir, entry["path"])
        name = os.path.basename(entry["path"])
        f0 = base + 0.80 * done / total
        f1 = base + 0.80 * (done + entry["size"]) / total
        label = name
        if os.path.exists(dst) and os.path.getsize(dst) == entry["size"]:
            if already_verified(dst, entry) or sha256_file(dst, prog, label, f0, f1) == entry["sha256"]:
                mark_verified(dst, entry)
                summary["kept"].append(name)
                log.info(f"{name}: already in the Model Folder, verified")
                done += entry["size"]
                continue
            log.warn(f"{name}: in the Model Folder but the hash does not match; replacing it")
        source = None
        if existing:
            for cand in find_existing(existing, name, entry["size"]):
                if os.path.realpath(cand) == os.path.realpath(dst):
                    continue
                source = cand
                break
        if source and link:  # decision 66: verify in place, then point the Model Folder at it
            log.info(f"{name}: found {source}; verifying it to link instead of copying")
            if sha256_file(source, prog, f"{label} (link)", f0, f1) == entry["sha256"]:
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                if os.path.lexists(dst):  # a mismatching file or a dangling link from an earlier run
                    os.unlink(dst)
                os.symlink(os.path.abspath(source), dst)
                mark_verified(dst, entry)
                summary["linked"].append(f"{name} -> {source}")
                done += entry["size"]
                continue
            log.warn(f"{name}: the existing copy is a different version; downloading instead")
            source = None
        if source:
            log.info(f"{name}: found {source}; copying with verification")
            try:
                with open(source, "rb") as src:
                    stream_copy(src, dst, entry["size"], entry["sha256"], prog, f"{label} (copy)", f0, f1)
                mark_verified(dst, entry)
                summary["copied"].append(f"{name} from {source}")
                done += entry["size"]
                continue
            except SetupError as exc:
                log.warn(f"{name}: {exc}; the existing copy is a different version; downloading instead")
        if not token:
            raise SetupError(f"{name} is not in the Model Folder or the Existing Models folder, and no "
                             f"Hugging Face token was given to download it.")
        url = f"https://huggingface.co/{entry['repo']}/resolve/{entry['revision']}/{entry['hf_path']}"
        log.info(f"{name}: downloading {url}")
        download(url, token, dst, entry, prog, f"{label} (download)", f0, f1, entry["repo"])
        mark_verified(dst, entry)
        summary["downloaded"].append(name)
        done += entry["size"]
    return summary


FP16_MIN_SPEEDUP = 1.2  # below this the extra 42 GB is not worth it (M2-M4 have native bf16)


def make_fp16_copy(py: str, man: dict, model_dir: str, prog: Progress) -> str:
    """Decision 67, macOS only: "1" when the fp16 copy of the transformer exists and is faster
    here, else "0". Errors are logged; the plugin then simply runs bf16."""
    worker = os.path.join(WORKER_DIR, "ltx_worker.py")
    entry = next(e for e in man["files"] if e["key"] == "transformer")
    src = os.path.join(model_dir, entry["path"])
    dst = src.replace("-bf16.safetensors", "-fp16.safetensors")
    try:
        prog.update(0.90, "fp16: checking whether it is faster on this Mac")
        out = subprocess.run([py, "-I", "-B", worker, "--fp16-probe"], capture_output=True, text=True,
                             timeout=300).stdout
        line = next(ln for ln in out.splitlines() if ln.startswith("fp16_probe "))
        bf16_ms, fp16_ms = (float(v) for v in line.split()[1:3])
        speedup = bf16_ms / max(fp16_ms, 1e-6)
        log.info(f"fp16 probe: bf16 {bf16_ms:.1f} ms, fp16 {fp16_ms:.1f} ms per matmul -> {speedup:.2f}x")
        if speedup < FP16_MIN_SPEEDUP:
            log.info(f"fp16: not faster enough here (< {FP16_MIN_SPEEDUP}x); staying on bf16, no copy")
            return "0"
        source = {"sha256": entry["sha256"]}  # the marker ties the copy to the bf16 file it came from
        if os.path.exists(dst) and already_verified(dst, source):
            log.info(f"fp16: copy already there and current: {dst}")
            return "1"
        run([py, "-I", "-B", worker, "--fp16-convert", src, dst], dict(os.environ),
            "fp16: making a faster copy of the transformer (42 GB, one time)", prog, 0.90,
            watch=(os.path.join(os.path.dirname(dst), "fp16-tmp"), entry["size"], 0.92))
        mark_verified(dst, source)
        log.info(f"fp16: copy written: {dst} ({speedup:.2f}x faster matmuls here)")
        return "1"
    except Cancelled:
        raise
    except (SetupError, OSError, subprocess.SubprocessError, StopIteration, ValueError) as exc:
        log.warn(f"fp16: skipped ({exc}); the plugin runs bf16")
        return "0"


def write_config(updates: dict) -> None:
    cfg: dict = {}
    if os.path.exists(CONFIG):
        with open(CONFIG) as f:
            for line in f:
                line = line.rstrip("\n")
                if line and not line.startswith("#") and "=" in line:
                    k, v = line.split("=", 1)
                    cfg[k] = v
    cfg.update(updates)
    os.makedirs(os.path.dirname(CONFIG), exist_ok=True)
    tmp = CONFIG + ".tmp"
    with open(tmp, "w") as f:
        f.write("# LTX Alpha Gen OFX user settings. Written by the plugin.\n")
        for k in sorted(cfg):
            f.write(f"{k}={cfg[k]}\n")
    os.replace(tmp, CONFIG)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--model-dir", required=True)
    ap.add_argument("--cache-root", required=True)
    ap.add_argument("--existing", default="")
    ap.add_argument("--link", action="store_true", help="symlink files from --existing instead of copying")
    ap.add_argument("--skip-runtime", action="store_true", help="tests: reuse --ltx-python instead")
    ap.add_argument("--ltx-python", default="")
    ap.add_argument("--no-ui", action="store_true")
    a = ap.parse_args()
    token = os.environ.pop("HF_TOKEN", "")
    ui = not a.no_ui
    with open(os.path.join(HERE, "model_manifest.json")) as f:
        man = json.load(f)
    model_dir = os.path.abspath(os.path.expanduser(a.model_dir))
    log.info(f"setup start: model_dir={model_dir} existing={a.existing or '-'} link={a.link} cache_root={a.cache_root} "
             f"token={'given' if token else 'none'} (length {len(token)})")
    prog = Progress(ui)
    t0 = time.time()
    try:
        cc = find_c_compiler()
        if not cc and sys.platform == "darwin":
            raise SetupError("The Apple Command Line Tools are missing. The GPU code compiles a small "
                             "helper while it runs, so they are needed: run xcode-select --install in "
                             "Terminal. Then press Download again.")
        if not cc:  # before any download: the prompt cache step would fail after ~80 GB otherwise
            raise SetupError("No C compiler found. The GPU code (Triton) compiles small helpers while it "
                             "runs, so gcc is needed: sudo dnf install gcc. Then press Download again.")
        log.info(f"C compiler for Triton: {cc}")
        os.makedirs(model_dir, exist_ok=True)
        py = a.ltx_python if a.skip_runtime else install_runtime(man, os.path.join(model_dir, "runtime"), prog)
        summary = gather_models(man, model_dir, a.existing, token, prog, link=a.link)
        fp16_ready = make_fp16_copy(py, man, model_dir, prog) if sys.platform == "darwin" else None
        emb = os.path.join(a.cache_root, "prompt-empty.safetensors")
        worker = os.path.join(WORKER_DIR, "ltx_worker.py")
        m = {e["key"]: os.path.join(model_dir, e["path"]) for e in man["files"]}
        run([py, "-I", "-B", worker, "--build-embeddings", emb, "--transformer", m["transformer"],
             "--text-encoder", m["text_encoder"], "--video-vae", m["video_vae"]],
            dict(os.environ), "building the prompt cache (loads Gemma once)", prog, 0.92)
        cfg = {"model_dir": model_dir, "ltx_python": py, "runtime_commit": man["ltx2"]["commit"],
               "models_ready": time.strftime("%Y-%m-%d")}
        if fp16_ready is not None:
            cfg["transformer_fp16"] = fp16_ready
        write_config(cfg)
        prog.close()
        msg = (f"Setup finished in {time.time() - t0:.0f} s. Kept {len(summary['kept'])}, copied "
               f"{len(summary['copied'])}, linked {len(summary['linked'])}, downloaded "
               f"{len(summary['downloaded'])} file(s)."
               + (" fp16 copy in use: faster on this Mac (+42 GB)." if fp16_ready == "1" else ""))
        log.info(msg + f" copied: {summary['copied']} linked: {summary['linked']} downloaded: {summary['downloaded']}")
        Progress.message("info", msg, ui)
        return 0
    except Cancelled:
        prog.close()
        log.warn("setup cancelled by the user; partial files (*.part) removed on the next run")
        return 1
    except (SetupError, OSError, subprocess.SubprocessError) as exc:
        prog.close()
        log.error(f"setup failed: {exc}")
        Progress.message("error", f"Setup failed: {exc}", ui)
        return 2


if __name__ == "__main__":
    sys.exit(main())
