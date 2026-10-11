"""Resident worker + OCIO end-to-end test, as the plugin's Process runs it (no Flame).

  <LTX venv python> resident_smoke.py MODEL_DIR EMBEDDINGS

Runs job.py --no-ui several times against the real worker in serve mode and checks:
  1 unload=off starts a server that stays alive        5 Flame (pid) death -> server exits
  2 a 2nd job reuses it (and is faster)                 6 killing the server mid-job -> job fails cleanly
  3 bf16 + unload=on replaces it, then it exits, GPU freed
  4 idle timeout (LTXAG_IDLE_TIMEOUT=15) -> server exits
  7 colour: frames made by INVERTING the ACES 2.0 view; worker's forward conversion must give a
    correct matte again.
Every job's mattes are compared with the ground truth. Uses its own run dir and cache root under
/dev/shm. Exit 0 = all pass.
"""

from __future__ import annotations

import json
import os
import shutil
import signal
import subprocess
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
WORKER_DIR = os.path.join(HERE, "..", "worker")
sys.path.insert(0, WORKER_DIR)
sys.path.insert(0, HERE)

from ltxag_worker import cache, colour, transport  # noqa: E402
from worker_smoke import make_frames  # noqa: E402

W, H, N = 960, 540, 49
RUN = f"/dev/shm/ltxag_resident_test_{os.getpid()}"
ROOT = f"/dev/shm/ltxag_resident_cache_{os.getpid()}"
SOCK = f"{RUN}/worker.sock"
results: list[tuple[str, bool, str]] = []


def check(name: str, ok: bool, detail: str = "") -> None:
    results.append((name, ok, detail))
    print(("PASS " if ok else "FAIL ") + name + (f"  ({detail})" if detail else ""), flush=True)


def info() -> dict:
    try:
        with open(SOCK + ".json") as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError):
        return {}


def alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
        return True
    except (ProcessLookupError, TypeError):
        return False


def wait_until(cond, seconds: float) -> bool:
    end = time.time() + seconds
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.25)
    return cond()


def gpu_procs() -> set[int]:
    out = subprocess.run(["nvidia-smi", "--query-compute-apps=pid", "--format=csv,noheader"],
                         capture_output=True, text=True).stdout
    return {int(x) for x in out.split() if x.strip().isdigit()}


def cmdline(pid: int) -> str:
    try:
        with open(f"/proc/{pid}/cmdline", "rb") as f:
            return f.read().replace(b"\0", b" ").decode(errors="replace")
    except OSError:
        return ""


def run_job(tag: str, frames: np.ndarray, truth: np.ndarray, precision: str, unload: bool,
            colour_label: str = "Bypass", flame_pid: int = 0, env_extra: dict | None = None,
            background: bool = False):
    job_id = f"rt-{os.getpid()}-{tag}"
    inp = f"/dev/shm/ltxag_job_{job_id}_in.bin"
    with open(inp, "wb") as f:
        f.write(transport.HEADER.pack(transport.IN_MAGIC, transport.VERSION, W, H, N, 3))
        frames.astype("<f2").tofile(f)
    keys = [f"{tag[:4]:0<4}{i:012x}" for i in range(N)]
    m = {"job_id": job_id, "node_id": "rtest", "input": inp, "width": W, "height": H, "frames": N,
         "chunks": [[0, N]], "frame_keys": keys, "contrib": [[[0, 1.0]]] * N, "cache_root": ROOT,
         "worker": os.path.join(WORKER_DIR, "ltx_worker.py"), "worker_python": sys.executable,
         "worker_args": ["--model-dir", MODEL_DIR, "--embeddings", EMB], "worker_socket": SOCK,
         "model_dir": MODEL_DIR, "unload": unload, "flame_pid": flame_pid, "colour": colour_label,
         "ocio_config": "", "seed": 1234, "seconds": 0, "simulate": "none", "precision": precision,
         "title": tag}
    mpath = f"/dev/shm/ltxag_job_{job_id}.json"
    with open(mpath, "w") as f:
        json.dump(m, f)
    env = dict(os.environ, **(env_extra or {}))
    cmd = [sys.executable, "-I", "-B", os.path.join(WORKER_DIR, "job.py"), mpath, "--no-ui"]
    if background:
        return subprocess.Popen(cmd, env=env), job_id, keys
    t0 = time.time()
    rc = subprocess.run(cmd, env=env).returncode
    return rc, time.time() - t0, job_id, keys


def matte_error(keys, truth) -> float:
    errs = []
    for i, k in enumerate(keys):
        p = cache.matte_path(ROOT, k)
        if not os.path.exists(p):
            return 1.0
        with open(p, "rb") as f:
            f.seek(transport.HEADER.size)
            errs.append(np.abs(np.fromfile(f, dtype="<f2").astype(np.float32).reshape(H, W) - truth[i]).mean())
    return float(np.max(errs))


def status(job_id: str) -> dict:
    with open(os.path.join(ROOT, "jobs", job_id + ".json")) as f:
        return json.load(f)


def main() -> int:
    global MODEL_DIR, EMB
    MODEL_DIR, EMB = sys.argv[1], sys.argv[2]
    os.makedirs(RUN, exist_ok=True)
    frames, truth = make_frames(W, H, N)

    # 1 start + stay
    rc, t1, jid, keys = run_job("one", frames, truth, "fp8", unload=False)
    i1 = info()
    check("1 resident server starts and stays", rc == 0 and alive(i1.get("pid")) and i1.get("state") == "ready",
          f"rc={rc} {t1:.1f}s pid={i1.get('pid')} state={i1.get('state')} vram={i1.get('vram_gb')}GB")
    check("1 matte correct", matte_error(keys, truth) < 0.02, f"max err {matte_error(keys, truth):.4f}")

    # 2 reuse
    rc, t2, jid, keys = run_job("two", frames, truth, "fp8", unload=False)
    i2 = info()
    check("2 second job reuses the same server", rc == 0 and i2.get("pid") == i1.get("pid"),
          f"pid {i1.get('pid')} -> {i2.get('pid')}")
    check("2 reuse is faster", t2 < t1, f"{t1:.1f}s -> {t2:.1f}s")
    check("2 matte correct", matte_error(keys, truth) < 0.02, f"max err {matte_error(keys, truth):.4f}")

    # 3 restart for bf16, unload
    old_pid = i2.get("pid")
    rc, t3, jid, keys = run_job("three", frames, truth, "bf16", unload=True)
    gone = wait_until(lambda: not os.path.exists(SOCK + ".json"), 30)
    check("3 precision change replaced the server", rc == 0 and not alive(old_pid), f"rc={rc} {t3:.1f}s")
    check("3 unload: server exited and socket removed", gone)
    time.sleep(1)
    workers_on_gpu = [p for p in gpu_procs() if "ltx_worker" in cmdline(p)]
    check("3 GPU memory returned (no ltx_worker on the GPU)", not workers_on_gpu, f"{workers_on_gpu}")
    check("3 matte correct", matte_error(keys, truth) < 0.02, f"max err {matte_error(keys, truth):.4f}")

    # 4 idle timeout
    rc, _, jid, keys = run_job("four", frames, truth, "fp8", unload=False, env_extra={"LTXAG_IDLE_TIMEOUT": "15"})
    pid4 = info().get("pid")
    exited = wait_until(lambda: not alive(pid4), 40)
    check("4 idle timeout ends the resident server", rc == 0 and exited, f"pid {pid4}")

    # 5 Flame death
    fake_flame = subprocess.Popen(["sleep", "600"])
    rc, _, jid, keys = run_job("five", frames, truth, "fp8", unload=False, flame_pid=fake_flame.pid)
    pid5 = info().get("pid")
    fake_flame.kill()
    fake_flame.wait()
    check("5 server exits when Flame dies", rc == 0 and wait_until(lambda: not alive(pid5), 10), f"pid {pid5}")

    # 6 crash mid-job
    proc, jid6, keys6 = run_job("six", frames, truth, "fp8", unload=False, background=True)
    wait_until(lambda: info().get("state") == "busy", 120)
    pid6 = info().get("pid")
    time.sleep(3)
    if pid6:
        os.killpg(pid6, signal.SIGKILL)
    proc.wait(timeout=120)
    st = status(jid6)
    check("6 server killed mid-job -> job failed with a clear message",
          proc.returncode == 1 and st["state"] == "failed" and "unexpectedly" in st["message"], st["message"])
    leftovers = [x for x in os.listdir("/dev/shm") if x.startswith(f"ltxag_job_{jid6}")]
    check("6 no shm leftovers", not leftovers, str(leftovers))

    # 7 colour: invert the view so the worker's forward conversion restores the picture
    import PyOpenColorIO as ocio

    cfg = ocio.Config.CreateFromFile(colour.default_config())
    t = ocio.DisplayViewTransform()
    t.setSrc("ACEScg")
    t.setDisplay(colour.DISPLAY)
    t.setView(colour.VIEW)
    inv = cfg.getProcessor(t, ocio.TRANSFORM_DIR_INVERSE).getDefaultCPUProcessor()
    acescg = frames.astype(np.float32).copy()
    inv.applyRGB(acescg.reshape(-1, 3))
    back = colour.Converter("ACEScg").apply(acescg)
    check("7 forward(inverse(x)) round trip", float(np.abs(back - frames.astype(np.float32)).max()) < 0.01,
          f"max diff {float(np.abs(back - frames.astype(np.float32)).max()):.4f}")
    rc, _, jid, keys = run_job("seven", acescg.astype(np.float16), truth, "fp8", unload=True, colour_label="ACEScg")
    err7 = matte_error(keys, truth)
    check("7 ACEScg input converted by the worker -> correct matte", rc == 0 and err7 < 0.02, f"max err {err7:.4f}")

    shutil.rmtree(ROOT, ignore_errors=True)
    shutil.rmtree(RUN, ignore_errors=True)
    failed = [r for r in results if not r[1]]
    print(f"\n{len(results) - len(failed)}/{len(results)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
