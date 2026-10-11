// Faint "no media"-style marker drawn into the matte when a frame has no result
// (decision 40): diagonal stripes plus a centred caption such as NOT PROCESSED.
// Built-in 5x7 font covering only the letters the captions need. Pure, no OFX.
#pragma once

#include <string>
#include <vector>

namespace ltxag {

constexpr float kMarkerStripe = 0.08f;  // faint, but clearly not a real matte
constexpr float kMarkerText = 0.30f;

// matte: w*h floats, rows top-down, overwritten. Unsupported characters render as spaces.
void drawMarker(std::vector<float>& matte, int w, int h, const std::string& caption);

// Decision 58: the image output shows the source with the matte overlaid in magenta at 30%
// (50% was too opaque, user after test 4): out = src * (1 - 0.3 m) + magenta * 0.3 m, per RGB
// channel. m = 0 leaves the source as is.
constexpr float kOverlayColour[3] = {1.0f, 0.0f, 1.0f};
constexpr float kOverlayOpacity = 0.3f;
inline void overlayPixel(const float* src, float m, float* out) {
  const float o = kOverlayOpacity * m;
  for (int k = 0; k < 3; ++k) out[k] = src[k] * (1.0f - o) + kOverlayColour[k] * o;
}

}  // namespace ltxag
