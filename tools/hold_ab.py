"""A/B of Frame Hold (decision 74) on synthetic fast motion with a known matte.

  python hold_ab.py WORKER_PY MODEL_DIR EMBEDDINGS OUTDIR [--w 1920 --h 1080 --n 19
                    --speeds 4,24,64,128 --holds 1,2,4,8 --precision bf16]

A textured, fringed disc circles over a textured background at a set speed (px per frame),
rendered with a 180-degree shutter (8 sub-samples), so fast motion blurs like real footage; the
truth matte is blurred the same way. Per speed, one worker run (one model load) gets the clip
pre-expanded for every hold (frame 0 once, every later frame `hold` times), as the reporter did,
so every decoded copy of a held frame can be scored (pick offset 0..hold-1). Last, the clip goes
once through the worker's own --hold path (the largest hold, fastest speed) and is compared with
the external result at the worker's pick (padding.held_output_indices).

Metrics per frame (frames 1..n-1; frame 0 is encoded alone in every variant): mean abs error over
the frame and in a band around the true edge, IoU at 0.5, and the centroid lag along the motion
(matte centroid minus true centroid, projected on the direction of motion, px; negative = behind).
Writes OUTDIR/results.csv (per frame), OUTDIR/summary.txt, and side-by-side PNGs of one frame.
Run in the LTX venv (numpy; PIL for the PNGs).
"""

from __future__ import annotations

import argparse
import math
import os
import subprocess
import sys
import tempfile
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "worker"))

from ltxag_worker import padding, transport  # noqa: E402

# Named ltxag_<pid>_...: the plugin's staging sweep keeps such files while <pid> lives (cache.cpp).
STAGE = "/dev/shm" if os.path.isdir("/dev/shm") else tempfile.gettempdir()
SUBSAMPLES, SHUTTER = 8, 0.5


def make_clip(w: int, h: int, n: int, speed: float):
    """Frames (n,h,w,3) f16, truth (n,h,w) f32, centre and unit motion direction per frame."""
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    noise = np.random.default_rng(0).random((h, w), dtype=np.float32)
    bg = np.stack([0.25 + 0.2 * np.sin(xx / 37.0) + 0.1 * noise, 0.35 + 0.2 * np.cos(yy / 53.0),
                   0.30 + 0.1 * noise], axis=-1)
    r, path = 0.22 * min(w, h), 0.2 * min(w, h)
    omega = speed / path  # rad per frame, so the centre moves `speed` px per frame

    def centre(t: float) -> tuple[float, float]:
        return w / 2 + path * math.cos(omega * t), h / 2 + path * math.sin(omega * t)

    frames = np.empty((n, h, w, 3), np.float16)
    truth = np.empty((n, h, w), np.float32)
    centres, dirs = [], []
    for f in range(n):
        rgb = np.zeros((h, w, 3), np.float32)
        a_sum = np.zeros((h, w), np.float32)
        for s in range(SUBSAMPLES):
            t = f + SHUTTER * ((s + 0.5) / SUBSAMPLES - 0.5)
            cx, cy = centre(t)
            dx, dy = xx - cx, yy - cy
            d = np.sqrt(dx * dx + dy * dy)
            ang = np.arctan2(dy, dx)
            fringe = 0.5 + 0.5 * np.sin(ang * 40.0)
            a = np.clip((r + 0.06 * r * fringe - d) / (0.02 * r), 0.0, 1.0)
            # texture on the subject, moving with it (stripes in the disc's frame)
            tex = 0.1 * np.sin((dx * 0.8 + dy * 0.6) / 9.0)
            subj = np.stack([0.85 + tex, 0.55 + tex, 0.40 + 0.5 * tex], axis=-1)
            rgb += bg * (1 - a[..., None]) + subj * a[..., None]
            a_sum += a
        frames[f] = np.clip(rgb / SUBSAMPLES, 0, 1)
        truth[f] = a_sum / SUBSAMPLES
        centres.append(centre(f))
        vx, vy = -math.sin(omega * f), math.cos(omega * f)
        dirs.append((vx, vy) if speed > 0 else (1.0, 0.0))
    return frames, truth, centres, dirs, r


def stage(frames: np.ndarray, tag: str) -> str:
    n, h, w, _ = frames.shape
    path = os.path.join(STAGE, f"ltxag_{os.getpid()}_holdab_{tag}_in.bin")
    with open(path, "wb") as f:
        f.write(transport.HEADER.pack(transport.IN_MAGIC, transport.VERSION, w, h, n, 3))
        frames.astype("<f2").tofile(f)
    return path


def run_worker(a, inp: str, spans, hold: int, tag: str) -> tuple[list[np.ndarray], list[float]]:
    prefix = os.path.join(STAGE, f"ltxag_{os.getpid()}_holdab_{tag}_out")
    cmd = [a.worker_py, "-I", "-B", os.path.join(HERE, "..", "worker", "ltx_worker.py"), "--in", inp,
           "--chunks", ",".join(f"{s}:{ln}" for s, ln in spans), "--out-prefix", prefix,
           "--model-dir", a.model_dir, "--embeddings", a.embeddings, "--precision", a.precision,
           "--hold", str(hold)]
    print("running:", " ".join(cmd), flush=True)
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True)
    secs = []
    for line in proc.stdout:
        line = line.rstrip("\n")
        if line.startswith(("info", "error", "done", "vram", "Traceback")) or "Error" in line:
            print("  worker:", line, flush=True)
        if line.startswith("info chunk ") and ": done in " in line:
            secs.append(float(line.split(": done in ")[1].split(" s")[0]))
    rc = proc.wait()
    os.unlink(inp)
    if rc != 0:
        raise SystemExit(f"worker exit {rc}")
    out = []
    for i in range(len(spans)):
        p = f"{prefix}_{i}.bin"
        with open(p, "rb") as f:
            _, _, w, h, n, _ = transport.HEADER.unpack(f.read(transport.HEADER.size))
            out.append(np.fromfile(f, dtype="<f2").astype(np.float32).reshape(n, h, w))
        os.unlink(p)
    return out, secs


def metrics(m: np.ndarray, t: np.ndarray, c, d, r: float, speed: float) -> tuple[float, float, float, float]:
    h, w = t.shape
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    dist = np.sqrt((xx - c[0]) ** 2 + (yy - c[1]) ** 2)
    band = np.abs(dist - r) < 24 + 0.5 * SHUTTER * speed + 0.06 * r
    mae = float(np.abs(m - t).mean())
    band_mae = float(np.abs(m - t)[band].mean())
    mb, tb = m > 0.5, t > 0.5
    iou = float((mb & tb).sum() / max(1, (mb | tb).sum()))

    def centroid(a):
        s = a.sum()
        return (float((a * xx).sum() / s), float((a * yy).sum() / s)) if s > 0 else (0.0, 0.0)

    cm, ct = centroid(m), centroid(t)
    lag = (cm[0] - ct[0]) * d[0] + (cm[1] - ct[1]) * d[1]
    return mae, band_mae, iou, lag


def save_png(path: str, rows: list[np.ndarray]) -> None:
    try:
        from PIL import Image
    except ImportError:
        return
    img = np.concatenate([np.concatenate(r, axis=1) for r in rows], axis=0)
    Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8)).save(path)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("worker_py")
    ap.add_argument("model_dir")
    ap.add_argument("embeddings")
    ap.add_argument("outdir")
    ap.add_argument("--w", type=int, default=1920)
    ap.add_argument("--h", type=int, default=1080)
    ap.add_argument("--n", type=int, default=19)
    ap.add_argument("--speeds", default="4,24,64,128")
    ap.add_argument("--holds", default="1,2,4,8")
    ap.add_argument("--precision", default="bf16")
    a = ap.parse_args()
    os.makedirs(a.outdir, exist_ok=True)
    speeds = [float(s) for s in a.speeds.split(",")]
    holds = [int(k) for k in a.holds.split(",")]
    n = a.n
    if 1 + max(holds) * (n - 1) > 145:
        raise SystemExit(f"n={n} with hold {max(holds)} exceeds 145 model frames")
    csv = open(os.path.join(a.outdir, "results.csv"), "w")
    csv.write("speed,hold,pick,frame,mae,band_mae,iou,lag_px\n")
    summary = [f"hold_ab {a.w}x{a.h} n={n} precision={a.precision} shutter={SHUTTER} "
               f"subsamples={SUBSAMPLES} {time.strftime('%Y-%m-%d %H:%M')}",
               "speed  hold  pick   band_mae   mae      IoU     lag_px(mean)  |lag|max   s/run"]
    last = None
    for speed in speeds:
        frames, truth, centres, dirs, r = make_clip(a.w, a.h, n, speed)
        parts, spans, start = [], [], 0
        for k in holds:
            idx = padding.hold_indices(n, k)
            parts.append(frames[idx])
            spans.append((start, len(idx)))
            start += len(idx)
        outs, secs = run_worker(a, stage(np.concatenate(parts), f"s{int(speed)}"), spans, 1, f"s{int(speed)}")
        picks_png = []
        for k, out, sec in zip(holds, outs, secs + [0.0] * len(holds)):
            for p in range(k):
                sel = [0] + [1 + k * (i - 1) + p for i in range(1, n)]
                rows = []
                for i in range(1, n):
                    mm = metrics(out[sel[i]], truth[i], centres[i], dirs[i], r, speed)
                    rows.append(mm)
                    csv.write(f"{speed:g},{k},{p},{i}," + ",".join(f"{v:.5f}" for v in mm) + "\n")
                arr = np.array(rows)
                tag = " *" if p == k // 2 else ""
                summary.append(f"{speed:5g}  {k:4d}  {p:4d}{tag:2s} {arr[:, 1].mean():8.4f}  {arr[:, 0].mean():7.4f}  "
                               f"{arr[:, 2].mean():6.4f}  {arr[:, 3].mean():10.1f}  {np.abs(arr[:, 3]).max():9.1f}  "
                               f"{sec:6.1f}")
                if p == k // 2:
                    mid = n // 2
                    picks_png.append(out[sel[mid]][::4, ::4])
            if k == max(holds) and speed == max(speeds):
                last = out[padding.held_output_indices(n, k)]
        mid = n // 2
        top = [frames[mid].astype(np.float32).mean(-1)[::4, ::4], truth[mid][::4, ::4]]
        top += [np.zeros_like(top[0])] * (len(picks_png) - len(top))
        save_png(os.path.join(a.outdir, f"speed{int(speed)}_frame{mid}.png"), [top, picks_png])
        print("\n".join(summary), flush=True)
    # The worker's own --hold path must match the external expansion at the worker's pick.
    k, speed = max(holds), max(speeds)
    if k > 1:
        frames, *_ = make_clip(a.w, a.h, n, speed)
        outs, _ = run_worker(a, stage(frames, "internal"), [(0, n)], k, "internal")
        diff = np.abs(outs[0] - last)
        summary.append(f"worker --hold {k} vs external pick at speed {speed:g}: max diff {diff.max():.4f}, "
                       f"mean {diff.mean():.5f}")
    csv.close()
    with open(os.path.join(a.outdir, "summary.txt"), "w") as f:
        f.write("\n".join(summary) + "\n")
    print("\n".join(summary))
    print("picture rows: source luma | truth ; then the middle-pick matte per hold (" +
          ", ".join(str(k) for k in holds) + ")")
    return 0


if __name__ == "__main__":
    sys.exit(main())
