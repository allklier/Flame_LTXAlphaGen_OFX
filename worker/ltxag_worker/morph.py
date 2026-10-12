"""Flow-warped dissolve between two mattes of the same frame (decision 62).

Two overlapping chunks give two slightly different mattes for the frames they share. A plain
dissolve of the two leaves a ghost edge where they disagree; here each matte is first warped
towards the other by optical flow, so the edge travels from one position to the other.

numpy only, because job.py runs under the system python3. The flow is Horn-Schunck, coarse to
fine, on a copy reduced to at most MAX_SIDE pixels on its long edge; the warps run at full size.
"""

from __future__ import annotations

import numpy as np

MAX_SIDE = 512  # the flow is estimated at this size or smaller
LEVELS = 4
ITERATIONS = 20
SMOOTHNESS = 0.15  # Horn-Schunck alpha, mattes being in 0..1
MAX_DISPLACEMENT = 0.05  # fraction of the long edge; larger flow is clamped
SAME = 0.01  # mattes closer than this everywhere are just blended


def _reduce(a: np.ndarray, s: int) -> np.ndarray:
    """Mean over s x s blocks (edge rows/columns beyond a whole block are repeated)."""
    if s == 1:
        return a
    h, w = a.shape
    ph, pw = -h % s, -w % s
    if ph or pw:
        a = np.pad(a, ((0, ph), (0, pw)), mode="edge")
    return a.reshape(a.shape[0] // s, s, a.shape[1] // s, s).mean(axis=(1, 3))


def _resize(a: np.ndarray, h: int, w: int) -> np.ndarray:
    """Bilinear resize of a 2-D array to (h, w)."""
    ah, aw = a.shape
    if (ah, aw) == (h, w):
        return a
    ys = np.clip((np.arange(h) + 0.5) * ah / h - 0.5, 0, ah - 1)
    xs = np.clip((np.arange(w) + 0.5) * aw / w - 0.5, 0, aw - 1)
    y0, x0 = np.floor(ys).astype(np.intp), np.floor(xs).astype(np.intp)
    y1, x1 = np.minimum(y0 + 1, ah - 1), np.minimum(x0 + 1, aw - 1)
    fy, fx = (ys - y0)[:, None], (xs - x0)[None, :]
    top = a[y0][:, x0] * (1 - fx) + a[y0][:, x1] * fx
    bot = a[y1][:, x0] * (1 - fx) + a[y1][:, x1] * fx
    return top * (1 - fy) + bot * fy


def warp(img: np.ndarray, fx: np.ndarray, fy: np.ndarray) -> np.ndarray:
    """out(x, y) = img(x + fx, y + fy), bilinear, clamped at the borders."""
    h, w = img.shape
    xs = np.clip(np.arange(w, dtype=np.float32)[None, :] + fx, 0, w - 1)
    ys = np.clip(np.arange(h, dtype=np.float32)[:, None] + fy, 0, h - 1)
    x0, y0 = np.floor(xs).astype(np.intp), np.floor(ys).astype(np.intp)
    x1, y1 = np.minimum(x0 + 1, w - 1), np.minimum(y0 + 1, h - 1)
    tx, ty = xs - x0, ys - y0
    top = img[y0, x0] * (1 - tx) + img[y0, x1] * tx
    bot = img[y1, x0] * (1 - tx) + img[y1, x1] * tx
    return top * (1 - ty) + bot * ty


def _average(a: np.ndarray) -> np.ndarray:
    """Horn-Schunck neighbourhood mean: 4 neighbours 1/6, diagonals 1/12."""
    p = np.pad(a, 1, mode="edge")
    return (p[:-2, 1:-1] + p[2:, 1:-1] + p[1:-1, :-2] + p[1:-1, 2:]) / 6.0 + (
        p[:-2, :-2] + p[:-2, 2:] + p[2:, :-2] + p[2:, 2:]) / 12.0


def estimate_flow(a: np.ndarray, b: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Flow (fx, fy) with a(x) ~ b(x + f(x)), at a's size."""
    a = np.ascontiguousarray(a, dtype=np.float32)
    b = np.ascontiguousarray(b, dtype=np.float32)
    h, w = a.shape
    s = max(1, -(-max(h, w) // MAX_SIDE))
    pyr = [(_reduce(a, s), _reduce(b, s))]
    for _ in range(LEVELS - 1):
        pa, pb = pyr[-1]
        if min(pa.shape) < 32:
            break
        pyr.append((_reduce(pa, 2), _reduce(pb, 2)))
    u = v = None
    for pa, pb in reversed(pyr):
        lh, lw = pa.shape
        if u is None:
            u, v = np.zeros_like(pa), np.zeros_like(pa)
        else:
            u, v = 2 * _resize(u, lh, lw), 2 * _resize(v, lh, lw)
        bw = warp(pb, u, v)
        ix = 0.5 * (np.gradient(pa, axis=1) + np.gradient(bw, axis=1))
        iy = 0.5 * (np.gradient(pa, axis=0) + np.gradient(bw, axis=0))
        it = bw - pa
        denom = SMOOTHNESS ** 2 + ix * ix + iy * iy
        du, dv = np.zeros_like(pa), np.zeros_like(pa)
        for _ in range(ITERATIONS):
            ub, vb = _average(du), _average(dv)
            t = (ix * ub + iy * vb + it) / denom
            du, dv = ub - ix * t, vb - iy * t
        u, v = u + du, v + dv
    scale = h / u.shape[0]  # the reduced size back to a's size
    limit = MAX_DISPLACEMENT * max(h, w)
    fx = np.clip(_resize(u, h, w) * (w / u.shape[1]), -limit, limit)
    fy = np.clip(_resize(v, h, w) * scale, -limit, limit)
    return fx.astype(np.float32), fy.astype(np.float32)


def morph(a: np.ndarray, b: np.ndarray, t: float) -> np.ndarray:
    """The matte at fraction t (0 = a, 1 = b) of the way from a to b.

    Each side is warped by its own flow so that t = 0 gives a and t = 1 gives b exactly:
      a_t(x) = a(x + t * F_ba(x)),  b_t(x) = b(x + (1 - t) * F_ab(x)),  out = (1-t) a_t + t b_t.
    """
    a = np.asarray(a, dtype=np.float32)
    b = np.asarray(b, dtype=np.float32)
    if float(np.max(np.abs(a - b))) < SAME:
        return (1.0 - t) * a + t * b
    abx, aby = estimate_flow(a, b)
    bax, bay = estimate_flow(b, a)
    at = warp(a, t * bax, t * bay)
    bt = warp(b, (1.0 - t) * abx, (1.0 - t) * aby)
    return np.clip((1.0 - t) * at + t * bt, 0.0, 1.0)
