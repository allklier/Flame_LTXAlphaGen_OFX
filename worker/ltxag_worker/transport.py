"""Shared-memory frame files exchanged with the OFX shim.

Layout (little-endian), written by the shim into /dev/shm:
  input : b"LTXA" u32 version u32 W u32 H u32 N u32 C  then N*H*W*C float16, rows top-down
  output: b"LTXM" u32 version u32 W u32 H u32 N u32 1  then N*H*W float16, rows top-down
Must match ofx/src/transport.h.
"""

from __future__ import annotations

import struct

import numpy as np

VERSION = 1
HEADER = struct.Struct("<4sIIIII")  # 24 bytes
IN_MAGIC = b"LTXA"
OUT_MAGIC = b"LTXM"


def read_frames(path: str) -> np.ndarray:
    with open(path, "rb") as f:
        magic, version, w, h, n, c = HEADER.unpack(f.read(HEADER.size))
        if magic != IN_MAGIC or version != VERSION:
            raise ValueError(f"bad input header {magic!r} v{version}")
        data = np.fromfile(f, dtype="<f2", count=n * h * w * c)
    if data.size != n * h * w * c:
        raise ValueError(f"short input: {data.size} of {n * h * w * c} values")
    return data.reshape(n, h, w, c)


def read_frames_range(path: str, start: int, count: int) -> np.ndarray:
    """Frames [start, start+count) of an input file, without reading the rest."""
    with open(path, "rb") as f:
        magic, version, w, h, n, c = HEADER.unpack(f.read(HEADER.size))
    if magic != IN_MAGIC or version != VERSION:
        raise ValueError(f"bad input header {magic!r} v{version}")
    if start < 0 or start + count > n:
        raise ValueError(f"frames {start}+{count} outside input of {n}")
    frame = h * w * c * 2
    return np.array(np.memmap(path, dtype="<f2", mode="r", offset=HEADER.size + start * frame,
                              shape=(count, h, w, c)))


def write_mattes(path: str, mattes: np.ndarray) -> None:
    n, h, w = mattes.shape
    with open(path, "wb") as f:
        f.write(HEADER.pack(OUT_MAGIC, VERSION, w, h, n, 1))
        np.ascontiguousarray(mattes, dtype="<f2").tofile(f)
