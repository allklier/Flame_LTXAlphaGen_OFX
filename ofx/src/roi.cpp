#include "roi.h"

#include <algorithm>

namespace ltxag {

void RoiBounds::include(int x, int y) {
  x0 = std::min(x0, x);
  y0 = std::min(y0, y);
  x1 = std::max(x1, x);
  y1 = std::max(y1, y);
}

void RoiScan::addRow(const float* px, int comps, int w, int y) {
  if (comps != 4) return;  // no alpha: opaque, not a matte
  for (int x = 0; x < w; ++x) {
    const float a = px[size_t(x) * 4 + 3];
    if (a < 0.99f) alphaVaries = true;
    if (a > kRoiThreshold) alpha.include(x, y);
  }
}

void roiRowAlpha(const float* px, int comps, int w, float* out) {
  for (int x = 0; x < w; ++x)
    out[x] = comps == 4 ? std::min(1.0f, std::max(0.0f, px[size_t(x) * 4 + 3])) : 1.0f;
}

// [lo, hi] inclusive -> start and length along one axis of size `size`.
static void fitAxis(int lo, int hi, int margin, int size, int& start, int& len) {
  int a = std::max(0, lo - margin), b = std::min(size, hi + 1 + margin);  // [a, b)
  int want = std::max(b - a, std::min(kRoiMinSide, size));
  want = (want + kRoiMultiple - 1) / kRoiMultiple * kRoiMultiple;
  want = std::min(want, size);
  start = (a + b) / 2 - want / 2;  // centred on the grown box, then slid into the image
  start = std::max(0, std::min(start, size - want));
  len = want;
}

RoiBox expandRoi(const RoiBounds& b, int margin, int imgW, int imgH) {
  RoiBox r{};
  fitAxis(b.x0, b.x1, std::max(margin, kRoiMinMargin), imgW, r.x, r.w);
  fitAxis(b.y0, b.y1, std::max(margin, kRoiMinMargin), imgH, r.y, r.h);
  return r;
}

}  // namespace ltxag
