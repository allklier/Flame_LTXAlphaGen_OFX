#include "motion.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <thread>

namespace ltxag {

namespace {
constexpr int kMaxSide = 512;
constexpr int kCell = 8;             // cell size at level 0 (32 px of an HD crop)
constexpr double kReliable = 0.6;    // a moved match must cost < 60 % of no motion
constexpr double kMaxSearch = 160.0;  // crop pixels per frame covered by the coarse search
constexpr float kPenalty = 0.002f;   // per level pixel of displacement, against flat blocks
}  // namespace

MotionAnalyzer::MotionAnalyzer(int width, int height) {
  f0_ = std::max(1, (std::max(width, height) + kMaxSide - 1) / kMaxSide);
  w0_ = std::max(1, width / f0_);
  h0_ = std::max(1, height / f0_);
  sum_.assign(size_t(w0_) * h0_, 0.0f);
  mask_.assign(size_t(w0_) * h0_, 0.0f);
}

void MotionAnalyzer::addRow(int y, const float* rgb, const float* alpha) {
  const int y0 = y / f0_;
  if (y0 >= h0_) return;
  float* s = &sum_[size_t(y0) * w0_];
  float* m = &mask_[size_t(y0) * w0_];
  for (int x0 = 0; x0 < w0_; ++x0) {
    for (int k = 0; k < f0_; ++k) {
      const float* p = rgb + size_t(x0 * f0_ + k) * 3;
      const float v = std::max(0.0f, (p[0] + p[1] + p[2]) / 3.0f);
      s[x0] += v / (1.0f + v);  // tone-mapped: linear highlights don't dominate
      if (alpha) m[x0] += alpha[x0 * f0_ + k] > 0.05f ? 1.0f : 0.0f;
    }
  }
  if (alpha) hasMask_ = true;
}

MotionAnalyzer::Level MotionAnalyzer::half(const Level& a) {
  Level b;
  b.w = std::max(1, a.w / 2);
  b.h = std::max(1, a.h / 2);
  b.px.resize(size_t(b.w) * b.h);
  for (int y = 0; y < b.h; ++y)
    for (int x = 0; x < b.w; ++x) {
      const int x1 = std::min(2 * x + 1, a.w - 1), y1 = std::min(2 * y + 1, a.h - 1);
      b.px[size_t(y) * b.w + x] = 0.25f * (a.at(2 * x, 2 * y) + a.at(x1, 2 * y) + a.at(2 * x, y1) + a.at(x1, y1));
    }
  return b;
}

double MotionAnalyzer::endFrame() {
  const float n = float(f0_ * f0_);
  cur_[0].w = w0_;
  cur_[0].h = h0_;
  cur_[0].px.resize(sum_.size());
  for (size_t i = 0; i < sum_.size(); ++i) cur_[0].px[i] = sum_[i] / n;
  cur_[1] = half(cur_[0]);
  cur_[2] = half(cur_[1]);
  const double motion = havePrev_ ? blockMotion() : 0.0;
  for (int l = 0; l < 3; ++l) std::swap(prev_[l], cur_[l]);
  havePrev_ = true;
  std::fill(sum_.begin(), sum_.end(), 0.0f);
  std::fill(mask_.begin(), mask_.end(), 0.0f);  // blockMotion used this frame's coverage
  return motion;
}

double MotionAnalyzer::blockMotion() const {
  // Cost of matching the window [x, x+bw) x [y, y+bh) of the current level-l image against the
  // previous image displaced by (dx, dy): cur(p) ~ prev(p - d). Windows partly outside are clipped.
  auto cost = [&](int l, int x, int y, int bw, int bh, int dx, int dy) {
    const Level& c = cur_[l];
    const Level& p = prev_[l];
    double s = 0.0;
    int cnt = 0;
    for (int yy = std::max(0, y); yy < std::min(c.h, y + bh); ++yy) {
      const int py = yy - dy;
      if (py < 0 || py >= p.h) continue;
      for (int xx = std::max(0, x); xx < std::min(c.w, x + bw); ++xx) {
        const int px = xx - dx;
        if (px < 0 || px >= p.w) continue;
        s += std::fabs(c.at(xx, yy) - p.at(px, py));
        ++cnt;
      }
    }
    if (cnt < bw * bh / 4) return 1e9;
    return s / cnt + kPenalty * std::sqrt(double(dx * dx + dy * dy));
  };
  auto best = [&](int l, int x, int y, int bw, int bh, int cx, int cy, int r) {
    int bx = cx, by = cy;
    double bc = cost(l, x, y, bw, bh, cx, cy);
    for (int dy = cy - r; dy <= cy + r; ++dy)
      for (int dx = cx - r; dx <= cx + r; ++dx) {
        const double c = cost(l, x, y, bw, bh, dx, dy);
        if (c < bc) bc = c, bx = dx, by = dy;
      }
    return std::make_pair(bx, by);
  };

  const int r2 = std::max(2, std::min(16, int(std::ceil(kMaxSearch / (f0_ * 4.0)))));
  const int nbx = w0_ / kCell, nby = h0_ / kCell;
  // Rows of 2x2 cell groups are split across threads; each row writes only its own results.
  const int groupRows = (nby + 1) / 2;
  std::vector<std::vector<double>> allRows(groupRows), insideRows(groupRows);
  auto doRow = [&](int gy) {
    for (int gx = 0; gx < (nbx + 1) / 2; ++gx) {
      // One coarse search per 2x2 group (an 8x8 window at 1/4 around the group's centre).
      const int gcx = gx * 2 * kCell + kCell, gcy = gy * 2 * kCell + kCell;
      const auto coarse = best(2, gcx / 4 - 4, gcy / 4 - 4, 8, 8, 0, 0, r2);
      for (int by = 2 * gy; by < std::min(nby, 2 * gy + 2); ++by)
        for (int bx = 2 * gx; bx < std::min(nbx, 2 * gx + 2); ++bx) {
          // Each cell refines with windows centred on itself.
          const int cx = bx * kCell + kCell / 2, cy = by * kCell + kCell / 2;
          auto d = best(1, cx / 2 - 4, cy / 2 - 4, 8, 8, 2 * coarse.first, 2 * coarse.second, 2);
          d = best(0, cx - 6, cy - 6, 12, 12, 2 * d.first, 2 * d.second, 1);
          // Occluded or ambiguous cells (no match clearly better than none) don't count.
          if (d.first != 0 || d.second != 0) {
            const double moved = cost(0, cx - 6, cy - 6, 12, 12, d.first, d.second);
            const double still = cost(0, cx - 6, cy - 6, 12, 12, 0, 0);
            if (moved > kReliable * still) continue;
          }
          const double mag = std::hypot(double(d.first), double(d.second)) * f0_;
          allRows[gy].push_back(mag);
          if (hasMask_) {
            double cover = 0.0;
            for (int yy = by * kCell; yy < (by + 1) * kCell; ++yy)
              for (int xx = bx * kCell; xx < (bx + 1) * kCell; ++xx) cover += mask_[size_t(yy) * w0_ + xx];
            if (cover >= 0.5 * kCell * kCell * f0_ * f0_) insideRows[gy].push_back(mag);
          }
        }
    }
  };
  const int nthreads = std::max(1, std::min(16, int(std::thread::hardware_concurrency())));
  std::vector<std::thread> pool;
  for (int t = 0; t < nthreads; ++t)
    pool.emplace_back([&, t] {
      for (int gy = t; gy < groupRows; gy += nthreads) doRow(gy);
    });
  for (auto& th : pool) th.join();
  std::vector<double> all, inside;
  for (int gy = 0; gy < groupRows; ++gy) {
    all.insert(all.end(), allRows[gy].begin(), allRows[gy].end());
    inside.insert(inside.end(), insideRows[gy].begin(), insideRows[gy].end());
  }
  std::vector<double>& v = inside.empty() ? all : inside;
  if (v.empty()) return 0.0;
  const size_t k = std::min(v.size(), std::max<size_t>(3, v.size() / 50));
  std::nth_element(v.begin(), v.begin() + (k - 1), v.end(), std::greater<double>());
  return v[k - 1];
}

}  // namespace ltxag
