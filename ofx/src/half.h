// float <-> IEEE half conversion with F16C (every AVX-era x86 CPU has it).
// target("f16c") keeps the instruction set requirement local to these functions.
#pragma once

#include <immintrin.h>

#include <cstddef>
#include <cstdint>

namespace ltxag {

__attribute__((target("f16c,avx"))) inline void floatToHalf(const float* src, uint16_t* dst,
                                                           size_t n) {
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    const __m128i h = _mm256_cvtps_ph(_mm256_loadu_ps(src + i), _MM_FROUND_TO_NEAREST_INT);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), h);
  }
  for (; i < n; ++i) dst[i] = _cvtss_sh(src[i], _MM_FROUND_TO_NEAREST_INT);
}

__attribute__((target("f16c,avx"))) inline void halfToFloat(const uint16_t* src, float* dst,
                                                           size_t n) {
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    const __m128i h = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
    _mm256_storeu_ps(dst + i, _mm256_cvtph_ps(h));
  }
  for (; i < n; ++i) dst[i] = _cvtsh_ss(src[i]);
}

}  // namespace ltxag
