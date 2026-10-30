// The pitch stage's AVX loops (declared in PitchShifter.hpp), defined once here rather than inline
// in the header that every stage user includes.
#include "../src/LinkPacket.hpp" // PitchShifter.hpp uses PitchStage without including it
#include "PitchShifter.hpp"
#include <cstdlib>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#ifdef _MSC_VER
#include <intrin.h>
#define PITCH_AVX_TARGET
#else
#include <cpuid.h>
#define PITCH_AVX_TARGET __attribute__((target("avx")))
#endif

namespace pitch_simd {
// AVX in the CPU (CPUID 1, ECX bit 28) and its 256-bit registers saved by the OS: OSXSAVE (bit 27)
// and the SSE and AVX state bits (1 and 2) of XCR0.
bool available() {
    static const bool supported = [] {
#ifdef _MSC_VER
        int info[4];
        __cpuid(info, 1);
        const unsigned features = unsigned(info[2]);
#else
        unsigned a = 0, b = 0, features = 0, d = 0;
        if (!__get_cpuid(1, &a, &b, &features, &d))
            return false;
#endif
        if (!(features & (1u << 27)) || !(features & (1u << 28)))
            return false;
#ifdef _MSC_VER
        const unsigned long long enabled = _xgetbv(0);
#else
        unsigned low = 0, high = 0;
        __asm__("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
        const unsigned long long enabled = low | (static_cast<unsigned long long>(high) << 32);
#endif
        return (enabled & 6) == 6;
    }();
    return supported;
}
PITCH_AVX_TARGET void butterflies(float *lowRe, float *lowIm, float *highRe, float *highIm, size_t half,
                                  const float *cosines, const float *sines) {
    for (size_t k = 0; k < half; k += 8) {
        const __m256 hr = _mm256_loadu_ps(highRe + k), hi = _mm256_loadu_ps(highIm + k);
        const __m256 cosine = _mm256_loadu_ps(cosines + k), sine = _mm256_loadu_ps(sines + k);
        const __m256 br = _mm256_sub_ps(_mm256_mul_ps(hr, cosine), _mm256_mul_ps(hi, sine));
        const __m256 bi = _mm256_add_ps(_mm256_mul_ps(hr, sine), _mm256_mul_ps(hi, cosine));
        const __m256 lr = _mm256_loadu_ps(lowRe + k), li = _mm256_loadu_ps(lowIm + k);
        _mm256_storeu_ps(highRe + k, _mm256_sub_ps(lr, br));
        _mm256_storeu_ps(highIm + k, _mm256_sub_ps(li, bi));
        _mm256_storeu_ps(lowRe + k, _mm256_add_ps(lr, br));
        _mm256_storeu_ps(lowIm + k, _mm256_add_ps(li, bi));
    }
    _mm256_zeroupper(); // the caller's SSE code runs next
}
} // namespace pitch_simd
#else
namespace pitch_simd {
bool available() { return false; }
// available() is false off x86, so this is never called; stop loudly if that ever changes.
void butterflies(float *, float *, float *, float *, size_t, const float *, const float *) { std::abort(); }
} // namespace pitch_simd
#endif
