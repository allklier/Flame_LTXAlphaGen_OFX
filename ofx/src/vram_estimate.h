// Size-aware peak-VRAM estimate for one worker job (decisions 12/34, build step 3 data).
// Fitted to measurements on the reference machine (RTX PRO 6000 Blackwell 96 GB, 2026-10-10/11):
//   official CLI, HD 1920x1088:  fp8 97f 34.1 / 145f 41.3 GB;  bf16 49f 44.9 / 145f 58.8 GB
//   official CLI, UHD 3840x2176 49f: fp8 54.1 / bf16 67.8 GB
//   our worker, HD 65f: fp8 28.3 GB rebuilt per chunk, 37.7 GB with the transformer resident
// need = base(precision) + 0.0718 GB * Mpx * frames + 0.70 GB * max(0, Mpx - 2.09) + 1 GB,
// where Mpx and frames are the PADDED model sizes of the largest chunk (/32, 8n+1, <= 145).
// bf16 base = fp8 base + 17.3 GB (the transformer weight difference).
// Resident transformer (Unload off; refit 2026-10-11 from Flame test 2 + an A/B run): once the
// transformer exists (every chunk after the first, every chunk of a reused worker) the reference
// encode at the start of a chunk runs on top of it, so
//   need = max(need above, T + 1.56 GB + 0.1088 GB * Mpx * frames + 0.7 GB context + 1 GB),
// T = the held transformer, reserved: fp8 21.7 GB, bf16 36.8 GB. Fitted on reserved memory
// (what free memory loses): HD 145 fp8 56.2 GB, 720p 57 bf16 44.2 GB. Pure, no OFX.
// Apple Silicon (mps = true, decision 65; bf16 only, MPS has no fp8): measured on an M1 Ultra
// (96 GB Metal working set, torch 2.14.1, our worker, 2026-10-12) as the lowest free working set
// (what the process held, incl. the MPS allocator's cache): 720p 49f 52.3, HD 33f 60.4,
// HD 73f 79.7 GB. need = 44.5 + 0.231 GB * Mpx * frames + 1 GB; HD 145 -> ~115 GB, over the
// working set. The resident-transformer case is not measured on MPS (UNVERIFIED): same formula.
#pragma once

namespace ltxag {

struct VramNeed {
  double gb;          // estimated peak
  int modelW, modelH;  // padded
  int modelFrames;     // padded, of the largest chunk
};

VramNeed estimateVramNeed(bool fp8, int width, int height, int framesInLargestChunk,
                          bool residentTransformer, bool mps = false);

}  // namespace ltxag
