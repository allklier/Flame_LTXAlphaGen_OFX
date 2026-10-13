# /// script
# requires-python = ">=3.10"
# dependencies = ["torch==2.14.1", "mps-sdpa==0.2.0", "mlx==0.32.3", "numpy"]
# ///
"""LTX Alpha Gen: Apple GPU benchmark (PyTorch-MPS vs MLX, fp16 / bf16 / fp32).

Measures the two operations that take most of LTX Alpha Gen's time, at the model's real sizes:
matrix multiplies (the transformer's 4096- and 16384-wide layers) and attention (32 heads x 128,
one sequence per clip size). It answers whether an Apple GPU, especially an M5 with its Neural
Accelerators, runs these faster in fp16 than in bf16, and whether PyTorch reaches that speed or
only MLX does.

Run:

    python3 mac_bench.py

Any python3 starts it. PyTorch and MLX need Python 3.10 or newer; if this one is older (Apple's
3.9 from the Command Line Tools), the script uses Flame's own Python
(/opt/Autodesk/python/<version>/bin/python3.13) instead. The first run creates a Python
environment in ~/Library/Caches/ltx-alphagen-bench and installs PyTorch and MLX into it with
pip (about 200 MB download, ~1 GB on disk, about a minute). Nothing is installed anywhere else;
delete that folder to remove everything. With uv, `uv run mac_bench.py` works too.

Takes a few minutes and about 15 GB of memory (less on Macs with under 64 GB: it then skips the
larger size). Prints a table and writes mac_bench_<chip>.json next to the script: please send
that file back. Close other GPU-heavy apps first.
"""

from __future__ import annotations

import glob
import importlib.util
import json
import os
import platform
import subprocess
import sys
import time

# Tokens the transformer sees per chunk (target + reference video tokens):
# 1280x720 x 49 frames and 1920x1080 x 33 frames.
SIZES = {"720p x 49": 12880, "HD x 33": 20400}
HIDDEN, FFN, HEADS, HEAD_DIM = 4096, 16384, 32, 128
REPEATS = 5

# The bootstrap (standard library only, so any python3 can start it): a venv with these pinned
# packages, made from a Python >= 3.10 (this one, else Flame's), then this script again inside it.
PACKAGES = ["torch==2.14.1", "mps-sdpa==0.2.0", "mlx==0.32.3", "numpy"]
WORK = os.path.expanduser("~/Library/Caches/ltx-alphagen-bench")


def base_python() -> str:
    if sys.version_info >= (3, 10):
        return sys.executable
    found = sorted(glob.glob("/opt/Autodesk/python/*/bin/python3.1[0-9]"))
    return found[-1] if found else ""


def bootstrap() -> int:
    venv_python = os.path.join(WORK, "venv", "bin", "python")
    if not os.path.exists(venv_python):
        py = base_python()
        if not py:
            print("PyTorch and MLX need Python 3.10 or newer, and no Flame Python was found in "
                  "/opt/Autodesk/python. Run this script with a newer python3.")
            return 1
        print(f"First run: setting up {WORK} with {py} (downloads about 200 MB, about a minute) ...", flush=True)
        os.makedirs(WORK, exist_ok=True)
        if subprocess.call([py, "-m", "venv", os.path.join(WORK, "venv")]) != 0:
            return 1
        # pip brings its own CA certificates (Flame's Python has none for urllib).
        if subprocess.call([venv_python, "-m", "pip", "install", "--quiet", "--disable-pip-version-check",
                            *PACKAGES]) != 0:
            print("Installing the packages failed; please send the output above.")
            return 1
    return subprocess.call([venv_python, os.path.abspath(__file__)], env=dict(os.environ, LTXAG_BENCH_CHILD="1"))


def sh(cmd: list[str]) -> str:
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=30).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ""


def system_info() -> dict:
    gpu = sh(["/usr/sbin/system_profiler", "SPDisplaysDataType"])
    cores = next((line.split(":")[1].strip() for line in gpu.splitlines() if "Total Number of Cores" in line), "?")
    return {"chip": sh(["/usr/sbin/sysctl", "-n", "machdep.cpu.brand_string"]),
            "gpu_cores": cores,
            "ram_gb": round(int(sh(["/usr/sbin/sysctl", "-n", "hw.memsize"]) or 0) / 2**30),
            "macos": platform.mac_ver()[0], "python": platform.python_version()}


def timed(fn, sync) -> float:
    """Seconds per call, best of REPEATS after one warm-up."""
    fn()
    sync()
    best = float("inf")
    for _ in range(REPEATS):
        t0 = time.perf_counter()
        fn()
        sync()
        best = min(best, time.perf_counter() - t0)
    return best


def bench_torch(tokens: int) -> dict:
    import torch
    from mps_sdpa import sdpa_opt

    dev, out = torch.device("mps"), {}
    dtypes = {"fp16": torch.float16, "bf16": torch.bfloat16, "fp32": torch.float32}
    for name, dt in dtypes.items():
        x = torch.randn(tokens, HIDDEN, device=dev, dtype=dt)
        w1 = torch.randn(HIDDEN, HIDDEN, device=dev, dtype=dt)
        w2 = torch.randn(HIDDEN, FFN, device=dev, dtype=dt)
        mm = 2 * tokens * HIDDEN * (HIDDEN + FFN)
        s = timed(lambda: (x @ w1, x @ w2), torch.mps.synchronize)
        out[f"matmul {name}"] = mm / s / 1e12
        q, k, v = (torch.randn(1, HEADS, tokens, HEAD_DIM, device=dev, dtype=dt) for _ in range(3))
        s = timed(lambda: sdpa_opt(q, k, v), torch.mps.synchronize)
        out[f"attention {name}"] = 4 * HEADS * tokens * tokens * HEAD_DIM / s / 1e12
    return out


def bench_mlx(tokens: int) -> dict:
    import mlx.core as mx

    out = {}
    dtypes = {"fp16": mx.float16, "bf16": mx.bfloat16, "fp32": mx.float32}
    for name, dt in dtypes.items():
        x = mx.random.normal((tokens, HIDDEN)).astype(dt)
        w1 = mx.random.normal((HIDDEN, HIDDEN)).astype(dt)
        w2 = mx.random.normal((HIDDEN, FFN)).astype(dt)
        mx.eval(x, w1, w2)
        mm = 2 * tokens * HIDDEN * (HIDDEN + FFN)
        s = timed(lambda: mx.eval(mx.matmul(x, w1), mx.matmul(x, w2)), lambda: None)
        out[f"matmul {name}"] = mm / s / 1e12
        q, k, v = (mx.random.normal((1, HEADS, tokens, HEAD_DIM)).astype(dt) for _ in range(3))
        mx.eval(q, k, v)
        scale = HEAD_DIM ** -0.5
        s = timed(lambda: mx.eval(mx.fast.scaled_dot_product_attention(q, k, v, scale=scale)), lambda: None)
        out[f"attention {name}"] = 4 * HEADS * tokens * tokens * HEAD_DIM / s / 1e12
    return out


def main() -> int:
    if sys.platform != "darwin" or platform.machine() != "arm64":
        print("This benchmark is for Macs with Apple Silicon.")
        return 1
    if not all(importlib.util.find_spec(m) for m in ("torch", "mlx", "mps_sdpa")):
        if os.environ.get("LTXAG_BENCH_CHILD"):
            print("The packages did not install; please send the output above.")
            return 1
        return bootstrap()
    info = system_info()
    sizes = dict(SIZES) if info["ram_gb"] >= 64 else {"720p x 49": SIZES["720p x 49"]}
    print(f"{info['chip']}, {info['gpu_cores']} GPU cores, {info['ram_gb']} GB, macOS {info['macos']}", flush=True)
    results = {}
    for label, tokens in sizes.items():
        for fw, fn in (("pytorch", bench_torch), ("mlx", bench_mlx)):
            print(f"  {label}: {fw} ...", flush=True)
            try:
                results[f"{label} | {fw}"] = fn(tokens)
            except Exception as exc:  # noqa: BLE001 - report and carry on with the other framework
                results[f"{label} | {fw}"] = {"error": f"{type(exc).__name__}: {exc}"}
    import mlx.core as mx
    import torch

    info.update(torch=torch.__version__, mlx=mx.__version__)
    print(f"\nTFLOPS (higher is better), torch {info['torch']}, mlx {info['mlx']}:")
    cols = ["matmul fp16", "matmul bf16", "matmul fp32", "attention fp16", "attention bf16", "attention fp32"]
    print(f"{'':28s}" + "".join(f"{c:>16s}" for c in cols))
    for key, r in results.items():
        if "error" in r:
            print(f"{key:28s}  {r['error']}")
        else:
            print(f"{key:28s}" + "".join(f"{r[c]:16.1f}" for c in cols))
    chip = "".join(ch for ch in info["chip"].replace(" ", "_") if ch.isalnum() or ch == "_") or "mac"
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), f"mac_bench_{chip}.json")
    with open(path, "w") as f:
        json.dump({"system": info, "results": results}, f, indent=1)
    print(f"\nWrote {path}: please send this file back.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
