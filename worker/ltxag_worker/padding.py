"""Spatial (/32) and temporal (8n+1) padding for the model, and the matching crop.

The worker contract is "N frames of WxH in, N mattes of WxH out"; padding never
leaves the worker. Design: CLAUDE.md "Padding and chunking" (decision 24).
"""

from __future__ import annotations

import numpy as np

SPATIAL_MULTIPLE = 32
TEMPORAL_STEP = 8


def padded_size(n: int, multiple: int = SPATIAL_MULTIPLE) -> int:
    return -(-n // multiple) * multiple


def padded_frame_count(n: int) -> int:
    """Smallest 8k+1 >= n."""
    return n + (-(n - 1)) % TEMPORAL_STEP


def pad_spatial(frames: np.ndarray) -> tuple[np.ndarray, tuple[int, int]]:
    """Edge-replicate (N,H,W,C) up to multiples of 32, split evenly per axis.

    Returns the padded array and (top, left) offsets for cropping back.
    """
    _, h, w, _ = frames.shape
    ph, pw = padded_size(h) - h, padded_size(w) - w
    top, left = ph // 2, pw // 2
    if ph == 0 and pw == 0:
        return frames, (0, 0)
    padded = np.pad(frames, ((0, 0), (top, ph - top), (left, pw - left), (0, 0)), mode="edge")
    return padded, (top, left)


def pad_temporal(frames: np.ndarray) -> np.ndarray:
    """Pad at the end only, up to 8k+1, mirroring the tail (..., N-1, N-2, N-3, ...).

    Frame 0 is never padded: the VAE is temporally causal and encodes it alone.
    Falls back to holding the last frame when the clip is too short to mirror.
    """
    n = frames.shape[0]
    extra = padded_frame_count(n) - n
    if extra == 0:
        return frames
    if n - 1 >= extra:
        idx = [n - 2 - k for k in range(extra)]
    else:
        idx = [n - 1] * extra
    return np.concatenate([frames, frames[idx]], axis=0)


def hold_indices(n: int, hold: int) -> list[int]:
    """Frame Hold (decision 74): frame 0 once (the VAE encodes it alone), every later frame
    `hold` times, so one 8-frame latent group spans 8/hold source frames instead of 8."""
    return [0] + [i for i in range(1, n) for _ in range(hold)]


def held_output_indices(n: int, hold: int) -> list[int]:
    """The decoded frame kept for each source frame: frame 0, then the middle copy of each hold."""
    return [0] + [1 + hold * (i - 1) + hold // 2 for i in range(1, n)]


def crop(mattes: np.ndarray, n: int, top: int, left: int, h: int, w: int) -> np.ndarray:
    """Undo both paddings: (N',H',W') -> (n,h,w)."""
    return mattes[:n, top : top + h, left : left + w]
