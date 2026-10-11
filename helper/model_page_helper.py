"""Prototype stand-in for the Model Page download helper. Downloads nothing.

Launched detached by the OFX "Download" button. Tests that:
  - a long job can run outside Flame's UI thread with its own progress window,
  - the HF token arrives via the environment (HF_TOKEN), never on argv.
The token value is never printed or logged; only whether it arrived and its length.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "worker"))

from ltxag_worker.log import Logger  # noqa: E402

CONFIG = os.path.expanduser("~/.config/ltx-alphagen/config.conf")
SECONDS = 15
_logger = Logger("helper")


def log(msg: str) -> None:
    _logger.info(msg)


def mark_models_ready(target: str) -> None:
    """Prototype stand-in for the verify step: record in the user config that models exist."""
    cfg = {}
    if os.path.exists(CONFIG):
        with open(CONFIG) as f:
            for line in f:
                line = line.rstrip("\n")
                if line and not line.startswith("#") and "=" in line:
                    k, v = line.split("=", 1)
                    cfg[k] = v
    cfg["models_ready"] = time.strftime("%Y-%m-%d")
    cfg["model_dir"] = target
    os.makedirs(os.path.dirname(CONFIG), exist_ok=True)
    tmp = CONFIG + ".tmp"
    with open(tmp, "w") as f:
        f.write("# LTX Alpha Gen OFX user settings. Written by the plugin.\n")
        for k in sorted(cfg):
            f.write(f"{k}={cfg[k]}\n")
    os.replace(tmp, CONFIG)


def main() -> int:
    token = os.environ.pop("HF_TOKEN", "")
    target = sys.argv[1] if len(sys.argv) > 1 else "(none)"
    log(f"started; token received={'yes' if token else 'no'} length={len(token)}; "
        f"argv has no token={token not in ' '.join(sys.argv) if token else True}; target={target}")
    zenity = shutil.which("zenity")
    if not zenity:
        log("zenity not found; simulating without a window")
        time.sleep(SECONDS)
        mark_models_ready(target)
        log("finished (no window); models_ready recorded")
        return 0
    proc = subprocess.Popen(
        [zenity, "--progress", "--title=LTX Alpha Gen - model download (prototype)",
         "--text=Simulated download...", "--percentage=0", "--auto-close"],
        stdin=subprocess.PIPE, text=True)
    try:
        for i in range(SECONDS + 1):
            if proc.poll() is not None:
                log("window closed by user (cancel)")
                return 1
            pct = int(100 * i / SECONDS)
            proc.stdin.write(f"# Simulated download {pct}% into {target}\n{pct}\n")
            proc.stdin.flush()
            time.sleep(1)
    except BrokenPipeError:
        log("window closed by user (cancel)")
        return 1
    proc.stdin.close()
    proc.wait()
    mark_models_ready(target)
    log("finished; models_ready recorded")
    return 0


if __name__ == "__main__":
    sys.exit(main())
