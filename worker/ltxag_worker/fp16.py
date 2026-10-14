"""fp16 transformer on Apple Silicon (decision 68). Runs in the LTX venv (needs torch).

probe(): times a transformer-sized matmul in bf16 and fp16 on MPS. The M1 has no fast bf16
(fp16 measured 1.40x faster per denoising step); M2-M4 have native bf16; on M5 PyTorch runs
bf16 as fast as fp16 (mac_bench). Setup makes the fp16 copy only where fp16 is faster.

convert(): writes an fp16 copy of the bf16 transformer checkpoint, tensor by tensor (constant
memory). BF16 tensors become F16, the F32 scale/shift tables and the metadata (model config) are
kept. Loading that copy holds the same memory as the bf16 file; casting at load instead held
~26 GB more and loaded slower (fp16 experiment 2026-10-13). Fails if a weight would overflow fp16
(none does in LTX-2.5 22B; 0.0003 % of the nonzero weights become 0).
"""

from __future__ import annotations

import json
import os
import struct
import time

F16_MAX = 65504.0
F16_TINY = 2.0 ** -24  # smallest positive subnormal


def probe(torch, device, n: int = 4096, repeats: int = 5) -> tuple[float, float]:  # noqa: ANN001
    """Milliseconds per (n x n) @ (n x n) matmul in bf16 and fp16."""
    times = []
    for dtype in (torch.bfloat16, torch.float16):
        a = torch.randn(n, n, device=device, dtype=dtype)
        a @ a  # warm-up: kernel selection
        torch.mps.synchronize()
        t0 = time.perf_counter()
        for _ in range(repeats):
            a @ a
        torch.mps.synchronize()
        times.append((time.perf_counter() - t0) / repeats * 1000)
    return times[0], times[1]


def convert(torch, src: str, dst: str, progress=print) -> tuple[int, int]:  # noqa: ANN001
    """Writes dst (via dst's folder + "/fp16-tmp/"); returns (values that become 0, nonzero values).
    Raises ValueError if a value would overflow fp16."""
    with open(src, "rb") as f:
        n = struct.unpack("<Q", f.read(8))[0]
        header = json.loads(f.read(n))
    base = 8 + n
    meta = header.pop("__metadata__", None)
    names = sorted(header, key=lambda k: header[k]["data_offsets"][0])
    out, offset = {}, 0
    for k in names:
        e = header[k]
        size = e["data_offsets"][1] - e["data_offsets"][0]
        out[k] = {"dtype": "F16" if e["dtype"] == "BF16" else e["dtype"], "shape": e["shape"],
                  "data_offsets": [offset, offset + size]}
        offset += size
    if meta is not None:
        out["__metadata__"] = meta
    hb = json.dumps(out, separators=(",", ":")).encode()
    hb += b" " * (-len(hb) % 8)
    tmp_dir = os.path.join(os.path.dirname(os.path.abspath(dst)), "fp16-tmp")
    os.makedirs(tmp_dir, exist_ok=True)
    tmp = os.path.join(tmp_dir, os.path.basename(dst) + ".part")
    underflow = nonzero = 0
    with open(src, "rb") as fi, open(tmp, "wb") as fo:
        fo.write(struct.pack("<Q", len(hb)))
        fo.write(hb)
        for i, k in enumerate(names):
            e = header[k]
            a, b = e["data_offsets"]
            fi.seek(base + a)
            raw = fi.read(b - a)
            if e["dtype"] == "BF16":
                t = torch.frombuffer(bytearray(raw), dtype=torch.bfloat16).float()
                mag = t.abs()
                if bool((mag > F16_MAX).any()):
                    fo.close()
                    os.unlink(tmp)
                    raise ValueError(f"{k} has values beyond fp16's range")
                underflow += int(((mag > 0) & (mag < F16_TINY)).sum())
                nonzero += int((mag > 0).sum())
                raw = t.to(torch.float16).numpy().tobytes()
            fo.write(raw)
            if i % 500 == 0:
                progress(f"{i}/{len(names)} tensors")
    os.replace(tmp, dst)
    os.rmdir(tmp_dir)
    return underflow, nonzero
