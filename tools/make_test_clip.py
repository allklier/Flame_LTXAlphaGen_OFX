"""Synthetic reference clip for build step 3 (VRAM/time measurements).

A soft-edged disc with a fuzzy "hair" fringe moving over a textured background, so the
model has a plausible foreground. Not for quality judgement, only for cost measurement.

  python make_test_clip.py OUT.mp4 WIDTH HEIGHT FRAMES [FPS]
Requires numpy and av (both in the LTX-2 venv).
"""

from __future__ import annotations

import sys

import av
import numpy as np


def main() -> int:
    out, w, h, n = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
    fps = int(sys.argv[5]) if len(sys.argv) > 5 else 24
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    rng = np.random.default_rng(0)
    noise = rng.random((h, w), dtype=np.float32)
    background = np.stack([0.25 + 0.2 * np.sin(xx / 37.0) + 0.1 * noise,
                           0.35 + 0.2 * np.cos(yy / 53.0),
                           0.30 + 0.1 * noise], axis=-1)
    container = av.open(out, mode="w")
    stream = container.add_stream("libx264", rate=fps)
    stream.width, stream.height, stream.pix_fmt = w, h, "yuv420p"
    stream.options = {"crf": "16"}
    r = 0.22 * min(w, h)
    for f in range(n):
        t = f / max(1, n - 1)
        cx, cy = w * (0.25 + 0.5 * t), h * (0.5 + 0.15 * np.sin(6.28 * t))
        d = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2)
        fringe = 0.5 + 0.5 * np.sin(np.arctan2(yy - cy, xx - cx) * 40.0)
        alpha = np.clip((r + 0.06 * r * fringe - d) / (0.02 * r), 0.0, 1.0)[..., None]
        subject = np.array([0.85, 0.55, 0.40], np.float32)
        frame = background * (1 - alpha) + subject * alpha
        img = (np.clip(frame, 0, 1) * 255).astype(np.uint8)
        for packet in stream.encode(av.VideoFrame.from_ndarray(img, format="rgb24")):
            container.mux(packet)
    for packet in stream.encode():
        container.mux(packet)
    container.close()
    print(f"wrote {out}: {w}x{h} {n} frames @ {fps} fps")
    return 0


if __name__ == "__main__":
    sys.exit(main())
