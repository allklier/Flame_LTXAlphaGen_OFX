// Plain-assert tests for chunk_plan; run via ctest. Exits non-zero on failure.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>

#include "chunk_plan.h"

using namespace ltxag;

static int failures = 0;
#define CHECK(cond, ...)                                     \
  do {                                                       \
    if (!(cond)) {                                           \
      std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
      std::fprintf(stderr, __VA_ARGS__);                     \
      std::fprintf(stderr, "\n");                            \
      ++failures;                                            \
    }                                                        \
  } while (0)

static void checkClip(int n, int L = kChunkLen, int O = kOverlap, bool smooth = false) {
  const auto chunks = planChunks(n, L, O);
  CHECK(!chunks.empty(), "n=%d no chunks", n);
  CHECK(chunks.front().start == 0, "n=%d first start %d", n, chunks.front().start);
  CHECK(chunks.back().end() == n - 1, "n=%d last end %d", n, chunks.back().end());
  for (const auto& c : chunks) {
    CHECK(c.len <= L && c.len >= 1, "n=%d bad len %d", n, c.len);
    if (n > L) CHECK(c.len == L, "n=%d chunk not full length", n);
  }
  for (size_t j = 0; j + 1 < chunks.size(); ++j) {
    const int overlap = chunks[j].end() - chunks[j + 1].start + 1;
    // Rounding of evenly spread starts may cost at most one frame of overlap.
    CHECK(overlap >= O - 1, "n=%d overlap %d between %zu and %zu", n, overlap, j, j + 1);
    CHECK(chunks[j + 1].start > chunks[j].start, "n=%d starts not increasing", n);
  }
  float prevLastWeight = -1.0f;
  int prevLastChunk = -1;
  for (int i = 0; i < n; ++i) {
    const auto contrib = frameContributions(chunks, i, O, smooth);
    CHECK(contrib.size() == 1 || contrib.size() == 2, "n=%d i=%d %zu contributions", n, i,
          contrib.size());
    float sum = 0.0f;
    for (const auto& c : contrib) {
      const Chunk& ch = chunks[c.chunk];
      CHECK(i >= ch.start && i <= ch.end(), "n=%d i=%d chunk %d does not cover frame", n, i,
            c.chunk);
      CHECK(c.weight > 0.0f && c.weight <= 1.0f, "n=%d i=%d weight %f", n, i, c.weight);
      sum += c.weight;
    }
    CHECK(std::fabs(sum - 1.0f) < 1e-5f, "n=%d i=%d weights sum %f", n, i, sum);
    // The later chunk's weight never decreases while it is the later chunk.
    const auto& last = contrib.back();
    if (last.chunk == prevLastChunk && contrib.size() == 2)
      CHECK(last.weight >= prevLastWeight, "n=%d i=%d weight not monotonic", n, i);
    prevLastChunk = last.chunk;
    prevLastWeight = contrib.size() == 2 ? last.weight : 1.0f;
  }
}

// Decision 75: any Auto plan covers the clip, keeps every chunk within the model-frame budget,
// holds each chunk at least as much as any of its frames needs, and gives valid weights.
static int gFallbacks = 0;  // plans that came out as the fixed plan (counted, printed)

static void checkAuto(const std::vector<int>& holds, int s, const char* what) {
  const int n = int(holds.size());
  const auto chunks = planAutoChunks(holds, s);
  const bool uniform = std::all_of(holds.begin(), holds.end(), [&](int k) { return k == holds[0]; });
  {  // Never dearer than the fixed plan at the clip's highest hold.
    const int top = *std::max_element(holds.begin(), holds.end());
    auto fixed = planChunks(n, heldChunkLen(s, top), heldOverlap(s, top));
    for (auto& x : fixed) x.hold = top;
    CHECK(planModelFrames(chunks) <= planModelFrames(fixed), "%s n=%d: %d model frames > fixed %dx %d", what, n,
          planModelFrames(chunks), top, planModelFrames(fixed));
    if (chunks.size() == fixed.size() && chunks.front().len == fixed.front().len && chunks.front().hold == top) ++gFallbacks;
  }
  CHECK(!chunks.empty() && chunks.front().start == 0 && chunks.back().end() == n - 1, "%s n=%d coverage", what, n);
  for (size_t j = 0; j < chunks.size(); ++j) {
    const Chunk& c = chunks[j];
    CHECK(heldModelFrames(c.len, c.hold) <= kSizeLen[s], "%s n=%d chunk %zu: %d model frames", what, n, j,
          heldModelFrames(c.len, c.hold));
    for (int f = c.start; f <= c.end(); ++f)
      CHECK(c.hold >= holds[f], "%s n=%d chunk %zu hold %d < frame %d hold %d", what, n, j, c.hold, f, holds[f]);
    if (j + 1 < chunks.size()) {
      CHECK(chunks[j + 1].start > c.start, "%s n=%d starts not increasing at %zu", what, n, j);
      CHECK(chunks[j + 1].start <= c.end(), "%s n=%d no overlap after chunk %zu", what, n, j);
    }
    // Mixed plans keep a frame out of three chunks (uniform ones are planChunks', which may
    // overlap three chunks on short clips; their crossfade bands still don't overlap).
    if (!uniform && j + 2 < chunks.size())
      CHECK(chunks[j + 2].start > c.end(), "%s n=%d three chunks overlap at %zu", what, n, j);
  }
  for (int i = 0; i < n; ++i) {
    float sum = 0.0f;
    for (const auto& c : frameContributions(chunks, i, kSizeOverlap[s], true)) {
      CHECK(i >= chunks[c.chunk].start && i <= chunks[c.chunk].end(), "%s n=%d i=%d not covered", what, n, i);
      sum += c.weight;
    }
    CHECK(std::fabs(sum - 1.0f) < 1e-5f, "%s n=%d i=%d weights sum %f", what, n, i, sum);
  }
}

static void testAuto() {
  // Holds from motion: thresholds, both neighbours, the spread over one latent group.
  std::vector<double> m(40, 0.0);
  CHECK(autoHolds(m) == std::vector<int>(40, 1), "static clip holds 1");
  m[20] = 50.0;  // frames 19 and 20 move 50 px relative to each other
  const auto h = autoHolds(m);
  CHECK(h[19] == 4 && h[20] == 4 && h[15] == 4 && h[24] == 4 && h[14] == 1 && h[25] == 1, "spread of a 4x pair");
  m[20] = 20.0;
  CHECK(autoHolds(m)[20] == 2, "20 px -> 2x");
  m[20] = 11.0;
  CHECK(autoHolds(m)[20] == 1, "11 px -> 1x");

  for (int s = 0; s < kChunkSizes; ++s) {
    // Uniform holds: exactly the fixed-hold plan.
    for (int k : {1, 2, 4})
      for (int n : {1, 30, 200}) {
        const auto a = planAutoChunks(std::vector<int>(n, k), s);
        const auto b = planChunks(n, heldChunkLen(s, k), heldOverlap(s, k));
        bool same = a.size() == b.size();
        for (size_t j = 0; same && j < a.size(); ++j) same = a[j].start == b[j].start && a[j].len == b[j].len && a[j].hold == k;
        CHECK(same, "size %d hold %d n=%d: uniform auto != fixed plan", s, k, n);
      }
    // Structured: slow, then fast, then slow.
    for (int n : {20, 60, 150, 400}) {
      std::vector<int> v(n, 1);
      for (int f = n / 3; f < 2 * n / 3; ++f) v[f] = 4;
      checkAuto(v, s, "slow-fast-slow");
      for (int f = 0; f < n; ++f) v[f] = f % 50 < 10 ? 2 : 1;
      checkAuto(v, s, "bursts");
    }
    // Random walks of hold levels.
    unsigned seed = 7;
    for (int t = 0; t < 300; ++t) {
      seed = seed * 1103515245u + 12345u;
      const int n = 2 + int(seed % 500);
      std::vector<int> v(n);
      int k = 1;
      for (int f = 0; f < n; ++f) {
        seed = seed * 1103515245u + 12345u;
        if (seed % 23 == 0) k = (seed >> 8) % 3 == 0 ? 1 : (seed >> 8) % 3 == 1 ? 2 : 4;
        v[f] = k;
      }
      checkAuto(v, s, "random");
    }
  }
  {  // Motion drops before the middle and stays down: the 4x chunk ends at its middle (18 of 37).
    std::vector<int> v(300, 1);
    for (int f = 0; f < 10; ++f) v[f] = 4;
    auto c = planAutoChunks(v, 0);
    CHECK(c.size() >= 2 && c[0].hold == 4 && c[0].len == 18 && c[1].hold == 1,
          "early drop: first chunk %d frames at %dx", c.empty() ? 0 : c[0].len, c.empty() ? 0 : c[0].hold);
    checkAuto(v, 0, "early drop");
    // It comes back within the chunk's length: no early cut, the chunk runs to its budget (37).
    for (int f = 25; f < 30; ++f) v[f] = 4;
    c = planAutoChunks(v, 0);
    CHECK(!c.empty() && c[0].hold == 4 && c[0].len == 37, "drop and return: first chunk %d frames",
          c.empty() ? 0 : c[0].len);
    checkAuto(v, 0, "drop and return");
    // It drops after the middle: the chunk runs to its budget as before.
    std::fill(v.begin(), v.end(), 1);
    for (int f = 0; f < 25; ++f) v[f] = 4;
    c = planAutoChunks(v, 0);
    CHECK(!c.empty() && c[0].len == 37, "late drop: first chunk %d frames", c.empty() ? 0 : c[0].len);
  }
  {  // Flame test 2026-10-17 (120 frames 720p, motion per frame from the log): Auto planned 617
     // model frames, more than fixed 4x (580). Now: never more than the fixed plan.
    const double logged[] = {15, 111, 60, 33, 71, 125, 51, 28, 152, 64, 123, 25, 136, 47, 64, 99, 81, 42, 16, 30,
                             51, 37, 102, 75, 37, 34, 28, 19, 18, 21, 21, 31, 31, 13, 13, 16, 15, 18, 37, 27,
                             22, 22, 18, 15, 26, 39, 73, 61, 51, 27, 49, 39, 25, 17, 17, 20, 9, 9, 9, 9,
                             9, 11, 11, 9, 11, 13, 18, 11, 13, 13, 27, 13, 17, 15, 12, 8, 8, 8, 12, 15,
                             15, 15, 15, 17, 36, 76, 24, 24, 15, 39, 45, 30, 28, 15, 16, 18, 18, 18, 12, 16,
                             24, 24, 27, 27, 27, 22, 21, 82, 81, 21, 19, 31, 42, 21, 17, 15, 21, 15, 15};
    std::vector<double> m(1, 0.0);
    m.insert(m.end(), std::begin(logged), std::end(logged));
    CHECK(m.size() == 120, "logged clip has %zu frames", m.size());
    const auto holds = autoHolds(m);
    const auto c = planAutoChunks(holds, 0);
    auto fixed = planChunks(120, heldChunkLen(0, 4), heldOverlap(0, 4));
    for (auto& x : fixed) x.hold = 4;
    std::printf("flame clip: auto %d model frames in %zu chunks, fixed 4x %d\n", planModelFrames(c), c.size(),
                planModelFrames(fixed));
    CHECK(planModelFrames(c) <= planModelFrames(fixed), "flame clip: auto %d > fixed 4x %d", planModelFrames(c),
          planModelFrames(fixed));
    checkAuto(holds, 0, "flame clip");
  }
  {  // A short last chunk reaches back to half its full length only (4x Full: 18 of 37).
    std::vector<int> v(200, 1);
    for (int f = 190; f < 200; ++f) v[f] = 4;
    const auto c = planAutoChunks(v, 0);
    CHECK(c.back().hold == 4 && c.back().len == 18, "last chunk %d frames at %dx", c.back().len, c.back().hold);
  }
  {  // A fast middle third costs 4x only around it: 300 frames, frames 100..199 at 4x.
    std::vector<int> v(300, 1);
    for (int f = 100; f < 200; ++f) v[f] = 4;
    const auto c = planAutoChunks(v, 0);
    int model = 0;
    for (const auto& x : c) model += heldModelFrames(x.len, x.hold);
    CHECK(model < 4 * 300, "fast middle third: %d model frames, a fixed 4x plan needs ~1200", model);
    CHECK(c.front().hold == 1 && c.back().hold == 1, "slow ends stay at 1x");
  }
}

int main() {
  testAuto();
  for (int n = 1; n <= 2000; ++n) checkClip(n);
  // Chunk Size choices (decision 61) and smoothstep weights (decision 62).
  for (int s = 0; s < kChunkSizes; ++s) {
    CHECK((kSizeLen[s] - 1) % 8 == 0, "size %d is not 8n+1", kSizeLen[s]);
    for (int n = 1; n <= 700; ++n) {
      checkClip(n, kSizeLen[s], kSizeOverlap[s], false);
      checkClip(n, kSizeLen[s], kSizeOverlap[s], true);
    }
  }
  // Frame Hold (decision 74): a held chunk is exactly its size's model length, and plans stay valid.
  for (int hold : {1, 2, 4, 8}) {
    for (int s = 0; s < kChunkSizes; ++s) {
      const int L = heldChunkLen(s, hold), O = heldOverlap(s, hold);
      CHECK(heldModelFrames(L, hold) == kSizeLen[s], "hold %d size %d: %d model frames", hold, s,
            heldModelFrames(L, hold));
      CHECK(O >= 2 && O < L, "hold %d size %d: overlap %d, len %d", hold, s, O, L);
      if (hold == 1) CHECK(L == kSizeLen[s] && O == kSizeOverlap[s], "hold 1 changes size %d", s);
      for (int n = 1; n <= 300; ++n) checkClip(n, L, O, true);
    }
  }
  CHECK(heldChunkLen(0, 8) == 19 && heldChunkLen(0, 2) == 73 && heldModelFrames(1, 8) == 1, "held lengths");

  {  // smoothstep eases in and out but is symmetric around the middle
    auto c = planChunks(300);
    float lin = 0, sm = 0;
    int a = -1;
    for (int i = 0; i < 300 && a < 0; ++i)
      if (frameContributions(c, i).size() == 2) a = i;
    lin = frameContributions(c, a)[1].weight;
    sm = frameContributions(c, a, kOverlap, true)[1].weight;
    CHECK(sm < lin, "smooth weight %f should start lower than linear %f", sm, lin);
    const float l2 = frameContributions(c, a + 1)[1].weight, s2 = frameContributions(c, a + 1, kOverlap, true)[1].weight;
    CHECK(l2 > 0 && s2 > 0, "weights positive");
  }

  // Spot checks matching CLAUDE.md.
  {
    auto c = planChunks(300);
    CHECK(c.size() == 3 && c[0].start == 0 && c[1].start == 78 && c[2].start == 155,
          "n=300 plan mismatch");
  }
  {
    auto c = planChunks(100);
    CHECK(c.size() == 1 && c[0].len == 100, "n=100 should be one 100-frame chunk");
  }
  {
    auto c = planChunks(1000);
    CHECK(c.size() == 9, "n=1000 expected 9 chunks, got %zu", c.size());
  }
  // Crossfade band is O frames wide at most.
  {
    auto c = planChunks(1000);
    int blended = 0;
    for (int i = 0; i < 1000; ++i)
      if (frameContributions(c, i).size() == 2) ++blended;
    CHECK(blended <= 8 * kOverlap, "n=1000 %d blended frames", blended);
    CHECK(blended >= 8 * (kOverlap - 1), "n=1000 only %d blended frames", blended);
  }

  if (failures) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::printf("auto plans equal to the fixed plan: %d\n", gFallbacks);
  std::printf("test_chunk_plan: all checks passed\n");
  return 0;
}
