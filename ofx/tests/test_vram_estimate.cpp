// The estimate must cover every measured peak (never under-estimate) without being wildly high.
#include <cmath>
#include <cstdio>

#include "vram_estimate.h"

using namespace ltxag;

static int failures = 0;

static void covers(const char* what, bool fp8, int w, int h, int f, bool resident, double measured,
                   double maxOver) {
  const VramNeed n = estimateVramNeed(fp8, w, h, f, resident);
  const bool ok = n.gb >= measured && n.gb - measured <= maxOver;
  std::printf("%s %-34s est %5.1f GB vs measured %5.1f GB (model %dx%dx%d)\n", ok ? "ok  " : "FAIL", what, n.gb,
              measured, n.modelW, n.modelH, n.modelFrames);
  if (!ok) ++failures;
}

int main() {
  // Official CLI measurements (HD sizes as the model sees them).
  covers("HD 97 fp8", true, 1920, 1088, 97, false, 34.1, 4.0);
  covers("HD 145 fp8", true, 1920, 1088, 145, false, 41.3, 4.0);
  covers("HD 97 bf16", false, 1920, 1088, 97, false, 51.7, 4.0);
  covers("HD 145 bf16", false, 1920, 1088, 145, false, 58.8, 4.0);
  covers("UHD 49 fp8", true, 3840, 2176, 49, false, 54.1, 4.0);
  covers("UHD 49 bf16", false, 3840, 2176, 49, false, 67.8, 4.0);
  // Our worker (peak allocated + ~1.5 GB CUDA context/reserve slack).
  covers("worker HD 60->65 fp8", true, 1920, 1080, 60, false, 28.3 + 1.5, 4.0);
  covers("worker HD 60->65 bf16", false, 1920, 1080, 60, false, 45.5 + 1.5, 4.0);
  covers("worker HD 60->65 fp8 resident", true, 1920, 1080, 60, true, 37.7 + 1.5, 4.0);
  // Transformer kept (resident), measured as reserved + 0.7 GB context = what free memory lost.
  covers("A/B HD 145 fp8 resident, chunk 2", true, 1920, 1080, 145, true, 56.2 + 0.7, 4.0);
  covers("test 2: 720p 57 bf16 resident reused", false, 1280, 720, 50, true, 44.2, 4.0);
  covers("test 2: 720p 57 fp8 resident fresh", true, 1280, 720, 50, true, 28.0, 4.0);
  // Padding: 1080 -> 1088, 60 -> 65 frames, frames capped at the chunk length 145.
  const VramNeed p = estimateVramNeed(true, 1920, 1080, 300, false);
  if (p.modelH != 1088 || p.modelFrames != 145) {
    std::printf("FAIL padding %dx%d f=%d\n", p.modelW, p.modelH, p.modelFrames);
    ++failures;
  }
  // A 48 GB card next to Flame (~46.5 GB free, 5% reserve) must still take HD 145 fp8.
  if (estimateVramNeed(true, 1920, 1080, 145, false).gb > 46.5 - 2.3) {
    std::printf("FAIL HD 145 fp8 no longer fits a 48 GB card\n");
    ++failures;
  }
  std::printf(failures ? "%d failure(s)\n" : "test_vram_estimate: all checks passed\n", failures);
  return failures ? 1 : 0;
}
