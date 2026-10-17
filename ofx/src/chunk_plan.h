// Chunk planning for clips longer than the model's 145-frame window, and the
// per-frame crossfade weights between overlapping chunks. Pure functions, no OFX.
// Design: CLAUDE.md "Padding and chunking" (decisions 24, 30).
#pragma once

#include <vector>

namespace ltxag {

constexpr int kChunkLen = 145;  // 8*18+1, the model's max frame count
constexpr int kOverlap = 24;    // minimum overlap / crossfade length (decision 30)

// Chunk Size choices (decision 61): 8n+1 lengths and the overlap that goes with each.
// Index 0 is Auto in the UI (resolved to one of 1..3 at Process time).
constexpr int kChunkSizes = 3;
constexpr int kSizeLen[kChunkSizes] = {145, 73, 33};   // full, half, quarter
constexpr int kSizeOverlap[kChunkSizes] = {24, 16, 12};

// Frame Hold (decision 74): the worker keeps frame 0 once and repeats every later frame `hold`
// times (1, 2, 4, 8), so a chunk of `len` source frames is 1 + hold * (len - 1) model frames.
// A Chunk Size keeps its model length; it holds fewer source frames, with a smaller overlap.
inline int heldModelFrames(int len, int hold) { return len <= 0 ? 0 : 1 + hold * (len - 1); }
inline int heldChunkLen(int sizeIdx, int hold) { return 1 + (kSizeLen[sizeIdx] - 1) / hold; }
inline int heldOverlap(int sizeIdx, int hold) {
  const int o = kSizeOverlap[sizeIdx] / hold;
  return o < 2 ? 2 : o;
}

// A chunk covers real (0-based) frames [start, start + len), each held `hold` times (74).
struct Chunk {
  int start;
  int len;
  int hold = 1;
  int end() const { return start + len - 1; }  // inclusive
};

// Auto Frame Hold (decision 75). Motion thresholds in crop pixels per frame, from the A/B
// (worker/CLAUDE.md): below 12 no hold, below 40 2x, else 4x (8x is never chosen).
constexpr double kAutoHold2 = 12.0, kAutoHold4 = 40.0;
constexpr int kAutoSpread = 4;   // a fast frame also holds its neighbours (one latent group)
constexpr int kAutoMinChunk = 17;  // a shorter slow stretch before a faster one is absorbed
constexpr const char* kAutoHoldVersion = "auto1";  // cache key part; bump with the thresholds

// motion[f] = motion between frames f-1 and f (motion[0] unused). Returns a hold per frame.
std::vector<int> autoHolds(const std::vector<double>& motion);
// Chunks of at most kSizeLen[sizeIdx] model frames; each chunk's hold is at least every one of
// its frames' holds. All frames alike: the same plan as a fixed hold. Weights: frameContributions
// with kSizeOverlap[sizeIdx].
// Never more model frames than the fixed plan at the highest hold found (else that plan).
std::vector<Chunk> planAutoChunks(const std::vector<int>& holds, int sizeIdx);
// Total model frames of a plan (its cost, roughly).
int planModelFrames(const std::vector<Chunk>& chunks);

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
// crossfaded over min(O, overlap) frames: linearly, or with smoothstep (decision 62).
// One or two entries.
std::vector<Contribution> frameContributions(const std::vector<Chunk>& chunks, int i,
                                             int O = kOverlap, bool smooth = false);

}  // namespace ltxag
