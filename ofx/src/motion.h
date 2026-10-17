// Motion per frame for the Auto Frame Hold (decision 75). Pure, no OFX.
// Fed row by row while Process stages the (cropped) frames: each frame is box-downscaled to a
// tone-mapped luma image of at most 512 px (factor f0), with a 1/2 and 1/4 pyramid. Cells of 8x8
// at that size (32 px of an HD crop) are matched against the previous frame coarse to fine: one
// search per 2x2 cells at 1/4 (8x8 window, up to ~160 crop px), then +-2 at 1/2 and +-1 at full
// size per cell, minimising the mean absolute difference plus a small penalty per pixel of
// displacement (flat cells stay at 0). Cells whose best match isn't clearly better than no motion
// (occluded, ambiguous) are dropped. A frame's motion is the k-th largest cell displacement in
// crop pixels, k = max(3, 2 % of the cells): a fast subject counts even when it covers a small
// part of the frame. With an ROI alpha only cells at least half inside it (alpha > 0.05) count; a
// loose garbage matte still covers the subject. Precision ~f0 px (4 at HD). Threads per frame.
#pragma once

#include <cstddef>
#include <vector>

namespace ltxag {

class MotionAnalyzer {
 public:
  MotionAnalyzer(int width, int height);
  // Row y (0 = top) of the frame being staged: rgb = width*3 floats in the source colour space;
  // alpha = width floats (the ROI matte, crop-aligned) or nullptr.
  void addRow(int y, const float* rgb, const float* alpha);
  // Ends the frame; returns its motion relative to the previous frame (0 for the first).
  double endFrame();

 private:
  struct Level {
    int w = 0, h = 0;
    std::vector<float> px;
    float at(int x, int y) const { return px[std::size_t(y) * w + x]; }
  };
  static Level half(const Level& a);
  double blockMotion() const;

  int f0_, w0_, h0_;
  std::vector<float> sum_, mask_;
  bool hasMask_ = false;
  Level cur_[3], prev_[3];
  bool havePrev_ = false;
};

}  // namespace ltxag
