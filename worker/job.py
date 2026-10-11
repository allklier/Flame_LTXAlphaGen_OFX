"""Background processing job (decision 35). Launched detached by the OFX Process button.

  python -I -B job.py <manifest.json> [--no-ui]

The shim has already staged every source frame in /dev/shm and computed each frame's
cache key and crossfade contributions. This process, outside Flame:
  - shows its own progress window with a Cancel button (zenity),
  - runs the worker once for all chunks (the real model will load once per job),
  - blends overlapping chunks and writes one matte per frame into the disk cache,
  - always cleans up its /dev/shm files and pending markers, and records a final status.
Manifest fields: job_id node_id input width height frames chunks[[start,len]] frame_keys[]
contrib[[[chunk,weight],...]] cache_root worker [worker_python] [worker_args[]] seconds simulate
precision title.
Status file <cache_root>/jobs/<job_id>.json is rewritten after every chunk (state "running")
and at the end, so the plugin can follow progress.
"""

from __future__ import annotations

import json
import os
import queue
import shutil
import signal
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402

from ltxag_worker import cache, transport  # noqa: E402
from ltxag_worker.log import Logger  # noqa: E402

log = Logger("job")


class Progress:
    """zenity --progress window; cancelled() is True once the user pressed Cancel."""

    def __init__(self, title: str, enabled: bool) -> None:
        self.proc = None
        self.finished = False
        zenity = shutil.which("zenity") if enabled else None
        if zenity:
            self.proc = subprocess.Popen(
                [zenity, "--progress", f"--title={title}", "--text=Starting...",
                 "--percentage=0", "--auto-close", "--width=420"],
                stdin=subprocess.PIPE, text=True)

    def update(self, frac: float, text: str) -> None:
        if not self.proc or self.proc.poll() is not None:
            return
        pct = max(0, min(99, int(frac * 100)))  # 100 auto-closes; sent only by close()
        try:
            self.proc.stdin.write(f"# {text}\n{pct}\n")
            self.proc.stdin.flush()
        except BrokenPipeError:
            pass

    def cancelled(self) -> bool:
        return bool(self.proc) and not self.finished and self.proc.poll() is not None

    def close(self) -> None:
        self.finished = True
        if self.proc and self.proc.poll() is None:
            try:
                self.proc.stdin.write("100\n")
                self.proc.stdin.close()
            except BrokenPipeError:
                pass
            self.proc.wait(timeout=5)

    @staticmethod
    def error(title: str, text: str, enabled: bool) -> None:
        zenity = shutil.which("zenity") if enabled else None
        if zenity:
            subprocess.Popen([zenity, "--error", f"--title={title}", f"--text={text}", "--width=420"],
                             start_new_session=True)


def main() -> int:
    manifest_path = sys.argv[1]
    ui = "--no-ui" not in sys.argv
    with open(manifest_path) as f:
        m = json.load(f)
    job_id, root = m["job_id"], m["cache_root"]
    log.node, log.job = m.get("node_id", "-"), job_id
    keys, contrib, chunks = m["frame_keys"], m["contrib"], m["chunks"]
    out_prefix = f"/dev/shm/ltxag_job_{job_id}_out"
    pid_file = f"/dev/shm/ltxag_job_{job_id}.pid"
    with open(pid_file, "w") as f:
        f.write(str(os.getpid()))
    cache.mark_pending(root, keys, os.getpid())
    t_start = time.time()
    log.info(f"start: {m['frames']} frames {m['width']}x{m['height']}, chunks={chunks}, "
             f"precision={m['precision']} simulate={m['simulate']} sim_seconds={m['seconds']} "
             f"python={sys.executable} {sys.version.split()[0]} ui={ui}")
    cache.write_status(root, job_id, "running", "", chunks_done=0, chunks=len(chunks),
                       frames_done=0, frames=len(keys))

    progress = Progress(m.get("title", "LTX Alpha Gen"), ui)
    # The real worker runs in the LTX venv (worker_python); the dummy in this interpreter.
    cmd = [m.get("worker_python") or sys.executable, "-I", "-B", m["worker"], "--in", m["input"],
           "--chunks", ",".join(f"{s}:{n}" for s, n in chunks), "--out-prefix", out_prefix,
           "--seconds", str(m["seconds"]), "--simulate", m["simulate"], "--precision", m["precision"],
           *m.get("worker_args", [])]
    log.info("worker command: " + " ".join(cmd))
    worker = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True, start_new_session=True)
    log.info(f"worker pid {worker.pid}")
    t_chunk = time.time()
    lines: queue.Queue = queue.Queue()
    threading.Thread(target=lambda: [lines.put(line.rstrip("\n")) for line in worker.stdout],
                     daemon=True).start()

    results: dict[int, np.ndarray] = {}
    finished: set[int] = set()
    state, message = "failed", "worker exited without a result"

    def finalize(f: int) -> None:
        parts = contrib[f]
        if f in finished or any(c not in results for c, _ in parts):
            return
        matte = sum(w * results[c][f - chunks[c][0]].astype(np.float32) for c, w in parts)
        cache.write_matte(root, keys[f], matte)
        cache.clear_pending(root, [keys[f]])
        finished.add(f)

    while True:
        if progress.cancelled():
            log.info("cancel pressed in the progress window: killing worker")
            os.killpg(worker.pid, signal.SIGKILL)
            state, message = "cancelled", "cancelled by user"
            break
        try:
            line = lines.get(timeout=0.2)
        except queue.Empty:
            if worker.poll() is not None and lines.empty():
                break
            continue
        known = line.split(" ", 1)[0] in ("progress", "info", "chunk_done", "done", "error")
        if line.startswith("progress "):
            log.debug(f"worker: {line}")
        elif line.startswith("error "):
            log.error(f"worker: {line}")
        elif known:
            log.info(f"worker: {line}")
        else:
            log.warn(f"worker output: {line}")  # stderr, e.g. a Python traceback
        if line.startswith("progress "):
            _, frac, text = (line.split(" ", 2) + [""])[:3]
            progress.update(float(frac), text)
        elif line.startswith("chunk_done "):
            i = int(line.split()[1])
            s, n = chunks[i]
            results[i] = np.fromfile(f"{out_prefix}_{i}.bin", dtype="<f2",
                                     offset=transport.HEADER.size).reshape(n, m["height"], m["width"])
            os.unlink(f"{out_prefix}_{i}.bin")
            for f in range(s, s + n):
                finalize(f)
            for j in list(results):
                js, jn = chunks[j]
                if all(f in finished for f in range(js, js + jn)):
                    del results[j]
            log.info(f"chunk {i + 1}/{len(chunks)} done in {time.time() - t_chunk:.1f} s; "
                     f"{len(finished)}/{len(keys)} frames cached")
            t_chunk = time.time()
            cache.write_status(root, job_id, "running", "", chunks_done=i + 1, chunks=len(chunks),
                               frames_done=len(finished), frames=len(keys))
        elif line == "done":
            state, message = "done", ""
        elif line.startswith("error "):
            state, message = "failed", line[6:]

    status = worker.wait()
    if state == "done" and len(finished) != len(keys):
        state, message = "failed", f"only {len(finished)} of {len(keys)} frames were produced"
    if state == "failed" and status < 0:
        message = f"worker crashed (signal {-status})"
    progress.close()
    cache.clear_pending(root, keys)
    cache.write_status(root, job_id, state, message, chunks_done=len(chunks) if state == "done" else -1,
                       chunks=len(chunks), frames_done=len(finished), frames=len(keys))
    for p in [m["input"], manifest_path, pid_file] + [f"{out_prefix}_{i}.bin" for i in range(len(chunks))]:
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass
    summary = (f"end: {state} {message} ({len(finished)}/{len(keys)} frames cached, worker exit {status}, "
               f"{time.time() - t_start:.1f} s)")
    (log.info if state == "done" else log.warn if state == "cancelled" else log.error)(summary)
    if state == "failed":
        Progress.error("LTX Alpha Gen", f"Processing failed: {message}", ui)
    return 0 if state == "done" else 1


if __name__ == "__main__":
    sys.exit(main())
