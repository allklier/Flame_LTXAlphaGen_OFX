"""Shared diagnostic log, Python side (decision 39). Same file and line format as
ofx/src/log.cpp, so the plugin, job, worker and helper interleave in one timeline:

  2026-10-10 16:35:53.383 INFO  job[419116/419116] node=ab12cd34 job=166761-0-17 | message

Level: $LTXAG_LOG_LEVEL, else log_level in the user config, else info. Rotation is done
only by the plugin at load. Logging never raises.
"""

from __future__ import annotations

import os
import threading
import time

LEVELS = {"debug": 0, "info": 1, "warn": 2, "error": 3}
_NAMES = {0: "DEBUG", 1: "INFO ", 2: "WARN ", 3: "ERROR"}
CONFIG = os.path.expanduser("~/.config/ltx-alphagen/config.conf")


def log_path() -> str:
    return os.path.expanduser("~/.cache/ltx-alphagen/logs/ltx-alphagen.log")


def _configured_level() -> int:
    env = os.environ.get("LTXAG_LOG_LEVEL", "")
    if env in LEVELS:
        return LEVELS[env]
    try:
        with open(CONFIG) as f:
            for line in f:
                if line.startswith("log_level="):
                    return LEVELS.get(line.strip().split("=", 1)[1], 1)
    except OSError:
        pass
    return 1


class Logger:
    def __init__(self, component: str, node: str = "-", job: str = "-") -> None:
        self.component, self.node, self.job = component, node or "-", job or "-"
        self.level = _configured_level()

    def _write(self, level: int, msg: str) -> None:
        if level < self.level:
            return
        try:
            now = time.time()
            stamp = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(now)) + f".{int(now * 1000) % 1000:03d}"
            line = (f"{stamp} {_NAMES[level]} {self.component}[{os.getpid()}/{threading.get_native_id()}] "
                    f"node={self.node} job={self.job} | {msg}\n")
            path = log_path()
            os.makedirs(os.path.dirname(path), exist_ok=True)
            fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o644)
            try:
                os.write(fd, line.encode("utf-8", "replace"))  # one write per line
            finally:
                os.close(fd)
        except Exception:  # noqa: BLE001 - logging must never break the job
            pass

    def debug(self, msg: str) -> None:
        self._write(0, msg)

    def info(self, msg: str) -> None:
        self._write(1, msg)

    def warn(self, msg: str) -> None:
        self._write(2, msg)

    def error(self, msg: str) -> None:
        self._write(3, msg)
