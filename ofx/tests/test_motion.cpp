// MotionAnalyzer (decision 75): a textured disc over a textured background, moving a known
// number of pixels per frame, must measure close to that speed; a static clip near 0; an ROI
// mask around a small fast subject must find it; flat frames read 0. Run via ctest.
#include <cmath>
#include <cstdio>
#include <vector>

#include "motion.h"

using namespace ltxag;

static int failures = 0;
#define CHECK(cond, ...)                                        \
  do {                                                          \
    if (!(cond)) {                                              \
      std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
      std::fprintf(stderr, __VA_ARGS__);                        \
      std::fprintf(stderr, "\n");                               \
      ++failures;                                               \
    }                                                           \
  } while (0)

// Mean measured motion over n frames of a disc moving `speed` px per frame along x (wrapping
// around a circle so it stays in frame), radius r. withMask: the ROI alpha is a loose garbage
// matte, 1.6x the disc.
static double measure(int w, int h, double speed, double r, bool withMask, double bgShift = 0.0) {
  MotionAnalyzer a(w, h);
  std::vector<float> rgb(std::size_t(w) * 3), alpha(static_cast<std::size_t>(w));
  double sum = 0.0;
  const int n = 6;
  const double path = 0.2 * h;
  for (int f = 0; f < n; ++f) {
    const double ang = speed * f / path;
    const double cx = w / 2.0 + path * std::cos(ang), cy = h / 2.0 + path * std::sin(ang);
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        const double bx = x + bgShift * f;
        const double bg = 0.3 + 0.15 * std::sin(bx / 13.0) * std::cos(y / 17.0) + 0.05 * std::sin(bx * 0.9 + y * 1.7);
        const double dx = x - cx, dy = y - cy, d = std::hypot(dx, dy);
        const double cover = std::min(1.0, std::max(0.0, r - d + 0.5));
        const double fg = 0.7 + 0.15 * std::sin((dx * 0.8 + dy * 0.6) / 5.0);
        const float v = float(bg * (1 - cover) + fg * cover);
        for (int k = 0; k < 3; ++k) rgb[size_t(x) * 3 + k] = v;
        alpha[size_t(x)] = d < 1.6 * r ? 1.0f : 0.0f;
      }
      a.addRow(y, rgb.data(), withMask ? alpha.data() : nullptr);
    }
    const double m = a.endFrame();
    if (f > 0) sum += m;
  }
  return sum / (n - 1);
}

int main() {
  // HD-sized crop (f0 = 4): precision ~4 px; coarse search covers ~160 px.
  for (double v : {0.0, 24.0, 64.0, 128.0}) {
    const double m = measure(1920, 1080, v, 240.0, false);
    std::printf("HD disc %5.0f px/frame -> %6.1f\n", v, m);
    CHECK(std::fabs(m - v) <= 0.15 * v + 8.0, "HD disc %.0f px/frame measured %.1f", v, m);
  }
  // Small crop (f0 = 1).
  for (double v : {0.0, 20.0, 60.0}) {
    const double m = measure(480, 270, v, 60.0, false);
    std::printf("480 disc %5.0f px/frame -> %6.1f\n", v, m);
    CHECK(std::fabs(m - v) <= 0.15 * v + 3.0, "480 disc %.0f px/frame measured %.1f", v, m);
  }
  // A small fast subject (r = 40 at HD, ~1 % of the frame): found with a garbage-matte ROI.
  {
    const double m = measure(1920, 1080, 64.0, 40.0, true);
    std::printf("HD small disc 64 px/frame, ROI -> %.1f\n", m);
    CHECK(m >= 40.0, "small fast subject inside a garbage matte measured %.1f", m);
  }
  // Flat frames: nothing to match, must read 0.
  {
    MotionAnalyzer a(640, 360);
    std::vector<float> rgb(640 * 3, 0.5f);
    double m = 0;
    for (int f = 0; f < 3; ++f) {
      for (int y = 0; y < 360; ++y) a.addRow(y, rgb.data(), nullptr);
      m = std::max(m, a.endFrame());
    }
    CHECK(m == 0.0, "flat frames measured %.1f", m);
  }
  if (failures) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::printf("test_motion: all checks passed\n");
  return 0;
}
