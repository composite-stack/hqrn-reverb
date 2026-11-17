#pragma once

/**
 * @file HQRNKernels.hpp
 * @brief Explizite SIMD- & Skalare Rechenpfade für das Hyperbolische Quanten-Resonator-Netzwerk (HQRN)
 * 
 * Implementiert drei spezialisierte Pfade:
 * 1. Skalar (Baseline ohne Vektorisierung, für Vergleiche und Fallback)
 * 2. AVX2 + FMA (256-Bit Vektorisierung, 2x 8-wide Zustandsvektoren, In-Register FWHT-16, FMA)
 * 3. ARM NEON (128-Bit Vektorisierung, 4x 4-wide Zustandsvektoren, In-Register FWHT-16, FMA/MLA)
 * 
 * Standard: C++20
 * Portabilität: Native x86-64 (AVX2/FMA) & AArch64 (NEON), bzw. NEON via NEON_2_SSE.h auf x86.
 */

#include "HQRNReverbEngine.hpp"
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string_view>

#if defined(HQRN_FORCE_SCALAR) && (HQRN_FORCE_SCALAR != 0)
  #define HQRN_HAS_AVX2 0
  #define HQRN_HAS_NEON 0
#else
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  #if defined(__AVX2__)
    #include <immintrin.h>
    #define HQRN_HAS_AVX2 1
  #else
    #define HQRN_HAS_AVX2 0
  #endif
  #if defined(__GNUC__) || defined(__clang__)
    #include <cpuid.h>
  #elif defined(_MSC_VER)
    #include <intrin.h>
  #endif
#else
  #define HQRN_HAS_AVX2 0
#endif

#if defined(__linux__) && (defined(__arm__) || defined(_M_ARM))
  #include <sys/auxv.h>
  #include <asm/hwcap.h>
#endif

#if defined(__ARM_NEON) || defined(__aarch64__) || defined(_M_ARM64)
    #include <arm_neon.h>
    #define HQRN_HAS_NEON 1
#elif __has_include("NEON_2_SSE.h") && defined(__AVX2__) && defined(__FMA__)
    #ifndef NEON2SSE_DISABLE_PERFORMANCE_WARNING
        #define NEON2SSE_DISABLE_PERFORMANCE_WARNING
    #endif
    #ifndef USE_AVX2
        #define USE_AVX2
    #endif
    #if defined(__GNUC__) || defined(__clang__)
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wcomment"
        #pragma GCC diagnostic ignored "-Wunused-function"
        #pragma GCC diagnostic ignored "-Wunused-parameter"
    #endif
    #include "NEON_2_SSE.h"
    #if defined(__GNUC__) || defined(__clang__)
        #pragma GCC diagnostic pop
    #endif
    #define HQRN_HAS_NEON 1
#else
    #define HQRN_HAS_NEON 0
#endif
#endif // !HQRN_FORCE_SCALAR

namespace dsp::hqrn {

// ============================================================================
// LAUFZEIT-CPU-ERKENNUNG & SIMD-PFAD-AUSWAHL
// ============================================================================

enum class CpuSimdPath : uint8_t {
    Scalar = 0,
    AVX2   = 1,
    NEON   = 2
};

struct CpuCapabilities {
    bool hasAvx2{false};
    bool hasFma{false};
    bool hasNeon{false};

    [[nodiscard]] constexpr bool operator==(const CpuCapabilities&) const noexcept = default;
};

inline constexpr std::string_view getCpuSimdPathName(CpuSimdPath path) noexcept {
    switch (path) {
        case CpuSimdPath::AVX2:   return "AVX2 (256-Bit)";
        case CpuSimdPath::NEON:   return "ARM NEON (128-Bit)";
        case CpuSimdPath::Scalar:
        default:                  return "Skalar";
    }
}

class CpuDetector {
public:
    [[nodiscard]] static CpuCapabilities detectCapabilities() noexcept {
        CpuCapabilities caps;
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  #if defined(__GNUC__) || defined(__clang__)
        __builtin_cpu_init();
        caps.hasAvx2 = __builtin_cpu_supports("avx2") > 0;
        caps.hasFma  = __builtin_cpu_supports("fma") > 0;
  #elif defined(_MSC_VER)
        int info[4];
        __cpuid(info, 1);
        bool osxsave = (info[2] & (1 << 27)) != 0;
        caps.hasFma  = (info[2] & (1 << 12)) != 0;
        bool avx     = (info[2] & (1 << 28)) != 0;
        if (osxsave && caps.hasFma && avx) {
            unsigned long long xcr0 = _xgetbv(0);
            if ((xcr0 & 0x6) == 0x6) {
                __cpuidex(info, 7, 0);
                caps.hasAvx2 = (info[1] & (1 << 5)) != 0;
            }
        }
  #endif
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
        caps.hasNeon = true;
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
        caps.hasNeon = true;
#elif defined(__linux__) && (defined(__arm__) || defined(_M_ARM))
        unsigned long hwcap = getauxval(AT_HWCAP);
        caps.hasNeon = (hwcap & HWCAP_NEON) != 0;
#endif
        return caps;
    }

    [[nodiscard]] static bool hasAVX2() noexcept {
        const auto caps = detectCapabilities();
        return caps.hasAvx2 && caps.hasFma;
    }

    [[nodiscard]] static bool hasNativeNEON() noexcept {
        return detectCapabilities().hasNeon;
    }

    [[nodiscard]] static CpuSimdPath selectPath(const CpuCapabilities& caps) noexcept {
        if (caps.hasAvx2 && caps.hasFma) {
            return CpuSimdPath::AVX2;
        }
        if (caps.hasNeon) {
            return CpuSimdPath::NEON;
        }
        return CpuSimdPath::Scalar;
    }

    [[nodiscard]] static CpuSimdPath detectBestPath() noexcept {
        return selectPath(detectCapabilities());
    }
};


inline constexpr std::array<float, 8> E0_HALF = {
    -0.0003019409f,  0.0013999166f, -0.0042661702f,  0.0104049809f,
    -0.0222122293f,  0.0445282752f, -0.0934614170f,  0.3138971703f
};

// ============================================================================
// HILFSFUNKTIONEN FÜR HORIZONTALE VEKTOR-REDUKTIONEN
// ============================================================================

#if HQRN_HAS_AVX2
[[nodiscard]] inline float hsum_avx(__m256 v) noexcept {
    __m128 low = _mm256_castps256_ps128(v);
    __m128 high = _mm256_extractf128_ps(v, 1);
    __m128 sum128 = _mm_add_ps(low, high);
    __m128 shuf = _mm_movehdup_ps(sum128);
    __m128 sums = _mm_add_ps(sum128, shuf);
    shuf = _mm_movehl_ps(shuf, sums);
    sums = _mm_add_ss(sums, shuf);
    return _mm_cvtss_f32(sums);
}
#endif

#if HQRN_HAS_NEON
[[nodiscard]] inline float hsum_neon(float32x4_t v) noexcept {
#if defined(__aarch64__)
    return vaddvq_f32(v);
#else
    float32x2_t sum2 = vadd_f32(vget_low_f32(v), vget_high_f32(v));
    sum2 = vpadd_f32(sum2, sum2);
    return vget_lane_f32(sum2, 0);
#endif
}
#endif

// ============================================================================
// IN-REGISTER FAST WALSH-HADAMARD TRANSFORMATION (FWHT-16)
// ============================================================================

/**
 * AVX2 In-Register FWHT-16
 * 16 Zustandselemente in zwei 8-wide Vektoren v0 = [0..7], v1 = [8..15].
 * Führt alle 4 Butterfly-Stufen vollständig in Registern ohne Speicherzugriffe aus.
 */
#if HQRN_HAS_AVX2
inline void fwht16_avx2(__m256& v0, __m256& v1) noexcept {
    // Stufe 1: Abstand 1 (Nachbarelemente)
    __m256 u0 = _mm256_moveldup_ps(v0);
    __m256 w0 = _mm256_movehdup_ps(v0);
    __m256 s1_add0 = _mm256_add_ps(u0, w0);
    __m256 s1_sub0 = _mm256_sub_ps(u0, w0);
    __m256 st1_0 = _mm256_blend_ps(s1_add0, s1_sub0, 0xAA);

    __m256 u1 = _mm256_moveldup_ps(v1);
    __m256 w1 = _mm256_movehdup_ps(v1);
    __m256 s1_add1 = _mm256_add_ps(u1, w1);
    __m256 s1_sub1 = _mm256_sub_ps(u1, w1);
    __m256 st1_1 = _mm256_blend_ps(s1_add1, s1_sub1, 0xAA);

    // Stufe 2: Abstand 2 (Paare 0-1 mit 2-3)
    __m256 u2_0 = _mm256_shuffle_ps(st1_0, st1_0, _MM_SHUFFLE(1, 0, 1, 0));
    __m256 w2_0 = _mm256_shuffle_ps(st1_0, st1_0, _MM_SHUFFLE(3, 2, 3, 2));
    __m256 s2_add0 = _mm256_add_ps(u2_0, w2_0);
    __m256 s2_sub0 = _mm256_sub_ps(u2_0, w2_0);
    __m256 st2_0 = _mm256_blend_ps(s2_add0, s2_sub0, 0xCC);

    __m256 u2_1 = _mm256_shuffle_ps(st1_1, st1_1, _MM_SHUFFLE(1, 0, 1, 0));
    __m256 w2_1 = _mm256_shuffle_ps(st1_1, st1_1, _MM_SHUFFLE(3, 2, 3, 2));
    __m256 s2_add1 = _mm256_add_ps(u2_1, w2_1);
    __m256 s2_sub1 = _mm256_sub_ps(u2_1, w2_1);
    __m256 st2_1 = _mm256_blend_ps(s2_add1, s2_sub1, 0xCC);

    // Stufe 3: Abstand 4 (zwischen 128-Bit Lanes)
    __m256 low0 = _mm256_permute2f128_ps(st2_0, st2_0, 0x00);
    __m256 high0 = _mm256_permute2f128_ps(st2_0, st2_0, 0x11);
    __m256 s3_add0 = _mm256_add_ps(low0, high0);
    __m256 s3_sub0 = _mm256_sub_ps(low0, high0);
    __m256 st3_0 = _mm256_blend_ps(s3_add0, s3_sub0, 0xF0);

    __m256 low1 = _mm256_permute2f128_ps(st2_1, st2_1, 0x00);
    __m256 high1 = _mm256_permute2f128_ps(st2_1, st2_1, 0x11);
    __m256 s3_add1 = _mm256_add_ps(low1, high1);
    __m256 s3_sub1 = _mm256_sub_ps(low1, high1);
    __m256 st3_1 = _mm256_blend_ps(s3_add1, s3_sub1, 0xF0);

    // Stufe 4: Abstand 8 (zwischen st3_0 und st3_1)
    __m256 fin0 = _mm256_add_ps(st3_0, st3_1);
    __m256 fin1 = _mm256_sub_ps(st3_0, st3_1);

    const __m256 scale = _mm256_set1_ps(0.25f);
    v0 = _mm256_mul_ps(fin0, scale);
    v1 = _mm256_mul_ps(fin1, scale);
}
#endif

/**
 * ARM NEON In-Register FWHT-16
 * 16 Zustandselemente in vier 4-wide Vektoren v0=[0..3], v1=[4..7], v2=[8..11], v3=[12..15].
 */
#if HQRN_HAS_NEON
inline void fwht16_neon(float32x4_t& v0, float32x4_t& v1, float32x4_t& v2, float32x4_t& v3) noexcept {
    // Stufe 1: Abstand 1 (vtrnq)
    auto step1 = [](float32x4_t v) noexcept {
        float32x4x2_t trn = vtrnq_f32(v, v);
        float32x4_t add = vaddq_f32(trn.val[0], trn.val[1]);
        float32x4_t sub = vsubq_f32(trn.val[0], trn.val[1]);
        return vtrnq_f32(add, sub).val[0];
    };
    v0 = step1(v0);
    v1 = step1(v1);
    v2 = step1(v2);
    v3 = step1(v3);

    // Stufe 2: Abstand 2 (Kombination aus low/high 64-Bit)
    auto step2 = [](float32x4_t v) noexcept {
        float32x4_t low = vcombine_f32(vget_low_f32(v), vget_low_f32(v));
        float32x4_t high = vcombine_f32(vget_high_f32(v), vget_high_f32(v));
        float32x4_t add = vaddq_f32(low, high);
        float32x4_t sub = vsubq_f32(low, high);
        return vcombine_f32(vget_low_f32(add), vget_low_f32(sub));
    };
    v0 = step2(v0);
    v1 = step2(v1);
    v2 = step2(v2);
    v3 = step2(v3);

    // Stufe 3: Abstand 4 (v0 mit v1, v2 mit v3)
    float32x4_t t0 = vaddq_f32(v0, v1);
    float32x4_t t1 = vsubq_f32(v0, v1);
    float32x4_t t2 = vaddq_f32(v2, v3);
    float32x4_t t3 = vsubq_f32(v2, v3);

    // Stufe 4: Abstand 8 (t0..t1 mit t2..t3)
    float32x4_t f0 = vaddq_f32(t0, t2);
    float32x4_t f1 = vaddq_f32(t1, t3);
    float32x4_t f2 = vsubq_f32(t0, t2);
    float32x4_t f3 = vsubq_f32(t1, t3);

    float32x4_t scale = vdupq_n_f32(0.25f);
    v0 = vmulq_f32(f0, scale);
    v1 = vmulq_f32(f1, scale);
    v2 = vmulq_f32(f2, scale);
    v3 = vmulq_f32(f3, scale);
}
#endif

// ============================================================================
// 1. SKALARER RECHENPFAD (BASELINE)
// ============================================================================

class HQRNKernelScalar {
public:
    HQRNKernelScalar() noexcept : nodes_(createNodeLayout()) {}

    void prepare(float sampleRate) noexcept {
        sampleRate_ = sampleRate;
        reset();
        updateModes();
        updateSourceInjection();
        updateListenerExtraction();
    }

    void reset() noexcept {
        q_.fill(0.0f);
        p_.fill(0.0f);
    }

    void setBaseFrequency(float fBaseHz) noexcept {
        baseFreqHz_ = std::clamp(fBaseHz, 20.0f, 1000.0f);
        updateModes();
    }

    void setReverbTime(float t60Sec) noexcept {
        t60Sec_ = std::clamp(t60Sec, 0.1f, 30.0f);
        updateModes();
        updateListenerExtraction();
    }

    void setSourcePosition(DiskPoint zS) noexcept {
        sourcePos_ = zS.clamped(0.95f);
        updateSourceInjection();
    }

    void setListenerPosition(DiskPoint zL, float headingRad = 0.0f) noexcept {
        listenerPos_ = zL.clamped(0.95f);
        listenerHeading_ = headingRad;
        updateListenerExtraction();
    }

    // Explizit un-vektorisierte Schleifen für faire Skalar-Baseline
    #if defined(__GNUC__) || defined(__clang__)
    __attribute__((optimize("no-tree-vectorize")))
    #endif
    inline void processSample(float inL, float inR, float& outL, float& outR) noexcept {
        // 1. Phasendrehung & Dämpfung
        for (int u = 0; u < 16; ++u) {
            float qu = q_[u];
            float pu = p_[u];
            float cosT = cosTheta_[u];
            float sinT = sinTheta_[u];
            float damp = gamma_[u];

            float qRot = cosT * qu - sinT * pu;
            float pRot = sinT * qu + cosT * pu;

            q_[u] = damp * qRot;
            p_[u] = damp * pRot;
        }

        // 2. Hadamard-Kopplung
        fwht16(q_.data());
        fwht16(p_.data());

        // 3. Audio-Injektion
        for (int u = 0; u < 16; ++u) {
            p_[u] += bL_[u] * inL + bR_[u] * inR;
        }

        // 4. Stereo-Auskopplung
        float accL = 0.0f;
        float accR = 0.0f;
        for (int u = 0; u < 16; ++u) {
            accL += cL_[u] * p_[u];
            accR += cR_[u] * p_[u];
        }

        outL = accL;
        outR = accR;
    }

private:
    void updateModes() noexcept {
        if (sampleRate_ <= 0.0f) return;
        float sumGammaSq = 0.0f;
        for (int u = 0; u < 16; ++u) {
            float r_j = std::sqrt(std::max(0.0f, BOLZA_LAMBDA[u] - 0.25f));
            float freqHz = baseFreqHz_ * (r_j / std::sqrt(BOLZA_LAMBDA[0] - 0.25f));
            freqHz = std::min(freqHz, 0.48f * sampleRate_);
            float theta = 2.0f * pi_v<float> * freqHz / sampleRate_;
            cosTheta_[u] = std::cos(theta);
            sinTheta_[u] = std::sin(theta);
            float gamma = std::pow(10.0f, -3.0f / (sampleRate_ * t60Sec_));
            gamma_[u] = gamma;
            sumGammaSq += gamma * gamma;
        }
        meanGammaSq_ = sumGammaSq / 16.0f;
    }

    void updateSourceInjection() noexcept {
        constexpr float eps = 0.12f;
        std::array<float, 16> rawW{};
        float sumWSq = 0.0f;
        for (int u = 0; u < 16; ++u) {
            float dH = hyperbolicDistance(sourcePos_, nodes_[u]);
            float w = 1.0f / std::sqrt(std::sinh(dH + eps));
            rawW[u] = w;
            sumWSq += w * w;
        }
        float normW = (sumWSq > 1e-12f) ? (1.0f / std::sqrt(sumWSq)) : 0.25f;
        for (int u = 0; u < 16; ++u) {
            float wNorm = rawW[u] * normW;
            float psi = tangentAzimuth(sourcePos_, nodes_[u]);
            bL_[u] = wNorm * std::cos(0.5f * psi + pi_v<float> / 4.0f);
            bR_[u] = wNorm * std::sin(0.5f * psi + pi_v<float> / 4.0f);
        }
    }

    void updateListenerExtraction() noexcept {
        constexpr float eps = 0.12f;
        std::array<float, 16> rawV{};
        float sumVSq = 0.0f;
        for (int u = 0; u < 16; ++u) {
            float dH = hyperbolicDistance(listenerPos_, nodes_[u]);
            float v = 1.0f / std::sqrt(std::sinh(dH + eps));
            rawV[u] = v;
            sumVSq += v * v;
        }
        float normV = (sumVSq > 1e-12f) ? (1.0f / std::sqrt(sumVSq)) : 0.25f;
        float gRad = std::sqrt(std::max(1e-6f, 32.0f * (1.0f - meanGammaSq_)));
        for (int u = 0; u < 16; ++u) {
            float vNorm = rawV[u] * normV;
            float psi = tangentAzimuth(listenerPos_, nodes_[u]) - listenerHeading_;
            cL_[u] = gRad * vNorm * std::cos(0.5f * psi + pi_v<float> / 4.0f);
            cR_[u] = gRad * vNorm * std::sin(0.5f * psi + pi_v<float> / 4.0f);
        }
    }

    float sampleRate_{48000.0f};
    float baseFreqHz_{120.0f};
    float t60Sec_{1.5f};
    float meanGammaSq_{0.999f};
    DiskPoint sourcePos_{0.0f, 0.0f};
    DiskPoint listenerPos_{0.0f, 0.0f};
    float listenerHeading_{0.0f};
    const std::array<DiskPoint, 16> nodes_;

    std::array<float, 16> q_{};
    std::array<float, 16> p_{};
    std::array<float, 16> cosTheta_{};
    std::array<float, 16> sinTheta_{};
    std::array<float, 16> gamma_{};
    std::array<float, 16> bL_{};
    std::array<float, 16> bR_{};
    std::array<float, 16> cL_{};
    std::array<float, 16> cR_{};
};

// ============================================================================
// 2. AVX2 + FMA RECHENPFAD (256-BIT)
// ============================================================================

#if HQRN_HAS_AVX2
class HQRNKernelAVX2 {
public:
    HQRNKernelAVX2() noexcept : nodes_(createNodeLayout()) {}

    void prepare(float sampleRate) noexcept {
        sampleRate_ = sampleRate;
        reset();
        updateModes();
        updateSourceInjection();
        updateListenerExtraction();
    }

    void reset() noexcept {
        q0_ = _mm256_setzero_ps();
        q1_ = _mm256_setzero_ps();
        p0_ = _mm256_setzero_ps();
        p1_ = _mm256_setzero_ps();
    }

    void setBaseFrequency(float fBaseHz) noexcept {
        baseFreqHz_ = std::clamp(fBaseHz, 20.0f, 1000.0f);
        updateModes();
    }

    void setReverbTime(float t60Sec) noexcept {
        t60Sec_ = std::clamp(t60Sec, 0.1f, 30.0f);
        updateModes();
        updateListenerExtraction();
    }

    void setSourcePosition(DiskPoint zS) noexcept {
        sourcePos_ = zS.clamped(0.95f);
        updateSourceInjection();
    }

    void setListenerPosition(DiskPoint zL, float headingRad = 0.0f) noexcept {
        listenerPos_ = zL.clamped(0.95f);
        listenerHeading_ = headingRad;
        updateListenerExtraction();
    }

    inline void processSample(float inL, float inR, float& outL, float& outR) noexcept {
        // 1. Phasendrehung & Dämpfung via FMA
        __m256 qRot0 = _mm256_fmsub_ps(cosTheta0_, q0_, _mm256_mul_ps(sinTheta0_, p0_));
        __m256 pRot0 = _mm256_fmadd_ps(sinTheta0_, q0_, _mm256_mul_ps(cosTheta0_, p0_));
        q0_ = _mm256_mul_ps(gamma0_, qRot0);
        p0_ = _mm256_mul_ps(gamma0_, pRot0);

        __m256 qRot1 = _mm256_fmsub_ps(cosTheta1_, q1_, _mm256_mul_ps(sinTheta1_, p1_));
        __m256 pRot1 = _mm256_fmadd_ps(sinTheta1_, q1_, _mm256_mul_ps(cosTheta1_, p1_));
        q1_ = _mm256_mul_ps(gamma1_, qRot1);
        p1_ = _mm256_mul_ps(gamma1_, pRot1);

        // 2. In-Register Hadamard-Kopplung (FWHT-16)
        fwht16_avx2(q0_, q1_);
        fwht16_avx2(p0_, p1_);

        // 3. Audio-Injektion B(z_S) via FMA
        __m256 vInL = _mm256_set1_ps(inL);
        __m256 vInR = _mm256_set1_ps(inR);
        p0_ = _mm256_fmadd_ps(bL0_, vInL, _mm256_fmadd_ps(bR0_, vInR, p0_));
        p1_ = _mm256_fmadd_ps(bL1_, vInL, _mm256_fmadd_ps(bR1_, vInR, p1_));

        // 4. Stereo-Auskopplung C(z_L) via FMA & horizontale Summe
        __m256 dotL = _mm256_fmadd_ps(cL1_, p1_, _mm256_mul_ps(cL0_, p0_));
        __m256 dotR = _mm256_fmadd_ps(cR1_, p1_, _mm256_mul_ps(cR0_, p0_));

        outL = hsum_avx(dotL);
        outR = hsum_avx(dotR);
    }

private:
    void updateModes() noexcept {
        if (sampleRate_ <= 0.0f) return;
        alignas(32) std::array<float, 16> cosT{}, sinT{}, g{};
        float sumGammaSq = 0.0f;
        for (int u = 0; u < 16; ++u) {
            float r_j = std::sqrt(std::max(0.0f, BOLZA_LAMBDA[u] - 0.25f));
            float freqHz = baseFreqHz_ * (r_j / std::sqrt(BOLZA_LAMBDA[0] - 0.25f));
            freqHz = std::min(freqHz, 0.48f * sampleRate_);
            float theta = 2.0f * pi_v<float> * freqHz / sampleRate_;
            cosT[u] = std::cos(theta);
            sinT[u] = std::sin(theta);
            float gamma = std::pow(10.0f, -3.0f / (sampleRate_ * t60Sec_));
            g[u] = gamma;
            sumGammaSq += gamma * gamma;
        }
        meanGammaSq_ = sumGammaSq / 16.0f;

        cosTheta0_ = _mm256_load_ps(&cosT[0]);
        cosTheta1_ = _mm256_load_ps(&cosT[8]);
        sinTheta0_ = _mm256_load_ps(&sinT[0]);
        sinTheta1_ = _mm256_load_ps(&sinT[8]);
        gamma0_    = _mm256_load_ps(&g[0]);
        gamma1_    = _mm256_load_ps(&g[8]);
    }

    void updateSourceInjection() noexcept {
        constexpr float eps = 0.12f;
        alignas(32) std::array<float, 16> rawW{}, bL{}, bR{};
        float sumWSq = 0.0f;
        for (int u = 0; u < 16; ++u) {
            float dH = hyperbolicDistance(sourcePos_, nodes_[u]);
            float w = 1.0f / std::sqrt(std::sinh(dH + eps));
            rawW[u] = w;
            sumWSq += w * w;
        }
        float normW = (sumWSq > 1e-12f) ? (1.0f / std::sqrt(sumWSq)) : 0.25f;
        for (int u = 0; u < 16; ++u) {
            float wNorm = rawW[u] * normW;
            float psi = tangentAzimuth(sourcePos_, nodes_[u]);
            bL[u] = wNorm * std::cos(0.5f * psi + pi_v<float> / 4.0f);
            bR[u] = wNorm * std::sin(0.5f * psi + pi_v<float> / 4.0f);
        }

        bL0_ = _mm256_load_ps(&bL[0]);
        bL1_ = _mm256_load_ps(&bL[8]);
        bR0_ = _mm256_load_ps(&bR[0]);
        bR1_ = _mm256_load_ps(&bR[8]);
    }

    void updateListenerExtraction() noexcept {
        constexpr float eps = 0.12f;
        alignas(32) std::array<float, 16> rawV{}, cL{}, cR{};
        float sumVSq = 0.0f;
        for (int u = 0; u < 16; ++u) {
            float dH = hyperbolicDistance(listenerPos_, nodes_[u]);
            float v = 1.0f / std::sqrt(std::sinh(dH + eps));
            rawV[u] = v;
            sumVSq += v * v;
        }
        float normV = (sumVSq > 1e-12f) ? (1.0f / std::sqrt(sumVSq)) : 0.25f;
        float gRad = std::sqrt(std::max(1e-6f, 32.0f * (1.0f - meanGammaSq_)));
        for (int u = 0; u < 16; ++u) {
            float vNorm = rawV[u] * normV;
            float psi = tangentAzimuth(listenerPos_, nodes_[u]) - listenerHeading_;
            cL[u] = gRad * vNorm * std::cos(0.5f * psi + pi_v<float> / 4.0f);
            cR[u] = gRad * vNorm * std::sin(0.5f * psi + pi_v<float> / 4.0f);
        }

        cL0_ = _mm256_load_ps(&cL[0]);
        cL1_ = _mm256_load_ps(&cL[8]);
        cR0_ = _mm256_load_ps(&cR[0]);
        cR1_ = _mm256_load_ps(&cR[8]);
    }

    float sampleRate_{48000.0f};
    float baseFreqHz_{120.0f};
    float t60Sec_{1.5f};
    float meanGammaSq_{0.999f};
    DiskPoint sourcePos_{0.0f, 0.0f};
    DiskPoint listenerPos_{0.0f, 0.0f};
    float listenerHeading_{0.0f};
    const std::array<DiskPoint, 16> nodes_;

    __m256 q0_{}, q1_{};
    __m256 p0_{}, p1_{};
    __m256 cosTheta0_{}, cosTheta1_{};
    __m256 sinTheta0_{}, sinTheta1_{};
    __m256 gamma0_{}, gamma1_{};
    __m256 bL0_{}, bL1_{};
    __m256 bR0_{}, bR1_{};
    __m256 cL0_{}, cL1_{};
    __m256 cR0_{}, cR1_{};
};
#else
using HQRNKernelAVX2 = HQRNKernelScalar;
#endif

// ============================================================================
// 3. ARM NEON RECHENPFAD (128-BIT)
// ============================================================================

#if HQRN_HAS_NEON
class HQRNKernelNEON {
public:
    HQRNKernelNEON() noexcept : nodes_(createNodeLayout()) {}

    void prepare(float sampleRate) noexcept {
        sampleRate_ = sampleRate;
        reset();
        updateModes();
        updateSourceInjection();
        updateListenerExtraction();
    }

    void reset() noexcept {
        q0_ = vdupq_n_f32(0.0f); q1_ = vdupq_n_f32(0.0f);
        q2_ = vdupq_n_f32(0.0f); q3_ = vdupq_n_f32(0.0f);
        p0_ = vdupq_n_f32(0.0f); p1_ = vdupq_n_f32(0.0f);
        p2_ = vdupq_n_f32(0.0f); p3_ = vdupq_n_f32(0.0f);
    }

    void setBaseFrequency(float fBaseHz) noexcept {
        baseFreqHz_ = std::clamp(fBaseHz, 20.0f, 1000.0f);
        updateModes();
    }

    void setReverbTime(float t60Sec) noexcept {
        t60Sec_ = std::clamp(t60Sec, 0.1f, 30.0f);
        updateModes();
        updateListenerExtraction();
    }

    void setSourcePosition(DiskPoint zS) noexcept {
        sourcePos_ = zS.clamped(0.95f);
        updateSourceInjection();
    }

    void setListenerPosition(DiskPoint zL, float headingRad = 0.0f) noexcept {
        listenerPos_ = zL.clamped(0.95f);
        listenerHeading_ = headingRad;
        updateListenerExtraction();
    }

    inline void processSample(float inL, float inR, float& outL, float& outR) noexcept {
        // 1. Phasendrehung & Dämpfung via NEON vmlsq / vmlaq
        auto rotateDamp = [](float32x4_t& q, float32x4_t& p,
                             float32x4_t cosT, float32x4_t sinT, float32x4_t damp) noexcept {
            float32x4_t qRot = vmlsq_f32(vmulq_f32(cosT, q), sinT, p);
            float32x4_t pRot = vmlaq_f32(vmulq_f32(cosT, p), sinT, q);
            q = vmulq_f32(damp, qRot);
            p = vmulq_f32(damp, pRot);
        };

        rotateDamp(q0_, p0_, cosTheta0_, sinTheta0_, gamma0_);
        rotateDamp(q1_, p1_, cosTheta1_, sinTheta1_, gamma1_);
        rotateDamp(q2_, p2_, cosTheta2_, sinTheta2_, gamma2_);
        rotateDamp(q3_, p3_, cosTheta3_, sinTheta3_, gamma3_);

        // 2. In-Register Hadamard-Kopplung (FWHT-16)
        fwht16_neon(q0_, q1_, q2_, q3_);
        fwht16_neon(p0_, p1_, p2_, p3_);

        // 3. Audio-Injektion B(z_S) via vmlaq_n
        p0_ = vmlaq_n_f32(vmlaq_n_f32(p0_, bR0_, inR), bL0_, inL);
        p1_ = vmlaq_n_f32(vmlaq_n_f32(p1_, bR1_, inR), bL1_, inL);
        p2_ = vmlaq_n_f32(vmlaq_n_f32(p2_, bR2_, inR), bL2_, inL);
        p3_ = vmlaq_n_f32(vmlaq_n_f32(p3_, bR3_, inR), bL3_, inL);

        // 4. Stereo-Auskopplung C(z_L) via vmlaq & horizontale Reduktion
        float32x4_t sumL = vmulq_f32(cL0_, p0_);
        sumL = vmlaq_f32(sumL, cL1_, p1_);
        sumL = vmlaq_f32(sumL, cL2_, p2_);
        sumL = vmlaq_f32(sumL, cL3_, p3_);

        float32x4_t sumR = vmulq_f32(cR0_, p0_);
        sumR = vmlaq_f32(sumR, cR1_, p1_);
        sumR = vmlaq_f32(sumR, cR2_, p2_);
        sumR = vmlaq_f32(sumR, cR3_, p3_);

        outL = hsum_neon(sumL);
        outR = hsum_neon(sumR);
    }

private:
    void updateModes() noexcept {
        if (sampleRate_ <= 0.0f) return;
        alignas(16) std::array<float, 16> cosT{}, sinT{}, g{};
        float sumGammaSq = 0.0f;
        for (int u = 0; u < 16; ++u) {
            float r_j = std::sqrt(std::max(0.0f, BOLZA_LAMBDA[u] - 0.25f));
            float freqHz = baseFreqHz_ * (r_j / std::sqrt(BOLZA_LAMBDA[0] - 0.25f));
            freqHz = std::min(freqHz, 0.48f * sampleRate_);
            float theta = 2.0f * pi_v<float> * freqHz / sampleRate_;
            cosT[u] = std::cos(theta);
            sinT[u] = std::sin(theta);
            float gamma = std::pow(10.0f, -3.0f / (sampleRate_ * t60Sec_));
            g[u] = gamma;
            sumGammaSq += gamma * gamma;
        }
        meanGammaSq_ = sumGammaSq / 16.0f;

        cosTheta0_ = vld1q_f32(&cosT[0]);  cosTheta1_ = vld1q_f32(&cosT[4]);
        cosTheta2_ = vld1q_f32(&cosT[8]);  cosTheta3_ = vld1q_f32(&cosT[12]);
        sinTheta0_ = vld1q_f32(&sinT[0]);  sinTheta1_ = vld1q_f32(&sinT[4]);
        sinTheta2_ = vld1q_f32(&sinT[8]);  sinTheta3_ = vld1q_f32(&sinT[12]);
        gamma0_    = vld1q_f32(&g[0]);     gamma1_    = vld1q_f32(&g[4]);
        gamma2_    = vld1q_f32(&g[8]);     gamma3_    = vld1q_f32(&g[12]);
    }

    void updateSourceInjection() noexcept {
        constexpr float eps = 0.12f;
        alignas(16) std::array<float, 16> rawW{}, bL{}, bR{};
        float sumWSq = 0.0f;
        for (int u = 0; u < 16; ++u) {
            float dH = hyperbolicDistance(sourcePos_, nodes_[u]);
            float w = 1.0f / std::sqrt(std::sinh(dH + eps));
            rawW[u] = w;
            sumWSq += w * w;
        }
        float normW = (sumWSq > 1e-12f) ? (1.0f / std::sqrt(sumWSq)) : 0.25f;
        for (int u = 0; u < 16; ++u) {
            float wNorm = rawW[u] * normW;
            float psi = tangentAzimuth(sourcePos_, nodes_[u]);
            bL[u] = wNorm * std::cos(0.5f * psi + pi_v<float> / 4.0f);
            bR[u] = wNorm * std::sin(0.5f * psi + pi_v<float> / 4.0f);
        }

        bL0_ = vld1q_f32(&bL[0]);  bL1_ = vld1q_f32(&bL[4]);
        bL2_ = vld1q_f32(&bL[8]);  bL3_ = vld1q_f32(&bL[12]);
        bR0_ = vld1q_f32(&bR[0]);  bR1_ = vld1q_f32(&bR[4]);
        bR2_ = vld1q_f32(&bR[8]);  bR3_ = vld1q_f32(&bR[12]);
    }

    void updateListenerExtraction() noexcept {
        constexpr float eps = 0.12f;
        alignas(16) std::array<float, 16> rawV{}, cL{}, cR{};
        float sumVSq = 0.0f;
        for (int u = 0; u < 16; ++u) {
            float dH = hyperbolicDistance(listenerPos_, nodes_[u]);
            float v = 1.0f / std::sqrt(std::sinh(dH + eps));
            rawV[u] = v;
            sumVSq += v * v;
        }
        float normV = (sumVSq > 1e-12f) ? (1.0f / std::sqrt(sumVSq)) : 0.25f;
        float gRad = std::sqrt(std::max(1e-6f, 32.0f * (1.0f - meanGammaSq_)));
        for (int u = 0; u < 16; ++u) {
            float vNorm = rawV[u] * normV;
            float psi = tangentAzimuth(listenerPos_, nodes_[u]) - listenerHeading_;
            cL[u] = gRad * vNorm * std::cos(0.5f * psi + pi_v<float> / 4.0f);
            cR[u] = gRad * vNorm * std::sin(0.5f * psi + pi_v<float> / 4.0f);
        }

        cL0_ = vld1q_f32(&cL[0]);  cL1_ = vld1q_f32(&cL[4]);
        cL2_ = vld1q_f32(&cL[8]);  cL3_ = vld1q_f32(&cL[12]);
        cR0_ = vld1q_f32(&cR[0]);  cR1_ = vld1q_f32(&cR[4]);
        cR2_ = vld1q_f32(&cR[8]);  cR3_ = vld1q_f32(&cR[12]);
    }

    float sampleRate_{48000.0f};
    float baseFreqHz_{120.0f};
    float t60Sec_{1.5f};
    float meanGammaSq_{0.999f};
    DiskPoint sourcePos_{0.0f, 0.0f};
    DiskPoint listenerPos_{0.0f, 0.0f};
    float listenerHeading_{0.0f};
    const std::array<DiskPoint, 16> nodes_;

    float32x4_t q0_{}, q1_{}, q2_{}, q3_{};
    float32x4_t p0_{}, p1_{}, p2_{}, p3_{};
    float32x4_t cosTheta0_{}, cosTheta1_{}, cosTheta2_{}, cosTheta3_{};
    float32x4_t sinTheta0_{}, sinTheta1_{}, sinTheta2_{}, sinTheta3_{};
    float32x4_t gamma0_{}, gamma1_{}, gamma2_{}, gamma3_{};
    float32x4_t bL0_{}, bL1_{}, bL2_{}, bL3_{};
    float32x4_t bR0_{}, bR1_{}, bR2_{}, bR3_{};
    float32x4_t cL0_{}, cL1_{}, cL2_{}, cL3_{};
    float32x4_t cR0_{}, cR1_{}, cR2_{}, cR3_{};
};
#else
using HQRNKernelNEON = HQRNKernelScalar;
#endif

// ============================================================================
// 4. BLOCKWEISE VEKTORISIERTE POLYPHASEN-FIR-FALTUNG & TRUE-PEAK CLIPPER
// ============================================================================

/**
 * 1. Skalares blockweises 2x-Halbbandfilter (kontinuierliche Puffer ohne Ring-Modulo).
 */
class BlockHalfbandScalar {
public:
    void reset() noexcept {
        histUp_.fill(0.0f);
        histDown0_.fill(0.0f);
        histDown1_.fill(0.0f);
    }

    void upsampleBlock(const float* in, float* outInterleaved, size_t numFrames) noexcept {
        float ext[1024 + 32];
        std::memcpy(ext, histUp_.data(), 15 * sizeof(float));
        std::memcpy(ext + 15, in, numFrames * sizeof(float));

        for (size_t m = 0; m < numFrames; ++m) {
            float acc = 0.0f;
            for (int k = 0; k < 8; ++k) {
                acc += E0_HALF[k] * (ext[15 + m - k] + ext[m + k]);
            }
            outInterleaved[2 * m]     = 2.0f * acc;
            outInterleaved[2 * m + 1] = ext[m + 8];
        }
        std::memcpy(histUp_.data(), ext + numFrames, 15 * sizeof(float));
    }

    void downsampleBlock(const float* inInterleaved, float* out, size_t numFrames) noexcept {
        float ext0[1024 + 32];
        float ext1[1024 + 32];
        std::memcpy(ext0, histDown0_.data(), 15 * sizeof(float));
        std::memcpy(ext1, histDown1_.data(), 15 * sizeof(float));

        for (size_t i = 0; i < numFrames; ++i) {
            ext0[15 + i] = inInterleaved[2 * i];
            ext1[15 + i] = inInterleaved[2 * i + 1];
        }

        for (size_t m = 0; m < numFrames; ++m) {
            float acc = 0.0f;
            for (int k = 0; k < 8; ++k) {
                acc += E0_HALF[k] * (ext0[15 + m - k] + ext0[m + k]);
            }
            out[m] = acc + 0.5f * ext1[m + 7];
        }
        std::memcpy(histDown0_.data(), ext0 + numFrames, 15 * sizeof(float));
        std::memcpy(histDown1_.data(), ext1 + numFrames, 15 * sizeof(float));
    }

private:
    std::array<float, 15> histUp_{};
    std::array<float, 15> histDown0_{};
    std::array<float, 15> histDown1_{};
};

/**
 * 2. AVX2 blockweises 2x-Halbbandfilter mit In-Register FMA & Shuffle-Interleaving.
 */
#if HQRN_HAS_AVX2
class BlockHalfbandAVX2 {
public:
    void reset() noexcept {
        histUp_.fill(0.0f);
        histDown0_.fill(0.0f);
        histDown1_.fill(0.0f);
    }

    void upsampleBlock(const float* in, float* outInterleaved, size_t numFrames) noexcept {
        alignas(32) float ext[1024 + 32];
        std::memcpy(ext, histUp_.data(), 15 * sizeof(float));
        std::memcpy(ext + 15, in, numFrames * sizeof(float));
        std::memset(ext + 15 + numFrames, 0, 16 * sizeof(float));

        size_t m = 0;
        for (; m + 8 <= numFrames; m += 8) {
            __m256 acc = _mm256_setzero_ps();
            for (int k = 0; k < 8; ++k) {
                __m256 tap = _mm256_set1_ps(E0_HALF[k]);
                __m256 v0 = _mm256_loadu_ps(ext + 15 + m - k);
                __m256 v1 = _mm256_loadu_ps(ext + m + k);
                acc = _mm256_fmadd_ps(tap, _mm256_add_ps(v0, v1), acc);
            }
            __m256 out0 = _mm256_add_ps(acc, acc);
            __m256 out1 = _mm256_loadu_ps(ext + m + 8);

            __m256 lo = _mm256_unpacklo_ps(out0, out1);
            __m256 hi = _mm256_unpackhi_ps(out0, out1);
            __m256 r0 = _mm256_permute2f128_ps(lo, hi, 0x20);
            __m256 r1 = _mm256_permute2f128_ps(lo, hi, 0x31);

            _mm256_storeu_ps(outInterleaved + 2 * m, r0);
            _mm256_storeu_ps(outInterleaved + 2 * m + 8, r1);
        }

        for (; m < numFrames; ++m) {
            float acc = 0.0f;
            for (int k = 0; k < 8; ++k) {
                acc += E0_HALF[k] * (ext[15 + m - k] + ext[m + k]);
            }
            outInterleaved[2 * m]     = 2.0f * acc;
            outInterleaved[2 * m + 1] = ext[m + 8];
        }
        std::memcpy(histUp_.data(), ext + numFrames, 15 * sizeof(float));
    }

    void downsampleBlock(const float* inInterleaved, float* out, size_t numFrames) noexcept {
        alignas(32) float ext0[1024 + 32];
        alignas(32) float ext1[1024 + 32];
        std::memcpy(ext0, histDown0_.data(), 15 * sizeof(float));
        std::memcpy(ext1, histDown1_.data(), 15 * sizeof(float));

        size_t i = 0;
        for (; i + 8 <= numFrames; i += 8) {
            __m256 vA = _mm256_loadu_ps(inInterleaved + 2 * i);
            __m256 vB = _mm256_loadu_ps(inInterleaved + 2 * i + 8);
            __m256 shufEvens = _mm256_shuffle_ps(vA, vB, _MM_SHUFFLE(2, 0, 2, 0));
            __m256 shufOdds  = _mm256_shuffle_ps(vA, vB, _MM_SHUFFLE(3, 1, 3, 1));

            __m256d permEvens = _mm256_permute4x64_pd(_mm256_castps_pd(shufEvens), _MM_SHUFFLE(3, 1, 2, 0));
            __m256d permOdds  = _mm256_permute4x64_pd(_mm256_castps_pd(shufOdds),  _MM_SHUFFLE(3, 1, 2, 0));

            _mm256_storeu_ps(ext0 + 15 + i, _mm256_castpd_ps(permEvens));
            _mm256_storeu_ps(ext1 + 15 + i, _mm256_castpd_ps(permOdds));
        }
        for (; i < numFrames; ++i) {
            ext0[15 + i] = inInterleaved[2 * i];
            ext1[15 + i] = inInterleaved[2 * i + 1];
        }

        std::memset(ext0 + 15 + numFrames, 0, 16 * sizeof(float));
        std::memset(ext1 + 15 + numFrames, 0, 16 * sizeof(float));

        size_t m = 0;
        const __m256 half = _mm256_set1_ps(0.5f);
        for (; m + 8 <= numFrames; m += 8) {
            __m256 acc = _mm256_setzero_ps();
            for (int k = 0; k < 8; ++k) {
                __m256 tap = _mm256_set1_ps(E0_HALF[k]);
                __m256 v0 = _mm256_loadu_ps(ext0 + 15 + m - k);
                __m256 v1 = _mm256_loadu_ps(ext0 + m + k);
                acc = _mm256_fmadd_ps(tap, _mm256_add_ps(v0, v1), acc);
            }
            __m256 vDelay = _mm256_loadu_ps(ext1 + m + 7);
            acc = _mm256_fmadd_ps(half, vDelay, acc);
            _mm256_storeu_ps(out + m, acc);
        }

        for (; m < numFrames; ++m) {
            float acc = 0.0f;
            for (int k = 0; k < 8; ++k) {
                acc += E0_HALF[k] * (ext0[15 + m - k] + ext0[m + k]);
            }
            out[m] = acc + 0.5f * ext1[m + 7];
        }

        std::memcpy(histDown0_.data(), ext0 + numFrames, 15 * sizeof(float));
        std::memcpy(histDown1_.data(), ext1 + numFrames, 15 * sizeof(float));
    }

private:
    std::array<float, 15> histUp_{};
    std::array<float, 15> histDown0_{};
    std::array<float, 15> histDown1_{};
};
#else
using BlockHalfbandAVX2 = BlockHalfbandScalar;
#endif

/**
 * 3. ARM NEON blockweises 2x-Halbbandfilter mit Structured Loads/Stores (vld2q/vst2q).
 */
#if HQRN_HAS_NEON
class BlockHalfbandNEON {
public:
    void reset() noexcept {
        histUp_.fill(0.0f);
        histDown0_.fill(0.0f);
        histDown1_.fill(0.0f);
    }

    void upsampleBlock(const float* in, float* outInterleaved, size_t numFrames) noexcept {
        alignas(16) float ext[1024 + 32];
        std::memcpy(ext, histUp_.data(), 15 * sizeof(float));
        std::memcpy(ext + 15, in, numFrames * sizeof(float));
        std::memset(ext + 15 + numFrames, 0, 16 * sizeof(float));

        size_t m = 0;
        for (; m + 4 <= numFrames; m += 4) {
            float32x4_t acc = vdupq_n_f32(0.0f);
            for (int k = 0; k < 8; ++k) {
                float32x4_t tap = vdupq_n_f32(E0_HALF[k]);
                float32x4_t v0 = vld1q_f32(ext + 15 + m - k);
                float32x4_t v1 = vld1q_f32(ext + m + k);
                acc = vmlaq_f32(acc, tap, vaddq_f32(v0, v1));
            }
            float32x4_t out0 = vaddq_f32(acc, acc);
            float32x4_t out1 = vld1q_f32(ext + m + 8);

            float32x4x2_t pair;
            pair.val[0] = out0;
            pair.val[1] = out1;
            vst2q_f32(outInterleaved + 2 * m, pair);
        }

        for (; m < numFrames; ++m) {
            float acc = 0.0f;
            for (int k = 0; k < 8; ++k) {
                acc += E0_HALF[k] * (ext[15 + m - k] + ext[m + k]);
            }
            outInterleaved[2 * m]     = 2.0f * acc;
            outInterleaved[2 * m + 1] = ext[m + 8];
        }
        std::memcpy(histUp_.data(), ext + numFrames, 15 * sizeof(float));
    }

    void downsampleBlock(const float* inInterleaved, float* out, size_t numFrames) noexcept {
        alignas(16) float ext0[1024 + 32];
        alignas(16) float ext1[1024 + 32];
        std::memcpy(ext0, histDown0_.data(), 15 * sizeof(float));
        std::memcpy(ext1, histDown1_.data(), 15 * sizeof(float));

        size_t i = 0;
        for (; i + 4 <= numFrames; i += 4) {
            float32x4x2_t pair = vld2q_f32(inInterleaved + 2 * i);
            vst1q_f32(ext0 + 15 + i, pair.val[0]);
            vst1q_f32(ext1 + 15 + i, pair.val[1]);
        }
        for (; i < numFrames; ++i) {
            ext0[15 + i] = inInterleaved[2 * i];
            ext1[15 + i] = inInterleaved[2 * i + 1];
        }

        std::memset(ext0 + 15 + numFrames, 0, 16 * sizeof(float));
        std::memset(ext1 + 15 + numFrames, 0, 16 * sizeof(float));

        size_t m = 0;
        float32x4_t half = vdupq_n_f32(0.5f);
        for (; m + 4 <= numFrames; m += 4) {
            float32x4_t acc = vdupq_n_f32(0.0f);
            for (int k = 0; k < 8; ++k) {
                float32x4_t tap = vdupq_n_f32(E0_HALF[k]);
                float32x4_t v0 = vld1q_f32(ext0 + 15 + m - k);
                float32x4_t v1 = vld1q_f32(ext0 + m + k);
                acc = vmlaq_f32(acc, tap, vaddq_f32(v0, v1));
            }
            float32x4_t vDelay = vld1q_f32(ext1 + m + 7);
            acc = vmlaq_f32(acc, half, vDelay);
            vst1q_f32(out + m, acc);
        }

        for (; m < numFrames; ++m) {
            float acc = 0.0f;
            for (int k = 0; k < 8; ++k) {
                acc += E0_HALF[k] * (ext0[15 + m - k] + ext0[m + k]);
            }
            out[m] = acc + 0.5f * ext1[m + 7];
        }

        std::memcpy(histDown0_.data(), ext0 + numFrames, 15 * sizeof(float));
        std::memcpy(histDown1_.data(), ext1 + numFrames, 15 * sizeof(float));
    }

private:
    std::array<float, 15> histUp_{};
    std::array<float, 15> histDown0_{};
    std::array<float, 15> histDown1_{};
};
#else
using BlockHalfbandNEON = BlockHalfbandScalar;
#endif

// ============================================================================
// LAUFZEIT-DISPATCHER: BLOCKWEISER FIR-HALBBAND-FILTER
// ============================================================================

class BlockHalfbandAuto {
public:
    explicit BlockHalfbandAuto(CpuSimdPath path = CpuDetector::detectBestPath()) noexcept
        : path_(path) {}

    explicit BlockHalfbandAuto(const CpuCapabilities& caps) noexcept
        : BlockHalfbandAuto(CpuDetector::selectPath(caps)) {}

    void setPath(CpuSimdPath path) noexcept {
        path_ = path;
    }

    [[nodiscard]] CpuSimdPath getPath() const noexcept {
        return path_;
    }

    [[nodiscard]] std::string_view getPathName() const noexcept {
        return getCpuSimdPathName(path_);
    }

    void applyCapabilities(const CpuCapabilities& caps) noexcept {
        setPath(CpuDetector::selectPath(caps));
    }

    void reset() noexcept {
        scalar_.reset();
        avx2_.reset();
        neon_.reset();
    }

    void upsampleBlock(const float* in, float* outInterleaved, size_t numFrames) noexcept {
        switch (path_) {
            case CpuSimdPath::AVX2:   avx2_.upsampleBlock(in, outInterleaved, numFrames); break;
            case CpuSimdPath::NEON:   neon_.upsampleBlock(in, outInterleaved, numFrames); break;
            case CpuSimdPath::Scalar:
            default:                  scalar_.upsampleBlock(in, outInterleaved, numFrames); break;
        }
    }

    void downsampleBlock(const float* inInterleaved, float* out, size_t numFrames) noexcept {
        switch (path_) {
            case CpuSimdPath::AVX2:   avx2_.downsampleBlock(inInterleaved, out, numFrames); break;
            case CpuSimdPath::NEON:   neon_.downsampleBlock(inInterleaved, out, numFrames); break;
            case CpuSimdPath::Scalar:
            default:                  scalar_.downsampleBlock(inInterleaved, out, numFrames); break;
        }
    }

    BlockHalfbandScalar& scalarFilter() noexcept { return scalar_; }
    BlockHalfbandAVX2&   avx2Filter()   noexcept { return avx2_; }
    BlockHalfbandNEON&   neonFilter()   noexcept { return neon_; }

private:
    CpuSimdPath path_{CpuSimdPath::Scalar};
    alignas(32) BlockHalfbandScalar scalar_;
    alignas(32) BlockHalfbandAVX2   avx2_;
    alignas(16) BlockHalfbandNEON   neon_;
};

/**
 * Skalarer blockweiser 4x True-Peak Soft-Clipper.
 */
class TruePeakClipperScalar {
public:
    void prepare(float sampleRate) noexcept {
        sampleRate_ = sampleRate;
        updateKnee();
        stage1UpL_.reset(); stage1UpR_.reset();
        stage2UpL_.reset(); stage2UpR_.reset();
        stage1DownL_.reset(); stage1DownR_.reset();
        stage2DownL_.reset(); stage2DownR_.reset();
    }

    void setCeilingDb(float ceilingDb) noexcept {
        ceilingLinear_ = std::pow(10.0f, ceilingDb / 20.0f);
        updateKnee();
    }

    void setKneeDb(float kneeDb) noexcept {
        kneeDb_ = std::max(0.0f, kneeDb);
        updateKnee();
    }

    void setEnabled(bool enabled) noexcept { enabled_ = enabled; }
    [[nodiscard]] bool isEnabled() const noexcept { return enabled_; }

    [[nodiscard]] inline float saturate(float x) const noexcept {
        float absX = std::abs(x);
        if (absX <= threshLinear_) return x;
        if (absX >= ceilingLinear_) return std::copysign(ceilingLinear_, x);
        float t = (absX - threshLinear_) / (ceilingLinear_ - threshLinear_);
        float smoothT = t * t * (3.0f - 2.0f * t);
        float satMag = threshLinear_ + smoothT * (ceilingLinear_ - threshLinear_);
        return std::copysign(satMag, x);
    }

    inline void processStereo(float inL, float inR, float& outL, float& outR) noexcept {
        processBlock(&inL, &inR, &outL, &outR, 1);
    }

    void processBlock(const float* inL, const float* inR, float* outL, float* outR, size_t numFrames) noexcept {
        if (!enabled_) {
            std::memcpy(outL, inL, numFrames * sizeof(float));
            std::memcpy(outR, inR, numFrames * sizeof(float));
            return;
        }

        constexpr size_t MAX_CHUNK = 512;
        alignas(32) float l2[1024], r2[1024];
        alignas(32) float l4[2048], r4[2048];

        for (size_t offset = 0; offset < numFrames; offset += MAX_CHUNK) {
            size_t n = std::min(MAX_CHUNK, numFrames - offset);
            const float* curInL = inL + offset;
            const float* curInR = inR + offset;
            float* curOutL = outL + offset;
            float* curOutR = outR + offset;

            stage1UpL_.upsampleBlock(curInL, l2, n);
            stage1UpR_.upsampleBlock(curInR, r2, n);

            stage2UpL_.upsampleBlock(l2, l4, 2 * n);
            stage2UpR_.upsampleBlock(r2, r4, 2 * n);

            size_t num4x = 4 * n;
            for (size_t i = 0; i < num4x; ++i) {
                l4[i] = saturate(l4[i]);
                r4[i] = saturate(r4[i]);
            }

            stage2DownL_.downsampleBlock(l4, l2, 2 * n);
            stage2DownR_.downsampleBlock(r4, r2, 2 * n);

            stage1DownL_.downsampleBlock(l2, curOutL, n);
            stage1DownR_.downsampleBlock(r2, curOutR, n);

            for (size_t i = 0; i < n; ++i) {
                curOutL[i] = saturate(curOutL[i]);
                curOutR[i] = saturate(curOutR[i]);
            }
        }
    }

private:
    void updateKnee() noexcept {
        float kneeFactor = std::pow(10.0f, -kneeDb_ / 20.0f);
        threshLinear_ = ceilingLinear_ * kneeFactor;
    }

    float sampleRate_{48000.0f};
    float ceilingLinear_{0.988553f};
    float kneeDb_{3.0f};
    float threshLinear_{0.70f};
    bool enabled_{true};

    BlockHalfbandScalar stage1UpL_, stage1UpR_;
    BlockHalfbandScalar stage2UpL_, stage2UpR_;
    BlockHalfbandScalar stage2DownL_, stage2DownR_;
    BlockHalfbandScalar stage1DownL_, stage1DownR_;
};

/**
 * AVX2-beschleunigter blockweiser 4x True-Peak Soft-Clipper.
 * Vektorisiert sowohl die 4x/2x Polyphasen-FIR-Faltung als auch die C2 Hermite-Sättigung.
 */
#if HQRN_HAS_AVX2
class TruePeakClipperAVX2 {
public:
    void prepare(float sampleRate) noexcept {
        sampleRate_ = sampleRate;
        updateKnee();
        stage1UpL_.reset(); stage1UpR_.reset();
        stage2UpL_.reset(); stage2UpR_.reset();
        stage1DownL_.reset(); stage1DownR_.reset();
        stage2DownL_.reset(); stage2DownR_.reset();
    }

    void setCeilingDb(float ceilingDb) noexcept {
        ceilingLinear_ = std::pow(10.0f, ceilingDb / 20.0f);
        updateKnee();
    }

    void setKneeDb(float kneeDb) noexcept {
        kneeDb_ = std::max(0.0f, kneeDb);
        updateKnee();
    }

    void setEnabled(bool enabled) noexcept { enabled_ = enabled; }
    [[nodiscard]] bool isEnabled() const noexcept { return enabled_; }

    inline void processStereo(float inL, float inR, float& outL, float& outR) noexcept {
        processBlock(&inL, &inR, &outL, &outR, 1);
    }

    void processBlock(const float* inL, const float* inR, float* outL, float* outR, size_t numFrames) noexcept {
        if (!enabled_) {
            std::memcpy(outL, inL, numFrames * sizeof(float));
            std::memcpy(outR, inR, numFrames * sizeof(float));
            return;
        }

        constexpr size_t MAX_CHUNK = 512;
        alignas(32) float l2[1024], r2[1024];
        alignas(32) float l4[2048], r4[2048];

        for (size_t offset = 0; offset < numFrames; offset += MAX_CHUNK) {
            size_t n = std::min(MAX_CHUNK, numFrames - offset);
            const float* curInL = inL + offset;
            const float* curInR = inR + offset;
            float* curOutL = outL + offset;
            float* curOutR = outR + offset;

            stage1UpL_.upsampleBlock(curInL, l2, n);
            stage1UpR_.upsampleBlock(curInR, r2, n);

            stage2UpL_.upsampleBlock(l2, l4, 2 * n);
            stage2UpR_.upsampleBlock(r2, r4, 2 * n);

            size_t num4x = 4 * n;
            size_t i = 0;
            for (; i + 8 <= num4x; i += 8) {
                __m256 vL = _mm256_loadu_ps(l4 + i);
                __m256 vR = _mm256_loadu_ps(r4 + i);
                _mm256_storeu_ps(l4 + i, saturate_avx2(vL));
                _mm256_storeu_ps(r4 + i, saturate_avx2(vR));
            }
            for (; i < num4x; ++i) {
                l4[i] = saturateScalar(l4[i]);
                r4[i] = saturateScalar(r4[i]);
            }

            stage2DownL_.downsampleBlock(l4, l2, 2 * n);
            stage2DownR_.downsampleBlock(r4, r2, 2 * n);

            stage1DownL_.downsampleBlock(l2, curOutL, n);
            stage1DownR_.downsampleBlock(r2, curOutR, n);

            size_t j = 0;
            for (; j + 8 <= n; j += 8) {
                __m256 vL = _mm256_loadu_ps(curOutL + j);
                __m256 vR = _mm256_loadu_ps(curOutR + j);
                _mm256_storeu_ps(curOutL + j, saturate_avx2(vL));
                _mm256_storeu_ps(curOutR + j, saturate_avx2(vR));
            }
            for (; j < n; ++j) {
                curOutL[j] = saturateScalar(curOutL[j]);
                curOutR[j] = saturateScalar(curOutR[j]);
            }
        }
    }

private:
    [[nodiscard]] inline __m256 saturate_avx2(__m256 x) const noexcept {
        const __m256 signMask = _mm256_set1_ps(-0.0f);
        __m256 sign = _mm256_and_ps(x, signMask);
        __m256 absX = _mm256_andnot_ps(signMask, x);

        __m256 diff = _mm256_sub_ps(absX, vThresh_);
        __m256 t = _mm256_mul_ps(diff, vInvKnee_);
        t = _mm256_max_ps(_mm256_setzero_ps(), _mm256_min_ps(_mm256_set1_ps(1.0f), t));

        const __m256 three = _mm256_set1_ps(3.0f);
        const __m256 two = _mm256_set1_ps(2.0f);
        __m256 factor = _mm256_fnmadd_ps(two, t, three);
        __m256 t2 = _mm256_mul_ps(t, t);
        __m256 smoothT = _mm256_mul_ps(t2, factor);

        __m256 satMag = _mm256_fmadd_ps(smoothT, vKneeWidth_, vThresh_);

        __m256 maskLinear = _mm256_cmp_ps(absX, vThresh_, _CMP_LE_OQ);
        __m256 maskCeiling = _mm256_cmp_ps(absX, vCeiling_, _CMP_GE_OQ);

        __m256 mag = _mm256_blendv_ps(satMag, absX, maskLinear);
        mag = _mm256_blendv_ps(mag, vCeiling_, maskCeiling);

        return _mm256_or_ps(mag, sign);
    }

    [[nodiscard]] inline float saturateScalar(float x) const noexcept {
        float absX = std::abs(x);
        if (absX <= threshLinear_) return x;
        if (absX >= ceilingLinear_) return std::copysign(ceilingLinear_, x);
        float t = (absX - threshLinear_) / (ceilingLinear_ - threshLinear_);
        float smoothT = t * t * (3.0f - 2.0f * t);
        float satMag = threshLinear_ + smoothT * (ceilingLinear_ - threshLinear_);
        return std::copysign(satMag, x);
    }

    void updateKnee() noexcept {
        float kneeFactor = std::pow(10.0f, -kneeDb_ / 20.0f);
        threshLinear_ = ceilingLinear_ * kneeFactor;
        kneeWidth_ = ceilingLinear_ - threshLinear_;
        invKnee_ = 1.0f / kneeWidth_;

        vCeiling_   = _mm256_set1_ps(ceilingLinear_);
        vThresh_    = _mm256_set1_ps(threshLinear_);
        vKneeWidth_ = _mm256_set1_ps(kneeWidth_);
        vInvKnee_   = _mm256_set1_ps(invKnee_);
    }

    float sampleRate_{48000.0f};
    float ceilingLinear_{0.988553f};
    float kneeDb_{3.0f};
    float threshLinear_{0.70f};
    float kneeWidth_{0.288553f};
    float invKnee_{3.465568f};
    bool enabled_{true};

    __m256 vCeiling_{}, vThresh_{}, vKneeWidth_{}, vInvKnee_{};

    BlockHalfbandAVX2 stage1UpL_, stage1UpR_;
    BlockHalfbandAVX2 stage2UpL_, stage2UpR_;
    BlockHalfbandAVX2 stage2DownL_, stage2DownR_;
    BlockHalfbandAVX2 stage1DownL_, stage1DownR_;
};
#else
using TruePeakClipperAVX2 = TruePeakClipperScalar;
#endif

/**
 * ARM NEON-beschleunigter blockweiser 4x True-Peak Soft-Clipper.
 * Vektorisiert die Polyphasen-FIR-Faltung mit Structured Loads (vld2q/vst2q)
 * und die C2 Hermite-Sättigung mit 128-Bit-NEON-Instruktionen.
 */
#if HQRN_HAS_NEON
class TruePeakClipperNEON {
public:
    void prepare(float sampleRate) noexcept {
        sampleRate_ = sampleRate;
        updateKnee();
        stage1UpL_.reset(); stage1UpR_.reset();
        stage2UpL_.reset(); stage2UpR_.reset();
        stage1DownL_.reset(); stage1DownR_.reset();
        stage2DownL_.reset(); stage2DownR_.reset();
    }

    void setCeilingDb(float ceilingDb) noexcept {
        ceilingLinear_ = std::pow(10.0f, ceilingDb / 20.0f);
        updateKnee();
    }

    void setKneeDb(float kneeDb) noexcept {
        kneeDb_ = std::max(0.0f, kneeDb);
        updateKnee();
    }

    void setEnabled(bool enabled) noexcept { enabled_ = enabled; }
    [[nodiscard]] bool isEnabled() const noexcept { return enabled_; }

    inline void processStereo(float inL, float inR, float& outL, float& outR) noexcept {
        processBlock(&inL, &inR, &outL, &outR, 1);
    }

    void processBlock(const float* inL, const float* inR, float* outL, float* outR, size_t numFrames) noexcept {
        if (!enabled_) {
            std::memcpy(outL, inL, numFrames * sizeof(float));
            std::memcpy(outR, inR, numFrames * sizeof(float));
            return;
        }

        constexpr size_t MAX_CHUNK = 512;
        alignas(16) float l2[1024], r2[1024];
        alignas(16) float l4[2048], r4[2048];

        for (size_t offset = 0; offset < numFrames; offset += MAX_CHUNK) {
            size_t n = std::min(MAX_CHUNK, numFrames - offset);
            const float* curInL = inL + offset;
            const float* curInR = inR + offset;
            float* curOutL = outL + offset;
            float* curOutR = outR + offset;

            stage1UpL_.upsampleBlock(curInL, l2, n);
            stage1UpR_.upsampleBlock(curInR, r2, n);

            stage2UpL_.upsampleBlock(l2, l4, 2 * n);
            stage2UpR_.upsampleBlock(r2, r4, 2 * n);

            size_t num4x = 4 * n;
            size_t i = 0;
            for (; i + 4 <= num4x; i += 4) {
                float32x4_t vL = vld1q_f32(l4 + i);
                float32x4_t vR = vld1q_f32(r4 + i);
                vst1q_f32(l4 + i, saturate_neon(vL));
                vst1q_f32(r4 + i, saturate_neon(vR));
            }
            for (; i < num4x; ++i) {
                l4[i] = saturateScalar(l4[i]);
                r4[i] = saturateScalar(r4[i]);
            }

            stage2DownL_.downsampleBlock(l4, l2, 2 * n);
            stage2DownR_.downsampleBlock(r4, r2, 2 * n);

            stage1DownL_.downsampleBlock(l2, curOutL, n);
            stage1DownR_.downsampleBlock(r2, curOutR, n);

            size_t j = 0;
            for (; j + 4 <= n; j += 4) {
                float32x4_t vL = vld1q_f32(curOutL + j);
                float32x4_t vR = vld1q_f32(curOutR + j);
                vst1q_f32(curOutL + j, saturate_neon(vL));
                vst1q_f32(curOutR + j, saturate_neon(vR));
            }
            for (; j < n; ++j) {
                curOutL[j] = saturateScalar(curOutL[j]);
                curOutR[j] = saturateScalar(curOutR[j]);
            }
        }
    }

private:
    [[nodiscard]] inline float32x4_t saturate_neon(float32x4_t x) const noexcept {
        float32x4_t absX = vabsq_f32(x);

        float32x4_t diff = vsubq_f32(absX, vThresh_);
        float32x4_t t = vmulq_f32(diff, vInvKnee_);
        t = vmaxq_f32(vdupq_n_f32(0.0f), vminq_f32(vdupq_n_f32(1.0f), t));

        float32x4_t three = vdupq_n_f32(3.0f);
        float32x4_t two = vdupq_n_f32(2.0f);
        float32x4_t factor = vmlsq_f32(three, two, t);
        float32x4_t t2 = vmulq_f32(t, t);
        float32x4_t smoothT = vmulq_f32(t2, factor);

        float32x4_t satMag = vmlaq_f32(vThresh_, smoothT, vKneeWidth_);

        uint32x4_t maskLinear = vcleq_f32(absX, vThresh_);
        uint32x4_t maskCeiling = vcgeq_f32(absX, vCeiling_);

        float32x4_t mag = vbslq_f32(maskLinear, absX, satMag);
        mag = vbslq_f32(maskCeiling, vCeiling_, mag);

        uint32x4_t signMask = vdupq_n_u32(0x80000000u);
        uint32x4_t signBits = vandq_u32(vreinterpretq_u32_f32(x), signMask);
        uint32x4_t magBits = vandq_u32(vreinterpretq_u32_f32(mag), vmvnq_u32(signMask));
        return vreinterpretq_f32_u32(vorrq_u32(magBits, signBits));
    }

    [[nodiscard]] inline float saturateScalar(float x) const noexcept {
        float absX = std::abs(x);
        if (absX <= threshLinear_) return x;
        if (absX >= ceilingLinear_) return std::copysign(ceilingLinear_, x);
        float t = (absX - threshLinear_) / (ceilingLinear_ - threshLinear_);
        float smoothT = t * t * (3.0f - 2.0f * t);
        float satMag = threshLinear_ + smoothT * (ceilingLinear_ - threshLinear_);
        return std::copysign(satMag, x);
    }

    void updateKnee() noexcept {
        float kneeFactor = std::pow(10.0f, -kneeDb_ / 20.0f);
        threshLinear_ = ceilingLinear_ * kneeFactor;
        kneeWidth_ = ceilingLinear_ - threshLinear_;
        invKnee_ = 1.0f / kneeWidth_;

        vCeiling_   = vdupq_n_f32(ceilingLinear_);
        vThresh_    = vdupq_n_f32(threshLinear_);
        vKneeWidth_ = vdupq_n_f32(kneeWidth_);
        vInvKnee_   = vdupq_n_f32(invKnee_);
    }

    float sampleRate_{48000.0f};
    float ceilingLinear_{0.988553f};
    float kneeDb_{3.0f};
    float threshLinear_{0.70f};
    float kneeWidth_{0.288553f};
    float invKnee_{3.465568f};
    bool enabled_{true};

    float32x4_t vCeiling_{}, vThresh_{}, vKneeWidth_{}, vInvKnee_{};

    BlockHalfbandNEON stage1UpL_, stage1UpR_;
    BlockHalfbandNEON stage2UpL_, stage2UpR_;
    BlockHalfbandNEON stage2DownL_, stage2DownR_;
    BlockHalfbandNEON stage1DownL_, stage1DownR_;
};
#else
using TruePeakClipperNEON = TruePeakClipperScalar;
#endif

// ============================================================================
// LAUFZEIT-DISPATCHER: 4x TRUE-PEAK CLIPPER
// ============================================================================

class TruePeakClipperAuto {
public:
    explicit TruePeakClipperAuto(CpuSimdPath path = CpuDetector::detectBestPath()) noexcept
        : path_(path) {}

    explicit TruePeakClipperAuto(const CpuCapabilities& caps) noexcept
        : TruePeakClipperAuto(CpuDetector::selectPath(caps)) {}

    void setPath(CpuSimdPath path) noexcept {
        path_ = path;
    }

    [[nodiscard]] CpuSimdPath getPath() const noexcept {
        return path_;
    }

    [[nodiscard]] std::string_view getPathName() const noexcept {
        return getCpuSimdPathName(path_);
    }

    void applyCapabilities(const CpuCapabilities& caps) noexcept {
        setPath(CpuDetector::selectPath(caps));
    }

    void prepare(float sampleRate) noexcept {
        sampleRate_ = sampleRate;
        scalar_.prepare(sampleRate);
        avx2_.prepare(sampleRate);
        neon_.prepare(sampleRate);
    }

    void setCeilingDb(float ceilingDb) noexcept {
        scalar_.setCeilingDb(ceilingDb);
        avx2_.setCeilingDb(ceilingDb);
        neon_.setCeilingDb(ceilingDb);
    }

    void setKneeDb(float kneeDb) noexcept {
        scalar_.setKneeDb(kneeDb);
        avx2_.setKneeDb(kneeDb);
        neon_.setKneeDb(kneeDb);
    }

    void setEnabled(bool enable) noexcept {
        scalar_.setEnabled(enable);
        avx2_.setEnabled(enable);
        neon_.setEnabled(enable);
    }

    void processStereo(float inL, float inR, float& outL, float& outR) noexcept {
        switch (path_) {
            case CpuSimdPath::AVX2:   avx2_.processStereo(inL, inR, outL, outR); break;
            case CpuSimdPath::NEON:   neon_.processStereo(inL, inR, outL, outR); break;
            case CpuSimdPath::Scalar:
            default:                  scalar_.processStereo(inL, inR, outL, outR); break;
        }
    }

    void processBlock(const float* inL, const float* inR, float* outL, float* outR, size_t numFrames) noexcept {
        switch (path_) {
            case CpuSimdPath::AVX2:   avx2_.processBlock(inL, inR, outL, outR, numFrames); break;
            case CpuSimdPath::NEON:   neon_.processBlock(inL, inR, outL, outR, numFrames); break;
            case CpuSimdPath::Scalar:
            default:                  scalar_.processBlock(inL, inR, outL, outR, numFrames); break;
        }
    }

    TruePeakClipperScalar& scalarClipper() noexcept { return scalar_; }
    TruePeakClipperAVX2&   avx2Clipper()   noexcept { return avx2_; }
    TruePeakClipperNEON&   neonClipper()   noexcept { return neon_; }

private:
    CpuSimdPath path_{CpuSimdPath::Scalar};
    float sampleRate_{48000.0f};
    alignas(32) TruePeakClipperScalar scalar_;
    alignas(32) TruePeakClipperAVX2   avx2_;
    alignas(32) TruePeakClipperNEON   neon_;
};

// ============================================================================
// 5. MASTER ENGINES FÜR DIE DREI PFADE
// ============================================================================

template <typename TKernel, typename TClipper>
class HQRNReverbEngineGeneric {
public:
    void prepare(float sampleRate) noexcept {
        sampleRate_ = sampleRate;
        kernel_.prepare(sampleRate);
        clipper_.prepare(sampleRate);
    }

    void reset() noexcept {
        kernel_.reset();
    }

    void setBaseFrequency(float fBaseHz) noexcept { kernel_.setBaseFrequency(fBaseHz); }
    void setReverbTime(float t60Sec) noexcept { kernel_.setReverbTime(t60Sec); }
    void setSourcePosition(float x, float y) noexcept { kernel_.setSourcePosition({x, y}); }
    void setListenerPosition(float x, float y, float headingRad = 0.0f) noexcept {
        kernel_.setListenerPosition({x, y}, headingRad);
    }

    void setMix(float wetRatio) noexcept { wetRatio_ = std::clamp(wetRatio, 0.0f, 1.0f); }
    void setOutputGainDb(float gainDb) noexcept { outputGain_ = std::pow(10.0f, gainDb / 20.0f); }

    void setClipperParams(float ceilingDb, float kneeDb = 3.0f, bool enable = true) noexcept {
        clipper_.setCeilingDb(ceilingDb);
        clipper_.setKneeDb(kneeDb);
        clipper_.setEnabled(enable);
    }

    void process(std::span<const float> inL, std::span<const float> inR,
                 std::span<float> outL, std::span<float> outR) noexcept {
        size_t n = std::min({inL.size(), inR.size(), outL.size(), outR.size()});

        constexpr size_t CHUNK_SIZE = 512;
        alignas(32) float mixedL[CHUNK_SIZE];
        alignas(32) float mixedR[CHUNK_SIZE];

        for (size_t offset = 0; offset < n; offset += CHUNK_SIZE) {
            size_t chunkSize = std::min(CHUNK_SIZE, n - offset);
            for (size_t i = 0; i < chunkSize; ++i) {
                float dryL = inL[offset + i];
                float dryR = inR[offset + i];

                float wetL = 0.0f;
                float wetR = 0.0f;
                kernel_.processSample(dryL, dryR, wetL, wetR);

                mixedL[i] = (dryL * (1.0f - wetRatio_) + wetL * wetRatio_) * outputGain_;
                mixedR[i] = (dryR * (1.0f - wetRatio_) + wetR * wetRatio_) * outputGain_;
            }

            clipper_.processBlock(mixedL, mixedR, outL.data() + offset, outR.data() + offset, chunkSize);
        }
    }

    TKernel& kernel() noexcept { return kernel_; }
    TClipper& clipper() noexcept { return clipper_; }

private:
    float sampleRate_{48000.0f};
    float wetRatio_{0.35f};
    float outputGain_{1.0f};

    TKernel kernel_;
    TClipper clipper_;
};

using HQRNReverbEngineScalar = HQRNReverbEngineGeneric<HQRNKernelScalar, TruePeakClipperScalar>;
using HQRNReverbEngineAVX2   = HQRNReverbEngineGeneric<HQRNKernelAVX2,   TruePeakClipperAVX2>;
using HQRNReverbEngineNEON   = HQRNReverbEngineGeneric<HQRNKernelNEON,   TruePeakClipperNEON>;

// ============================================================================
// LAUFZEIT-DISPATCHER: GESAMT-REVERB-ENGINE
// ============================================================================

class HQRNReverbEngineAuto {
public:
    explicit HQRNReverbEngineAuto(CpuSimdPath path = CpuDetector::detectBestPath()) noexcept
        : path_(path) {}

    explicit HQRNReverbEngineAuto(const CpuCapabilities& caps) noexcept
        : HQRNReverbEngineAuto(CpuDetector::selectPath(caps)) {}

    void prepare(float sampleRate) noexcept {
        sampleRate_ = sampleRate;
        scalarEngine_.prepare(sampleRate);
        avx2Engine_.prepare(sampleRate);
        neonEngine_.prepare(sampleRate);
    }

    void reset() noexcept {
        scalarEngine_.reset();
        avx2Engine_.reset();
        neonEngine_.reset();
    }

    void setSimdPath(CpuSimdPath path) noexcept {
        path_ = path;
    }

    [[nodiscard]] CpuSimdPath getSimdPath() const noexcept {
        return path_;
    }

    [[nodiscard]] std::string_view getSimdPathName() const noexcept {
        return getCpuSimdPathName(path_);
    }

    void applyCapabilities(const CpuCapabilities& caps) noexcept {
        setSimdPath(CpuDetector::selectPath(caps));
    }

    void setBaseFrequency(float fBaseHz) noexcept {
        scalarEngine_.setBaseFrequency(fBaseHz);
        avx2Engine_.setBaseFrequency(fBaseHz);
        neonEngine_.setBaseFrequency(fBaseHz);
    }

    void setReverbTime(float t60Sec) noexcept {
        scalarEngine_.setReverbTime(t60Sec);
        avx2Engine_.setReverbTime(t60Sec);
        neonEngine_.setReverbTime(t60Sec);
    }

    void setSourcePosition(float x, float y) noexcept {
        scalarEngine_.setSourcePosition(x, y);
        avx2Engine_.setSourcePosition(x, y);
        neonEngine_.setSourcePosition(x, y);
    }

    void setListenerPosition(float x, float y, float headingRad = 0.0f) noexcept {
        scalarEngine_.setListenerPosition(x, y, headingRad);
        avx2Engine_.setListenerPosition(x, y, headingRad);
        neonEngine_.setListenerPosition(x, y, headingRad);
    }

    void setMix(float wetRatio) noexcept {
        scalarEngine_.setMix(wetRatio);
        avx2Engine_.setMix(wetRatio);
        neonEngine_.setMix(wetRatio);
    }

    void setOutputGainDb(float gainDb) noexcept {
        scalarEngine_.setOutputGainDb(gainDb);
        avx2Engine_.setOutputGainDb(gainDb);
        neonEngine_.setOutputGainDb(gainDb);
    }

    void setClipperParams(float ceilingDb, float kneeDb = 3.0f, bool enable = true) noexcept {
        scalarEngine_.setClipperParams(ceilingDb, kneeDb, enable);
        avx2Engine_.setClipperParams(ceilingDb, kneeDb, enable);
        neonEngine_.setClipperParams(ceilingDb, kneeDb, enable);
    }

    void process(std::span<const float> inL, std::span<const float> inR,
                 std::span<float> outL, std::span<float> outR) noexcept {
        switch (path_) {
            case CpuSimdPath::AVX2:
                avx2Engine_.process(inL, inR, outL, outR);
                break;
            case CpuSimdPath::NEON:
                neonEngine_.process(inL, inR, outL, outR);
                break;
            case CpuSimdPath::Scalar:
            default:
                scalarEngine_.process(inL, inR, outL, outR);
                break;
        }
    }

    HQRNReverbEngineScalar& scalarEngine() noexcept { return scalarEngine_; }
    HQRNReverbEngineAVX2&   avx2Engine()   noexcept { return avx2Engine_; }
    HQRNReverbEngineNEON&   neonEngine()   noexcept { return neonEngine_; }

private:
    CpuSimdPath path_{CpuSimdPath::Scalar};
    float sampleRate_{48000.0f};

    alignas(32) HQRNReverbEngineScalar scalarEngine_;
    alignas(32) HQRNReverbEngineAVX2   avx2Engine_;
    alignas(32) HQRNReverbEngineNEON   neonEngine_;
};

} // namespace dsp::hqrn
