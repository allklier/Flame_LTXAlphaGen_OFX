"""LTX Alpha Gen: Flame Python hook that refreshes the viewer when a job finishes (decision 49 b).

Installed by copying this file to /opt/Autodesk/shared/python/ (or a $DL_PYTHON_HOOK_PATH folder);
Flame loads it at start-up. It runs inside Flame's Python, so it must never raise.

How it works: job.py writes ~/.cache/ltx-alphagen/run/refresh/<job>.json when a job ends. This
hook polls that folder once a second from Flame's idle loop (flame.schedule_idle_event, the
pattern of Autodesk's python_utilities/examples/watch_folder.py) and, on Flame's UI thread, tries
to make Batch re-render the LTX node.

Strategy (round 3, 2026-10-11): step flame.batch.current_frame to a neighbour, then back one
second later. Flame re-renders on every frame change, and the return renders the original
frame from the fresh cache.
History: round 1 (Flame test 2): idling on the UI thread, setting OFX params from Python (not
exposed: node.attributes are only pos_x, name, bypass, cache, ...) and setting current_frame to
its own value never rendered. Round 2 (test 3): the frame step DID render (+0.13 s), but both
undo steps were scheduled with delay=0.5 and failed: the signature is
schedule_idle_event(function, int delay=0). That left the frame moved and every LTX node
bypassed (black), so the bypass toggle is gone: it touched all LTX nodes (the hook cannot tell
which node finished; OFX params are invisible to Python) and the frame step is enough.
Everything goes to the plugin's log (~/.cache/ltx-alphagen/logs/ltx-alphagen.log), component "hook".
"""

from __future__ import annotations

import importlib.util
import json
import os

BUNDLE_WORKER = "/usr/OFX/Plugins/LTXAlphaGen.ofx.bundle/Contents/Resources/worker"
REFRESH_DIR = os.path.expanduser("~/.cache/ltx-alphagen/run/refresh")
POLL_SECONDS = 1
STEP_BACK = 1  # seconds before stepping back: schedule_idle_event takes an int delay


def _load_logger():
    """The plugin's own logger (same file and format), loaded by path: no sys.path change in Flame."""
    try:
        spec = importlib.util.spec_from_file_location("ltxag_hook_log", os.path.join(BUNDLE_WORKER, "ltxag_worker", "log.py"))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        return mod.Logger("hook")
    except Exception:  # noqa: BLE001

        class _Null:
            def __getattr__(self, _name):
                return lambda _msg: None

        return _Null()


log = _load_logger()
_started = False


def _schedule(fn, delay: int) -> None:
    import flame

    flame.schedule_idle_event(fn, delay=delay)


def _frame_step() -> str:
    import flame

    attr = flame.batch.current_frame
    v = int(attr.get_value())
    other = v - 1 if v > 1 else v + 1
    attr.set_value(other)

    def back():
        try:
            attr.set_value(v)
            log.info(f"refresh 'frame_step': back to frame {v}")
        except Exception as exc:  # noqa: BLE001
            log.info(f"refresh 'frame_step': could not go back to frame {v}: {type(exc).__name__}: {exc}")

    try:
        _schedule(back, STEP_BACK)
    except Exception as exc:  # noqa: BLE001 - never leave the user on another frame
        log.info(f"refresh 'frame_step': could not schedule the step back ({type(exc).__name__}); stepping back now")
        back()
    return f"current_frame {v} -> {other}, back in {STEP_BACK} s"


def _refresh(marker: dict) -> None:
    try:
        result = _frame_step()
        log.info(f"refresh after job {marker.get('job_id')} ({marker.get('state')}): {result}")
    except Exception as exc:  # noqa: BLE001
        log.info(f"refresh failed: {type(exc).__name__}: {exc}")


def _poll() -> None:
    try:
        names = sorted(n for n in os.listdir(REFRESH_DIR) if n.endswith(".json"))
    except OSError:
        names = []
    latest = None
    for name in names:  # several jobs ending together need one refresh
        path = os.path.join(REFRESH_DIR, name)
        try:
            with open(path) as f:
                latest = json.load(f)
            os.unlink(path)
        except (OSError, ValueError):
            continue
        log.info(f"job {latest.get('job_id')} ended ({latest.get('state')}), node {latest.get('node_id')}")
    if latest is not None:
        _refresh(latest)
    try:
        _schedule(_poll, POLL_SECONDS)
    except Exception as exc:  # noqa: BLE001
        log.info(f"could not reschedule the poll; refresh stops: {exc}")


def app_initialized(project_name):  # noqa: ARG001 - Flame hook signature
    global _started
    if _started:
        return
    _started = True
    try:
        _schedule(_poll, POLL_SECONDS)
        log.info("hook started: watching for finished jobs")
    except Exception as exc:  # noqa: BLE001
        log.info(f"hook could not start: {type(exc).__name__}: {exc}")


log.info("hook module loaded")
