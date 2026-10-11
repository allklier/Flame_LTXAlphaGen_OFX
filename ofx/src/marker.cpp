#include "marker.h"

#include <algorithm>
#include <cstdint>

namespace ltxag {

namespace {

struct Glyph {
  char c;
  uint8_t rows[7];  // 5 bits per row, MSB = leftmost column
};

const Glyph kFont[] = {
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
};

const Glyph* glyph(char c) {
  for (const auto& g : kFont)
    if (g.c == c) return &g;
  return nullptr;  // space or unsupported
}

}  // namespace

void drawMarker(std::vector<float>& matte, int w, int h, const std::string& caption) {
  if (w <= 0 || h <= 0) return;
  matte.assign(size_t(w) * h, 0.0f);

  // Diagonal stripes.
  const int period = std::max(8, h / 24);
  const int width = std::max(2, period / 4);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      if ((x + y) % period < width) matte[size_t(y) * w + x] = kMarkerStripe;

  // Centred caption on a clear band, each glyph cell 6x7 units (5 + 1 spacing).
  const int len = int(caption.size());
  if (len == 0) return;
  int s = std::max(1, h / 90);
  while (s > 1 && len * 6 * s > w * 9 / 10) --s;
  const int textW = len * 6 * s - s, textH = 7 * s;
  if (textW > w || textH > h) return;
  const int x0 = (w - textW) / 2, y0 = (h - textH) / 2;
  const int pad = 2 * s;
  for (int y = std::max(0, y0 - pad); y < std::min(h, y0 + textH + pad); ++y)
    for (int x = std::max(0, x0 - pad); x < std::min(w, x0 + textW + pad); ++x)
      matte[size_t(y) * w + x] = 0.0f;
  for (int i = 0; i < len; ++i) {
    const Glyph* g = glyph(caption[i]);
    if (!g) continue;
    for (int row = 0; row < 7; ++row)
      for (int col = 0; col < 5; ++col) {
        if (!(g->rows[row] & (0x10 >> col))) continue;
        for (int dy = 0; dy < s; ++dy)
          for (int dx = 0; dx < s; ++dx)
            matte[size_t(y0 + row * s + dy) * w + (x0 + i * 6 * s + col * s + dx)] = kMarkerText;
      }
  }
}

}  // namespace ltxag
