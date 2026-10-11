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

std::vector<Contribution> frameContributions(const std::vector<Chunk>& chunks, int i, int O) {
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
    const double w = std::min(1.0, std::max(0.0, (double(i) - lo) / band));
    if (w > 0.0 && w < 1.0) {
      return {{j, float(1.0 - w)}, {j + 1, float(w)}};
    }
    if (w >= 1.0) owner = j + 1;
  }
  return {{owner, 1.0f}};
}

}  // namespace ltxag
