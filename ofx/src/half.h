// float <-> IEEE half conversion.
// x86: F16C (every AVX-era x86 CPU has it); target("f16c") keeps the instruction set
// requirement local to these functions.
// ARM64 (Apple Silicon): NEON vcvt_f16_f32 / vcvt_f32_f16 (clang's arm_neon.h), round to
// nearest even like the x86 path; __fp16 for the tail.
#pragma once

#if defined(__aarch64__)
#include <arm_neon.h>
#else
#include <immintrin.h>
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ltxag {

#if defined(__aarch64__)

inline void floatToHalf(const float* src, uint16_t* dst, size_t n) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4)
    vst1_u16(dst + i, vreinterpret_u16_f16(vcvt_f16_f32(vld1q_f32(src + i))));
  for (; i < n; ++i) {
    const __fp16 h = __fp16(src[i]);
    std::memcpy(dst + i, &h, sizeof h);
  }
}

inline void halfToFloat(const uint16_t* src, float* dst, size_t n) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4)
    vst1q_f32(dst + i, vcvt_f32_f16(vreinterpret_f16_u16(vld1_u16(src + i))));
  for (; i < n; ++i) {
    __fp16 h;
    std::memcpy(&h, src + i, sizeof h);
    dst[i] = float(h);
  }
}

#else

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

#endif

}  // namespace ltxag
