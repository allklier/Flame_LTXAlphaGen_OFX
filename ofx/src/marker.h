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

}  // namespace ltxag
