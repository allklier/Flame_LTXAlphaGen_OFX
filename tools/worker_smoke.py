"""End-to-end smoke test of worker/ltx_worker.py without Flame (build step 3/4).

  python worker_smoke.py WORKER_PY MODEL_DIR EMBEDDINGS [W H N PRECISION CHUNKS]

Generates a synthetic clip (soft fuzzy disc over texture, like make_test_clip.py), writes it
in the shim's /dev/shm input format, runs the worker exactly as job.py would, then checks the
mattes: right count and size, high inside the disc, low outside. Saves the middle frame as
/dev/shm/ltxag_smoke_matte.png for a visual check. Exit 0 = pass.
CHUNKS like "0:65" (default: one chunk of all frames).
"""

from __future__ import annotations

import os
import subprocess
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "worker"))

from ltxag_worker import transport  # noqa: E402


def make_frames(w: int, h: int, n: int) -> tuple[np.ndarray, np.ndarray]:
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    noise = np.random.default_rng(0).random((h, w), dtype=np.float32)
    bg = np.stack([0.25 + 0.2 * np.sin(xx / 37.0) + 0.1 * noise, 0.35 + 0.2 * np.cos(yy / 53.0),
                   0.30 + 0.1 * noise], axis=-1)
    frames = np.empty((n, h, w, 3), np.float16)
    truth = np.empty((n, h, w), np.float32)
    r = 0.22 * min(w, h)
    for f in range(n):
        t = f / max(1, n - 1)
        cx, cy = w * (0.25 + 0.5 * t), h * (0.5 + 0.15 * np.sin(6.28 * t))
        d = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2)
        fringe = 0.5 + 0.5 * np.sin(np.arctan2(yy - cy, xx - cx) * 40.0)
        a = np.clip((r + 0.06 * r * fringe - d) / (0.02 * r), 0.0, 1.0)
        truth[f] = a
        frames[f] = bg * (1 - a[..., None]) + np.array([0.85, 0.55, 0.40], np.float32) * a[..., None]
    return frames, truth


def main() -> int:
    worker_py, model_dir, emb = sys.argv[1:4]
    w, h, n = (int(v) for v in (sys.argv[4:7] if len(sys.argv) > 6 else (1920, 1080, 60)))
    precision = sys.argv[7] if len(sys.argv) > 7 else "bf16"
    chunks = sys.argv[8] if len(sys.argv) > 8 else f"0:{n}"
    frames, truth = make_frames(w, h, n)
    inp = f"/dev/shm/ltxag_smoke_{os.getpid()}_in.bin"
    prefix = f"/dev/shm/ltxag_smoke_{os.getpid()}_out"
    with open(inp, "wb") as f:
        f.write(transport.HEADER.pack(transport.IN_MAGIC, transport.VERSION, w, h, n, 3))
        frames.astype("<f2").tofile(f)
    cmd = [worker_py, "-I", "-B", os.path.join(HERE, "..", "worker", "ltx_worker.py"), "--in", inp,
           "--chunks", chunks, "--out-prefix", prefix, "--model-dir", model_dir,
           "--embeddings", emb, "--precision", precision]
    print("running:", " ".join(cmd), flush=True)
    t0 = time.time()
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True)
    done, chunk_ids = False, []
    for line in proc.stdout:
        line = line.rstrip("\n")
        if not line.startswith("progress"):
            print("  worker:", line, flush=True)
        if line == "done":
            done = True
        elif line.startswith("chunk_done "):
            chunk_ids.append(int(line.split()[1]))
    rc = proc.wait()
    os.unlink(inp)
    print(f"worker exit {rc} after {time.time() - t0:.1f} s", flush=True)
    ok = done and rc == 0
    spans = [tuple(int(v) for v in c.split(":")) for c in chunks.split(",")]
    for i, (s, ln) in enumerate(spans):
        path = f"{prefix}_{i}.bin"
        if not os.path.exists(path):
            print(f"FAIL: chunk {i} output missing")
            ok = False
            continue
        with open(path, "rb") as f:
            hdr = transport.HEADER.unpack(f.read(transport.HEADER.size))
            mat = np.fromfile(f, dtype="<f2").astype(np.float32).reshape(ln, h, w)
        os.unlink(path)
        if hdr[2:5] != (w, h, ln):
            print(f"FAIL: chunk {i} header {hdr}")
            ok = False
        t = truth[s : s + ln]
        inside, outside = mat[t > 0.99].mean(), mat[t < 0.01].mean()
        err = np.abs(mat - t).mean()
        print(f"chunk {i}: inside mean {inside:.3f}, outside mean {outside:.3f}, mean abs error vs truth "
              f"{err:.4f}", flush=True)
        if not (inside > 0.8 and outside < 0.1):
            print(f"FAIL: chunk {i} matte does not separate the subject")
            ok = False
        if i == 0:
            try:
                from PIL import Image

                mid = ln // 2
                Image.fromarray((np.clip(mat[mid], 0, 1) * 255).astype(np.uint8)).resize(
                    (w // 4, h // 4)).save("/dev/shm/ltxag_smoke_matte.png")
            except ImportError:
                pass
    print("PASS" if ok else "FAIL", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
