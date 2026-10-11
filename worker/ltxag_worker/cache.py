"""Disk matte cache, writer side. Layout must match ofx/src/cache.h:
  <root>/<k[0:2]>/<key>.matte   b"LTXM" v1 W H 1 1, then H*W float16, rows top-down
  <root>/pending/<key>          holds the pid of the job computing it
  <root>/jobs/<job id>.json     final job status
"""

from __future__ import annotations

import json
import os
import time

import numpy as np

from . import transport


def matte_path(root: str, key: str) -> str:
    return os.path.join(root, key[:2], key + ".matte")


def pending_path(root: str, key: str) -> str:
    return os.path.join(root, "pending", key)


def write_matte(root: str, key: str, matte: np.ndarray) -> None:
    """Atomic: write a temp file in the target dir, then rename over the final name."""
    path = matte_path(root, key)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = f"{path}.tmp{os.getpid()}"
    transport.write_mattes(tmp, matte[np.newaxis])
    os.replace(tmp, path)


def mark_pending(root: str, keys: list[str], pid: int) -> None:
    os.makedirs(os.path.join(root, "pending"), exist_ok=True)
    for k in keys:
        with open(pending_path(root, k), "w") as f:
            f.write(str(pid))


def clear_pending(root: str, keys) -> None:
    for k in keys:
        try:
            os.unlink(pending_path(root, k))
        except FileNotFoundError:
            pass


def write_status(root: str, job_id: str, state: str, message: str = "", **progress) -> None:
    """state: running | done | failed | cancelled. Extra fields: chunks_done, chunks,
    frames_done, frames. Read by the plugin (ofx/src/plugin.cpp jsonField)."""
    d = os.path.join(root, "jobs")
    os.makedirs(d, exist_ok=True)
    tmp = os.path.join(d, f".{job_id}.tmp")
    with open(tmp, "w") as f:
        json.dump({"state": state, "message": message, "time": time.time(), **progress}, f)
    os.replace(tmp, os.path.join(d, job_id + ".json"))
