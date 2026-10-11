// Chunk planning for clips longer than the model's 145-frame window, and the
// per-frame crossfade weights between overlapping chunks. Pure functions, no OFX.
// Design: CLAUDE.md "Padding and chunking" (decisions 24, 30).
#pragma once

#include <vector>

namespace ltxag {

constexpr int kChunkLen = 145;  // 8*18+1, the model's max frame count
constexpr int kOverlap = 24;    // minimum overlap / crossfade length (decision 30)

// A chunk covers real (0-based) frames [start, start + len).
struct Chunk {
  int start;
  int len;
  int end() const { return start + len - 1; }  // inclusive
};

// n <= L: one chunk of n frames (the worker pads it to 8n+1).
// n > L: chunks of exactly L real frames, first at 0, last ending at n-1,
// starts spread evenly, every overlap >= O (up to rounding, see tests).
std::vector<Chunk> planChunks(int n, int L = kChunkLen, int O = kOverlap);

struct Contribution {
  int chunk;
  float weight;
};

// Chunks that contribute to frame i, with weights summing to 1. Between
// neighbouring chunks the switch happens at the middle of their overlap,
// crossfaded linearly over min(O, overlap) frames. One or two entries.
std::vector<Contribution> frameContributions(const std::vector<Chunk>& chunks, int i,
                                             int O = kOverlap);

}  // namespace ltxag
