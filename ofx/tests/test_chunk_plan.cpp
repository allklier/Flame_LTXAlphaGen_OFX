// Plain-assert tests for chunk_plan; run via ctest. Exits non-zero on failure.
#include <cmath>
#include <cstdio>
#include <cstdlib>

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

int main() {
  for (int n = 1; n <= 2000; ++n) checkClip(n);
  // Chunk Size choices (decision 61) and smoothstep weights (decision 62).
  for (int s = 0; s < kChunkSizes; ++s) {
    CHECK((kSizeLen[s] - 1) % 8 == 0, "size %d is not 8n+1", kSizeLen[s]);
    for (int n = 1; n <= 700; ++n) {
      checkClip(n, kSizeLen[s], kSizeOverlap[s], false);
      checkClip(n, kSizeLen[s], kSizeOverlap[s], true);
    }
  }
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
  std::printf("test_chunk_plan: all checks passed\n");
  return 0;
}
