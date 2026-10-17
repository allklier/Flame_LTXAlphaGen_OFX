#include "chunk_plan.h"

#include <algorithm>
#include <cmath>

namespace ltxag {

std::vector<Chunk> planChunks(int n, int L, int O) {
  if (n <= 0) return {};
  if (n <= L) return {{0, n}};
  const int k = static_cast<int>(std::ceil(double(n - L) / double(L - O))) + 1;
  std::vector<Chunk> chunks;
  chunks.reserve(k);
  for (int j = 0; j < k; ++j) {
    const int start = static_cast<int>(std::lround(double(j) * double(n - L) / double(k - 1)));
    chunks.push_back({start, L});
  }
  return chunks;
}

std::vector<int> autoHolds(const std::vector<double>& motion) {
  const int n = static_cast<int>(motion.size());
  std::vector<int> need(n, 1), out(n, 1);
  for (int f = 0; f < n; ++f) {
    // Frame f moves relative to both neighbours.
    const double m = std::max(f > 0 ? motion[f] : 0.0, f + 1 < n ? motion[f + 1] : 0.0);
    need[f] = m >= kAutoHold4 ? 4 : m >= kAutoHold2 ? 2 : 1;
  }
  for (int f = 0; f < n; ++f)
    for (int g = std::max(0, f - kAutoSpread); g <= std::min(n - 1, f + kAutoSpread); ++g)
      out[f] = std::max(out[f], need[g]);
  return out;
}

std::vector<Chunk> planAutoChunks(const std::vector<int>& holds, int sizeIdx) {
  const int n = static_cast<int>(holds.size());
  if (n == 0) return {};
  const int budget = kSizeLen[sizeIdx], baseOverlap = kSizeOverlap[sizeIdx];
  auto maxHold = [&](int a, int b) { return *std::max_element(holds.begin() + a, holds.begin() + b + 1); };
  if (std::all_of(holds.begin(), holds.end(), [&](int k) { return k == holds[0]; })) {
    auto chunks = planChunks(n, heldChunkLen(sizeIdx, holds[0]), heldOverlap(sizeIdx, holds[0]));
    for (auto& c : chunks) c.hold = holds[0];
    return chunks;
  }
  auto overlapFor = [&](int k) { return std::max(2, baseOverlap / k); };
  std::vector<Chunk> chunks;
  int s = 0, shared = 0;  // frames at the start of this chunk that the previous one also covers
  for (;;) {
    int k = holds[s], e = s, stopAt = -1;
    while (e + 1 < n && e != stopAt) {
      const int hn = holds[e + 1], len = e - s + 1;
      if (heldModelFrames(len + 1, std::max(k, hn)) > budget) break;
      // Before a faster stretch: cut, unless this chunk is still short (then it is absorbed).
      // A frame is never in three chunks: the cut leaves room for both overlaps.
      if (hn > k && len >= std::max(kAutoMinChunk, shared + overlapFor(hn) + 1)) break;
      // A slower stretch is absorbed, so a fast stretch never ends in a tiny chunk: up to the
      // budget, or, when the motion drops before the middle of this hold's full chunk length and
      // doesn't come back to this hold within that length, up to the middle (user, 2026-10-17:
      // no 4x runtime spent on slow frames).
      if (hn < k && stopAt < 0) {
        const int full = heldChunkLen(sizeIdx, k), mid = s + full / 2 - 1;  // last frame of the half
        const int windowEnd = std::min(n - 1, s + full - 1);
        if (e + 1 <= mid + 1 && maxHold(e + 1, windowEnd) < k &&
            mid - s + 1 >= shared + overlapFor(k) + 1)
          stopAt = std::max(e, std::min(mid, n - 1));
      }
      k = std::max(k, hn);
      ++e;
    }
    chunks.push_back({s, e - s + 1, k});
    if (e == n - 1) break;
    const int ov = std::max(1, std::min(overlapFor(std::max(k, holds[e + 1])), e - s + 1 - shared - 1));
    s = e + 1 - ov;
    shared = ov;
  }
  // A short last chunk reaches back to half its full length (like the cut at the middle above),
  // so it is not a tiny leftover; further would re-process frames at its hold for little gain
  // (Flame test 2026-10-17: reaching back to the full length cost ~45 s of 298). Never before the
  // previous chunk's start, nor into the chunk before that.
  if (chunks.size() >= 2) {
    Chunk& last = chunks.back();
    int lo = chunks[chunks.size() - 2].start + 1;
    if (chunks.size() >= 3) lo = std::max(lo, chunks[chunks.size() - 3].end() + 1);
    for (;;) {
      const int k = maxHold(last.start, n - 1);
      const int start = std::max(lo, n - heldChunkLen(sizeIdx, k) / 2);
      if (start >= last.start || maxHold(start, n - 1) != k) {
        last.hold = k;
        break;
      }
      last.start = start;
      last.len = n - start;
    }
  }
  // Never dearer than the fixed plan at the highest hold found: every boundary re-processes its
  // overlap, so on a mostly fast clip the uniform plan wins (Flame test 2026-10-17: Auto 617 model
  // frames, fixed 4x 580). A tie goes to the fixed plan (fewer, evenly spread seams).
  const int top = maxHold(0, n - 1);
  auto fixed = planChunks(n, heldChunkLen(sizeIdx, top), heldOverlap(sizeIdx, top));
  for (auto& c : fixed) c.hold = top;
  if (planModelFrames(fixed) <= planModelFrames(chunks)) return fixed;
  return chunks;
}

int planModelFrames(const std::vector<Chunk>& chunks) {
  int total = 0;
  for (const auto& c : chunks) total += heldModelFrames(c.len, c.hold);
  return total;
}

std::vector<Contribution> frameContributions(const std::vector<Chunk>& chunks, int i, int O,
                                             bool smooth) {
  const int k = static_cast<int>(chunks.size());
  if (k == 0) return {};
  if (k == 1) return {{0, 1.0f}};

  // Transition j sits between chunk j and j+1, at the middle of their overlap.
  int owner = 0;
  for (int j = 0; j + 1 < k; ++j) {
    const Chunk& a = chunks[j];
    const Chunk& b = chunks[j + 1];
    const double mid = 0.5 * (double(b.start) + double(a.end()));
    const int overlap = a.end() - b.start + 1;
    const double band = double(std::max(1, std::min(O, overlap)));
    const double lo = mid - 0.5 * band;
    double w = std::min(1.0, std::max(0.0, (double(i) - lo) / band));
    if (smooth) w = w * w * (3.0 - 2.0 * w);
    if (w > 0.0 && w < 1.0) {
      return {{j, float(1.0 - w)}, {j + 1, float(w)}};
    }
    if (w >= 1.0) owner = j + 1;
  }
  return {{owner, 1.0f}};
}

}  // namespace ltxag
