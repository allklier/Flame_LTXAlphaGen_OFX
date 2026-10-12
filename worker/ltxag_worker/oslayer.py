"""Everything in the Python side that differs between operating systems (decision 64).

Linux: zenity windows and /proc. Standard library only (setup_helper.py and job.py run under the
system python3).
"""

from __future__ import annotations

import shutil
import subprocess


class ProgressWindow:
    """A progress window with a Cancel button. Without a UI (or zenity) every call is a no-op."""

    def __init__(self, title: str, width: int, enabled: bool) -> None:
        self.proc = None
        zenity = shutil.which("zenity") if enabled else None
        if zenity:
            self.proc = subprocess.Popen(
                [zenity, "--progress", f"--title={title}", "--text=Starting...",
                 "--percentage=0", "--auto-close", f"--width={width}"],
                stdin=subprocess.PIPE, text=True)

    @property
    def shown(self) -> bool:
        return self.proc is not None

    def gone(self) -> bool:
        """True once the window has closed (the user pressed Cancel, or it was closed)."""
        return bool(self.proc) and self.proc.poll() is not None

    def show(self, frac: float, text: str) -> None:
        """Raises BrokenPipeError if the window has gone."""
        pct = max(0, min(99, int(frac * 100)))  # 100 auto-closes; sent only by close()
        self.proc.stdin.write(f"# {text}\n{pct}\n")
        self.proc.stdin.flush()

    def close(self, wait: float = 0.0) -> None:
        if self.proc and self.proc.poll() is None:
            try:
                self.proc.stdin.write("100\n")
                self.proc.stdin.close()
            except BrokenPipeError:
                pass
            if wait:
                self.proc.wait(timeout=wait)


def message(kind: str, title: str, text: str, width: int, enabled: bool) -> None:
    """A detached message window; kind is "info" or "error"."""
    zenity = shutil.which("zenity") if enabled else None
    if zenity:
        subprocess.Popen([zenity, f"--{kind}", f"--title={title}", f"--text={text}", f"--width={width}"],
                         start_new_session=True)


def process_name(pid: int) -> str:
    """Short name of a live process ("flame"), "" if it is gone."""
    try:
        with open(f"/proc/{int(pid)}/comm") as f:
            return f.read().strip()
    except (OSError, ValueError):
        return ""
