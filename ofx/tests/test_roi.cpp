// Plain-assert tests for roi; run via ctest.
#include <algorithm>
#include <cstdio>
#include <vector>

#include "roi.h"

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

static bool contains(const RoiBox& r, const RoiBounds& b, int margin, int W, int H) {
  const int mx = std::max(margin, kRoiMinMargin);
  return r.x <= std::max(0, b.x0 - mx) && r.y <= std::max(0, b.y0 - mx) &&
         r.x + r.w >= std::min(W, b.x1 + 1 + mx) && r.y + r.h >= std::min(H, b.y1 + 1 + mx);
}

int main() {
  // Sweep boxes and margins on HD and an odd-sized image.
  for (int W : {1920, 1000, 300}) {
    for (int H : {1080, 540, 200}) {
      for (int margin : {0, 32, 64, 300}) {
        for (int bx : {0, 5, W / 2, W - 3}) {
          for (int by : {0, 7, H / 2, H - 2}) {
            RoiBounds b;
            b.include(bx, by);
            b.include(std::min(W - 1, bx + 40), std::min(H - 1, by + 20));
            const RoiBox r = expandRoi(b, margin, W, H);
            CHECK(r.x >= 0 && r.y >= 0 && r.x + r.w <= W && r.y + r.h <= H,
                  "outside image %dx%d box %d,%d %dx%d", W, H, r.x, r.y, r.w, r.h);
            CHECK(r.w % 32 == 0 || r.w == W, "w=%d not /32 (W=%d)", r.w, W);
            CHECK(r.h % 32 == 0 || r.h == H, "h=%d not /32 (H=%d)", r.h, H);
            CHECK(r.w >= std::min(kRoiMinSide, W) && r.h >= std::min(kRoiMinSide, H),
                  "below minimum %dx%d", r.w, r.h);
            CHECK(contains(r, b, margin, W, H), "box %d,%d %dx%d misses bounds", r.x, r.y, r.w, r.h);
          }
        }
      }
    }
  }
  {  // a big object keeps its margin: 1000 wide + 2*64 -> 1128 -> 1152
    RoiBounds b;
    b.include(400, 300);
    b.include(1399, 800);
    const RoiBox r = expandRoi(b, 64, 1920, 1080);
    CHECK(r.w == 1152 && r.x == 400 - 64 - 12, "wide box x=%d w=%d", r.x, r.w);
    CHECK(r.h == 640 + 0 || r.h == 640, "tall box h=%d", r.h);  // 501+128=629 -> 640
  }
  {  // alpha scan: opaque is not a matte; a matte gives bounds
    RoiScan opaque;
    const float o[8] = {0, 0, 0, 1, 1, 1, 1, 1};
    opaque.addRow(o, 4, 2, 0);
    CHECK(!opaque.alphaVaries, "opaque alpha is not a matte");
    RoiScan m;
    const float row[12] = {0, 0, 0, 0, 0.2f, 0.2f, 0.2f, 0.5f, 1, 1, 1, 1};  // alpha 0, 0.5, 1
    m.addRow(row, 4, 3, 7);
    CHECK(m.alphaVaries && m.alpha.x0 == 1 && m.alpha.x1 == 2 && m.alpha.y0 == 7, "matte bounds");
    RoiScan rgb;
    const float c3[6] = {0, 0, 0, 1, 1, 1};
    rgb.addRow(c3, 3, 2, 0);
    CHECK(!rgb.alphaVaries && !rgb.alpha.any(), "RGB has no matte");
    float v[3];
    roiRowAlpha(row, 4, 3, v);
    CHECK(v[0] == 0.0f && v[1] == 0.5f && v[2] == 1.0f, "alpha values");
    roiRowAlpha(c3, 3, 2, v);
    CHECK(v[0] == 1.0f && v[1] == 1.0f, "RGB alpha is 1");
  }
  if (failures) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::printf("test_roi: all checks passed\n");
  return 0;
}
