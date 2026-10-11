"""job.py + real worker end to end, as the plugin's Process button runs it (no Flame).

  python3 job_real_smoke.py LTX_PYTHON MODEL_DIR [W H N PRECISION]

Plans chunks and crossfade weights exactly like ofx/src/chunk_plan.cpp (145 frames, 24
overlap), stages frames, writes the manifest, runs job.py --no-ui with this interpreter as the
job controller and LTX_PYTHON for the worker, then checks every cached matte against truth.
Uses a throwaway cache root under /dev/shm. Exit 0 = pass.
"""

from __future__ import annotations

import json
import math
import os
import shutil
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "worker"))
sys.path.insert(0, HERE)

from ltxag_worker import cache, transport  # noqa: E402
from worker_smoke import make_frames  # noqa: E402

L, O = 145, 24


def plan(n: int) -> list[tuple[int, int]]:
    if n <= L:
        return [(0, n)]
    k = math.ceil((n - L) / (L - O)) + 1
    return [(round(j * (n - L) / (k - 1)), L) for j in range(k)]


def contributions(chunks, i):  # mirrors frameContributions()
    if len(chunks) == 1:
        return [[0, 1.0]]
    owner = 0
    for j in range(len(chunks) - 1):
        (sa, la), (sb, _) = chunks[j], chunks[j + 1]
        end_a = sa + la - 1
        mid = 0.5 * (sb + end_a)
        band = max(1, min(O, end_a - sb + 1))
        w = min(1.0, max(0.0, (i - (mid - 0.5 * band)) / band))
        if 0.0 < w < 1.0:
            return [[j, 1.0 - w], [j + 1, w]]
        if w >= 1.0:
            owner = j + 1
    return [[owner, 1.0]]


def main() -> int:
    ltx_python, model_dir = sys.argv[1:3]
    w, h, n = (int(v) for v in (sys.argv[3:6] if len(sys.argv) > 5 else (960, 540, 200)))
    precision = sys.argv[6] if len(sys.argv) > 6 else "fp8"
    frames, truth = make_frames(w, h, n)
    job_id = f"smoke-{os.getpid()}"
    root = f"/dev/shm/ltxag_smoke_cache_{os.getpid()}"
    inp = f"/dev/shm/ltxag_job_{job_id}_in.bin"
    with open(inp, "wb") as f:
        f.write(transport.HEADER.pack(transport.IN_MAGIC, transport.VERSION, w, h, n, 3))
        frames.astype("<f2").tofile(f)
    chunks = plan(n)
    keys = [f"{i:016x}" for i in range(n)]
    manifest = {
        "job_id": job_id, "node_id": "smoke", "input": inp, "width": w, "height": h, "frames": n,
        "chunks": [list(c) for c in chunks], "frame_keys": keys,
        "contrib": [contributions(chunks, i) for i in range(n)], "cache_root": root,
        "worker": os.path.join(HERE, "..", "worker", "ltx_worker.py"), "worker_python": ltx_python,
        "worker_args": ["--model-dir", model_dir, "--embeddings",
                        "/Volumes/Data2/ltx-alphagen/cache/prompt-empty.safetensors"],
        "seconds": 0, "simulate": "none", "precision": precision, "title": "smoke",
    }
    mpath = f"/dev/shm/ltxag_job_{job_id}.json"
    with open(mpath, "w") as f:
        json.dump(manifest, f)
    print(f"{n} frames {w}x{h}, chunks {chunks}, precision {precision}", flush=True)
    rc = subprocess.run([sys.executable, "-I", "-B", os.path.join(HERE, "..", "worker", "job.py"), mpath,
                         "--no-ui"]).returncode
    with open(os.path.join(root, "jobs", job_id + ".json")) as f:
        status = json.load(f)
    print("job exit", rc, "status", status, flush=True)
    ok = rc == 0 and status["state"] == "done"
    errs = []
    for i, k in enumerate(keys):
        p = cache.matte_path(root, k)
        if not os.path.exists(p):
            print(f"FAIL: frame {i} not cached")
            ok = False
            continue
        with open(p, "rb") as f:
            f.seek(transport.HEADER.size)
            m = np.fromfile(f, dtype="<f2").astype(np.float32).reshape(h, w)
        errs.append(np.abs(m - truth[i]).mean())
    leftovers = [x for x in os.listdir("/dev/shm") if x.startswith(f"ltxag_job_{job_id}")]
    if leftovers:
        print("FAIL: leftovers", leftovers)
        ok = False
    if errs:
        e = np.array(errs)
        print(f"per-frame mean abs error: mean {e.mean():.4f}, max {e.max():.4f} at frame {int(e.argmax())}")
        ok = ok and e.max() < 0.02
    shutil.rmtree(root, ignore_errors=True)
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
