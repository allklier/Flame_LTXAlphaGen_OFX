#!/usr/bin/python3
"""Stand-in for the windows job.py opens (systest/README.md), installed by run_systest.sh:
Linux: as `zenity` first on PATH; macOS: as the compiled progress helper
(~/.cache/ltx-alphagen/bin/ltxag_progress-<hash>) in the test HOME.

Progress mode (zenity --progress ..., or the macOS helper's `<title> <width>`): reads zenity's
stdin protocol ("# text" / "<percent>"), appends every line to $LTXAG_FAKE_UI_DIR/progress.log,
exits 0 at "100" or end of input, and exits 1 (= the user pressed Cancel) as soon as the file
$LTXAG_FAKE_UI_DIR/cancel exists. Message mode (zenity --error/--info/--text-info ...): appends
the arguments to messages.log and exits 0. Standard library only."""

import os
import sys
import threading
import time

DIR = os.environ.get("LTXAG_FAKE_UI_DIR", "/tmp")


def log(name: str, text: str) -> None:
    with open(os.path.join(DIR, name), "a") as f:
        f.write(text + "\n")


def main() -> int:
    args = sys.argv[1:]
    log("windows.log", f"start pid={os.getpid()} args={args!r}")
    if args and args[0].startswith("--") and args[0] != "--progress":
        log("messages.log", " ".join(args))
        return 0

    def watch_cancel() -> None:
        while not os.path.exists(os.path.join(DIR, "cancel")):
            time.sleep(0.2)
        log("windows.log", f"cancel pid={os.getpid()}")
        os._exit(1)

    threading.Thread(target=watch_cancel, daemon=True).start()
    for line in sys.stdin:
        line = line.rstrip("\n")
        log("progress.log", line)
        if line.strip() == "100":
            break
    log("windows.log", f"end pid={os.getpid()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
