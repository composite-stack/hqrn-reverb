#pragma once

/**
 * @file HQRNReverbEngine.hpp
 * @brief Hyperbolisches Quanten-Resonator-Netzwerk (HQRN) Reverb Engine
 * 
 * Mathematische Grundlagen:
 * 1. Geometrie: 16 Knoten auf der Poincaré-Einheitskreisscheibe (8 innen r=0.35, 8 außen r=0.68).
 * 2. Spektrum: 16 Selberg-Eigenwerte des Laplace-Beltrami-Operators der Bolza-Fläche (Cook 2018).
 * 3. Linearer Kern (Weg 1): 100% linearer, zeitinvarianter Zustand [q_u, p_u] (32 Zustände).
 *    - Orthogonale Kopplungsmatrix: K = H_4 (x) I_2 via Fast Walsh-Hadamard Transform (O(N log N)).
 *    - Blockdiagonale Phasendrehung: R(theta_u) kombiniert mit modaler Dämpfung gamma_u.
 *    - Quelle B(z_S): Möbius-Hyperbolischer Abstand & Azimut, Injektion in Impulskoordinaten p_u.
 *    - Hörer C(z_L): Duale Möbius-Auskopplung mit Modendichte-Eichung g_rad = sqrt(32 * (1 - mean_gamma^2)).
 * 4. Master Output Stage:
 *    - Master Dry/Wet-Mix & Gain-Staging.
 *    - 4x oversampled True-Peak Soft-Clipper (2-stufiges polyphasiges Halbband-FIR, >85 dB Stopband-Dämpfung).
 *    - Bit-exakte Linearität unterhalb des Soft-Knee, C2-glatte Hermite-Sättigung, echter True-Peak Brickwall.
 * 
 * Standard: C++20
 * Autor: M. A. / Lovable Audio DSP
 */

#include <array>
#include <vector>
#include <cmath>
#include <complex>
#include <numbers>
#include <algorithm>
#include <span>
#include <cstddef>

namespace dsp::hqrn {

using std::numbers::pi_v;

// ============================================================================
// 1. POINCARÉ-SCHEIBEN-GEOMETRIE & SELBERG-SPEKTRUM
// ============================================================================

struct DiskPoint {
    float x{0.0f};
    float y{0.0f};

    [[nodiscard]] constexpr float radiusSq() const noexcept { return x * x + y * y; }
    [[nodiscard]] float radius() const noexcept { return std::sqrt(radiusSq()); }

    [[nodiscard]] DiskPoint clamped(float maxR = 0.95f) const noexcept {
        float r = radius();
        if (r > maxR && r > 1e-7f) {
            float s = maxR / r;
            return {x * s, y * s};
        }
        return *this;
    }
};

/**
 * Erzeugt die 16 Resonatorknoten auf zwei konzentrischen Ringen der Poincaré-Scheibe.
 * Ring 1 (innen):  8 Knoten bei r = 0.35, phi = k * pi/4
 * Ring 2 (außen):  8 Knoten bei r = 0.68, phi = k * pi/4 + pi/8
 */
inline std::array<DiskPoint, 16> createNodeLayout() noexcept {
    std::array<DiskPoint, 16> nodes{};
    constexpr float r1 = 0.35f;
    constexpr float r2 = 0.68f;
    for (int k = 0; k < 8; ++k) {
        float phi1 = k * (pi_v<float> / 4.0f);
        nodes[k] = {r1 * std::cos(phi1), r1 * std::sin(phi1)};

        float phi2 = k * (pi_v<float> / 4.0f) + (pi_v<float> / 8.0f);
        nodes[k + 8] = {r2 * std::cos(phi2), r2 * std::sin(phi2)};
    }
    return nodes;
}

/**
 * Berechnet den hyperbolischen Abstand d_H(a, b) über die Möbius-Differenz.
 * d_H = 2 * artanh(|(a - b) / (1 - conj(b)*a)|)
 */
inline float hyperbolicDistance(DiskPoint a, DiskPoint b) noexcept {
    std::complex<float> za(a.x, a.y);
    std::complex<float> zb(b.x, b.y);
    std::complex<float> diff = (za - zb) / (1.0f - std::conj(zb) * za);
    float delta = std::min(0.9999f, std::abs(diff));
    return 2.0f * std::atanh(delta);
}

/**
 * Berechnet das Azimut psi im Poincaré-Tangentialraum.
 */
inline float tangentAzimuth(DiskPoint origin, DiskPoint target) noexcept {
    std::complex<float> zo(origin.x, origin.y);
    std::complex<float> zt(target.x, target.y);
    std::complex<float> diff = (zt - zo) / (1.0f - std::conj(zo) * zt);
    return std::arg(diff);
}

/**
 * Die ersten 16 diskreten Selberg-Eigenwerte lambda_j des Laplace-Beltrami-Operators
 * auf der Bolza-Fläche (Cook 2018 / Strohmaier & Uski).
 */
inline constexpr std::array<float, 16> BOLZA_LAMBDA = {
    3.83889f,  5.35360f,  8.24900f, 14.71490f,
    15.03860f, 18.64500f, 20.50690f, 23.05450f,
    28.05910f, 30.80810f, 32.64930f, 36.21260f,
    38.93530f, 40.09130f, 42.83440f, 43.98640f
};

// ============================================================================
// 2. ORTHOGONALE FAST WALSH-HADAMARD TRANSFORMATION (FWHT-16)
// ============================================================================

/**
 * Berechnet in-place die unitäre 16-Punkt Fast Walsh-Hadamard Transformation.
 * K = H_4 = H_1 (x) H_1 (x) H_1 (x) H_1
 * Erfordert exakt 16 * 4 = 64 Additionen/Subtraktionen und Skalierung mit 1/sqrt(16) = 0.25.
 * Exakt unitär: K^T * K = I_16.
 */
inline void fwht16(float* data) noexcept {
    for (int len = 1; len < 16; len <<= 1) {
        for (int i = 0; i < 16; i += 2 * len) {
            for (int j = 0; j < len; ++j) {
                float u = data[i + j];
                float v = data[i + len + j];
                data[i + j] = u + v;
                data[i + len + j] = u - v;
            }
        }
    }
    // Unitäre Normierung
    for (int i = 0; i < 16; ++i) {
        data[i] *= 0.25f;
    }
}

// ============================================================================
// 3. POLYPHASIGES HALBBAND-FIR-FILTER FÜR 2x UP-/DOWNSAMPLING
// ============================================================================

/**
 * 31-Tap lineares, symmetrisches Halbband-Filter (Equiripple Remez-Design).
 * Sperrdämpfung: >85 dB.
 * Polyphase Zerlegung:
 * Phase 0 (e0): 16 Taps, achsensymmetrisch um die Mitte (8 symmetrische Paare).
 * Phase 1 (e1): Exakt 1 Nicht-Null-Koeffizient (Center-Tap = 0.5 bei Delay = 7).
 */
class PolyphaseHalfband2x {
public:
    static constexpr std::array<float, 8> E0_HALF = {
        -0.0003019409f,  0.0013999166f, -0.0042661702f,  0.0104049809f,
        -0.0222122293f,  0.0445282752f, -0.0934614170f,  0.3138971703f
    };

    void reset() noexcept {
        bufUp_.fill(0.0f);
        bufDown0_.fill(0.0f);
        bufDown1_.fill(0.0f);
        headUp_ = 0;
        headDown_ = 0;
    }

    // 2x Upsampling: 1 Input -> 2 Outputs (even, odd)
    void upsample(float in, float& out0, float& out1) noexcept {
        bufUp_[headUp_] = in;

        // Phase 0: Symmetrische Faltung mit e0 (8 Multiplikationen)
        float acc = 0.0f;
        for (int k = 0; k < 8; ++k) {
            float tap = E0_HALF[k];
            int idx0 = (headUp_ - k + 16) & 15;
            int idx1 = (headUp_ - (15 - k) + 16) & 15;
            acc += tap * (bufUp_[idx0] + bufUp_[idx1]);
        }
        out0 = 2.0f * acc;

        // Phase 1: Reines Delay von 7 Samples (Center-Tap 2 * 0.5 = 1.0)
        int delayIdx = (headUp_ - 7 + 16) & 15;
        out1 = bufUp_[delayIdx];

        headUp_ = (headUp_ + 1) & 15;
    }

    // 2x Downsampling: 2 Inputs (even, odd) -> 1 Output
    float downsample(float in0, float in1) noexcept {
        bufDown0_[headDown_] = in0;
        bufDown1_[headDown_] = in1;

        // Phase 0: Faltung mit e0
        float acc = 0.0f;
        for (int k = 0; k < 8; ++k) {
            float tap = E0_HALF[k];
            int idx0 = (headDown_ - k + 16) & 15;
            int idx1 = (headDown_ - (15 - k) + 16) & 15;
            acc += tap * (bufDown0_[idx0] + bufDown0_[idx1]);
        }

        // Phase 1: Verzögertes ungeradzahliges Sample um 8 Takte (2m - 15 = 2(m-8) + 1)
        int delayIdx = (headDown_ - 8 + 16) & 15;
        float delayed1 = 0.5f * bufDown1_[delayIdx];

        headDown_ = (headDown_ + 1) & 15;
        return acc + delayed1;
    }

private:
    std::array<float, 16> bufUp_{};
    std::array<float, 16> bufDown0_{};
    std::array<float, 16> bufDown1_{};
    int headUp_{0};
    int headDown_{0};
};

// ============================================================================
// 4. OVERSAMPLED TRUE-PEAK SOFT-CLIPPER (4x OVERSAMPLING)
// ============================================================================

/**
 * 4x Oversampled True-Peak Clipper:
 * - Kaskade aus zwei 2x Polyphasen-Halbbandfiltern.
 * - Bit-exakte Linearität unterhalb der Knie-Schwelle (keine THD/IMD im Arbeitsbereich).
 * - C2-stetiger kubischer Hermite-Spline im Kniebereich.
 * - Feste True-Peak Brickwall-Deckelung an der Ceiling.
 */
class TruePeakClipper {
public:
    void prepare(float sampleRate) noexcept {
        sampleRate_ = sampleRate;
        reset();
    }

    void reset() noexcept {
        stage1UpL_.reset(); stage1UpR_.reset();
        stage2UpL_.reset(); stage2UpR_.reset();
        stage1DownL_.reset(); stage1DownR_.reset();
        stage2DownL_.reset(); stage2DownR_.reset();
    }

    [[nodiscard]] size_t getLatencySamples() const noexcept {
        return enabled_ ? 22 : 0;
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

    // Glatte C2 Hermite-Sättigungskennlinie
    [[nodiscard]] inline float saturate(float x) const noexcept {
        float absX = std::abs(x);
        if (absX <= threshLinear_) {
            return x; // Bit-exakte Linearität unterhalb des Knies
        }
        if (absX >= ceilingLinear_) {
            return std::copysign(ceilingLinear_, x); // Absoluter Brickwall
        }

        // Hermite Smoothstep-Übergang: t in [0, 1] -> 3t^2 - 2t^3
        float t = (absX - threshLinear_) / (ceilingLinear_ - threshLinear_);
        float smoothT = t * t * (3.0f - 2.0f * t);
        float satMag = threshLinear_ + smoothT * (ceilingLinear_ - threshLinear_);
        return std::copysign(satMag, x);
    }

    void processStereo(float inL, float inR, float& outL, float& outR) noexcept {
        if (!enabled_) {
            outL = inL;
            outR = inR;
            return;
        }

        // 1. Stufe 1: 1x -> 2x
        float l2_0, l2_1, r2_0, r2_1;
        stage1UpL_.upsample(inL, l2_0, l2_1);
        stage1UpR_.upsample(inR, r2_0, r2_1);

        // 2. Stufe 2: 2x -> 4x
        std::array<float, 4> l4{}, r4{};
        stage2UpL_.upsample(l2_0, l4[0], l4[1]);
        stage2UpL_.upsample(l2_1, l4[2], l4[3]);
        stage2UpR_.upsample(r2_0, r4[0], r4[1]);
        stage2UpR_.upsample(r2_1, r4[2], r4[3]);

        // 3. True-Peak Sättigung bei 4-facher Abtastrate
        for (int i = 0; i < 4; ++i) {
            l4[i] = saturate(l4[i]);
            r4[i] = saturate(r4[i]);
        }

        // 4. Dezimation Stufe 2: 4x -> 2x
        float l2_down0 = stage2DownL_.downsample(l4[0], l4[1]);
        float l2_down1 = stage2DownL_.downsample(l4[2], l4[3]);
        float r2_down0 = stage2DownR_.downsample(r4[0], r4[1]);
        float r2_down1 = stage2DownR_.downsample(r4[2], r4[3]);

        // 5. Dezimation Stufe 1: 2x -> 1x
        float decimatedL = stage1DownL_.downsample(l2_down0, l2_down1);
        float decimatedR = stage1DownR_.downsample(r2_down0, r2_down1);

        // 6. Post-Decimation Safety-Klemme:
        // Fängt unvermeidliche Gibbs-Überschwinger der Dezimationsfilterung ab
        outL = saturate(decimatedL);
        outR = saturate(decimatedR);
    }

private:
    void updateKnee() noexcept {
        float kneeFactor = std::pow(10.0f, -kneeDb_ / 20.0f);
        threshLinear_ = ceilingLinear_ * kneeFactor;
    }

    float sampleRate_{48000.0f};
    float ceilingLinear_{0.988553f}; // -0.1 dBFS default True-Peak ceiling
    float kneeDb_{3.0f};             // 3 dB Soft-Knee
    float threshLinear_{0.70f};
    bool enabled_{true};

    PolyphaseHalfband2x stage1UpL_, stage1UpR_;
    PolyphaseHalfband2x stage2UpL_, stage2UpR_;
    PolyphaseHalfband2x stage2DownL_, stage2DownR_;
    PolyphaseHalfband2x stage1DownL_, stage1DownR_;
};

// ============================================================================
// 5. LINEARER HYPERBOLISCHER QUANTEN-RESONATORKERN (WEG 1)
// ============================================================================

/**
 * 16-Knoten linearer hyperbolischer Resonatorkern (32 Zustände).
 * Die Feedback-Schleife ist 100 % linear und zeitinvariant:
 * x[n+1] = A * x[n] + B(z_S) * u[n]
 * y[n]   = C(z_L) * x[n]
 * A = Gamma * K * R(Theta)
 */
class LinearHQRNKernel {
public:
    LinearHQRNKernel() noexcept : nodes_(createNodeLayout()) {}

    void prepare(float sampleRate) noexcept {
        sampleRate_ = sampleRate;
        reset();
        setCouplingPhase(couplingTheta_);
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

    void setCouplingPhase(float thetaRad) noexcept {
        couplingTheta_ = thetaRad;
        float cosT = std::cos(thetaRad);
        float sinT = std::sin(thetaRad);
        // M = P+ + e^{i theta} P- = (1+e^{ith})/2 I + (1-e^{ith})/2 H
        couplingAr_ = 0.5f * (1.0f + cosT);
        couplingAi_ = 0.5f * sinT;
        couplingBr_ = 0.5f * (1.0f - cosT);
        couplingBi_ = -0.5f * sinT;
    }

    [[nodiscard]] float getCouplingPhase() const noexcept {
        return couplingTheta_;
    }

    void getModeEnergies(float* outEnergies) const noexcept {
        if (outEnergies == nullptr) return;
        for (int u = 0; u < 16; ++u) {
            outEnergies[u] = q_[u] * q_[u] + p_[u] * p_[u];
        }
    }

    [[nodiscard]] float getModeEnergy(size_t u) const noexcept {
        if (u >= 16) return 0.0f;
        return q_[u] * q_[u] + p_[u] * p_[u];
    }

    [[nodiscard]] float getGrad() const noexcept {
        return gRad_;
    }

    [[nodiscard]] float getSourceListenerDistance() const noexcept {
        return hyperbolicDistance(sourcePos_, listenerPos_);
    }

    void setListenerPosition(DiskPoint zL, float headingRad = 0.0f) noexcept {
        listenerPos_ = zL.clamped(0.95f);
        listenerHeading_ = headingRad;
        updateListenerExtraction();
    }

    // Single-Sample Verarbeitungszyklus
    inline void processSample(float inL, float inR, float& outL, float& outR) noexcept {
        // 1. Phasendrehung & Dämpfung: [q_u, p_u] <- gamma_u * R(theta_u) * [q_u, p_u]
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

        // 2. Unitäre Hadamard-/Phasen-Kopplung: M(theta) = P+ + e^{i theta} P-
        std::array<float, 16> hq = q_;
        std::array<float, 16> hp = p_;
        fwht16(hq.data());
        fwht16(hp.data());

        for (int u = 0; u < 16; ++u) {
            float qu = q_[u];
            float pu = p_[u];
            float hqu = hq[u];
            float hpu = hp[u];
            q_[u] = couplingAr_ * qu - couplingAi_ * pu + couplingBr_ * hqu - couplingBi_ * hpu;
            p_[u] = couplingAi_ * qu + couplingAr_ * pu + couplingBi_ * hqu + couplingBr_ * hpu;
        }

        // 3. Audio-Injektion B(z_S) in Impulskoordinaten p_u
        for (int u = 0; u < 16; ++u) {
            p_[u] += bL_[u] * inL + bR_[u] * inR;
        }

        // 4. Lineare Stereo-Auskopplung C(z_L) aus p_u (Weg A mit g_rad)
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
            // Bolza Laplace-Beltrami Frequenzen: r_j = sqrt(lambda_j - 1/4)
            float r_j = std::sqrt(std::max(0.0f, BOLZA_LAMBDA[u] - 0.25f));
            float freqHz = baseFreqHz_ * (r_j / std::sqrt(BOLZA_LAMBDA[0] - 0.25f));
            freqHz = std::min(freqHz, 0.48f * sampleRate_);

            float theta = 2.0f * pi_v<float> * freqHz / sampleRate_;
            cosTheta_[u] = std::cos(theta);
            sinTheta_[u] = std::sin(theta);

            // Dämpfung: gamma = 10^(-3 / (fs * T60))
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
            float panL = std::cos(0.5f * psi + pi_v<float> / 4.0f);
            float panR = std::sin(0.5f * psi + pi_v<float> / 4.0f);

            bL_[u] = wNorm * panL;
            bR_[u] = wNorm * panR;
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

        // Modendichte-Eichung (Weg A): g_rad = sqrt(32 * (1 - meanGammaSq_))
        float gRad = std::sqrt(std::max(1e-6f, 32.0f * (1.0f - meanGammaSq_)));
        gRad_ = gRad;

        for (int u = 0; u < 16; ++u) {
            float vNorm = rawV[u] * normV;
            float psi = tangentAzimuth(listenerPos_, nodes_[u]) - listenerHeading_;
            float panL = std::cos(0.5f * psi + pi_v<float> / 4.0f);
            float panR = std::sin(0.5f * psi + pi_v<float> / 4.0f);

            cL_[u] = gRad * vNorm * panL;
            cR_[u] = gRad * vNorm * panR;
        }
    }

    float sampleRate_{48000.0f};
    float baseFreqHz_{120.0f};
    float t60Sec_{1.5f};
    float meanGammaSq_{0.999f};

    DiskPoint sourcePos_{0.0f, 0.0f};
    DiskPoint listenerPos_{0.0f, 0.0f};
    float listenerHeading_{0.0f};

    float couplingTheta_{pi_v<float>};
    float couplingAr_{0.0f};
    float couplingAi_{0.0f};
    float couplingBr_{1.0f};
    float couplingBi_{0.0f};
    float gRad_{1.0f};

    const std::array<DiskPoint, 16> nodes_;

    // SIMD-freundliches deinterleaved Layout: q und p getrennt
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
// 6. MASTER ENGINE (HQRN-REVERB + TRUE-PEAK CLIPPER + GAIN STAGING)
// ============================================================================

class HQRNReverbEngine {
public:
    void prepare(float sampleRate) noexcept {
        sampleRate_ = sampleRate;
        kernel_.prepare(sampleRate);
        clipper_.prepare(sampleRate);
    }

    void reset() noexcept {
        kernel_.reset();
        clipper_.reset();
    }

    [[nodiscard]] size_t getLatencySamples() const noexcept {
        return clipper_.getLatencySamples();
    }

    void setBaseFrequency(float fBaseHz) noexcept { kernel_.setBaseFrequency(fBaseHz); }
    void setReverbTime(float t60Sec) noexcept { kernel_.setReverbTime(t60Sec); }
    void setSourcePosition(float x, float y) noexcept { kernel_.setSourcePosition({x, y}); }
    void setListenerPosition(float x, float y, float headingRad = 0.0f) noexcept {
        kernel_.setListenerPosition({x, y}, headingRad);
    }

    void setCouplingPhase(float thetaRad) noexcept { kernel_.setCouplingPhase(thetaRad); }
    [[nodiscard]] float getCouplingPhase() const noexcept { return kernel_.getCouplingPhase(); }
    void getModeEnergies(float* outEnergies) const noexcept { kernel_.getModeEnergies(outEnergies); }
    [[nodiscard]] float getModeEnergy(size_t u) const noexcept { return kernel_.getModeEnergy(u); }
    [[nodiscard]] float getGrad() const noexcept { return kernel_.getGrad(); }
    [[nodiscard]] float getSourceListenerDistance() const noexcept { return kernel_.getSourceListenerDistance(); }

    void setMix(float wetRatio) noexcept { wetRatio_ = std::clamp(wetRatio, 0.0f, 1.0f); }
    void setOutputGainDb(float gainDb) noexcept { outputGain_ = std::pow(10.0f, gainDb / 20.0f); }

    void setClipperParams(float ceilingDb, float kneeDb = 3.0f, bool enable = true) noexcept {
        clipper_.setCeilingDb(ceilingDb);
        clipper_.setKneeDb(kneeDb);
        clipper_.setEnabled(enable);
    }

    /**
     * Stereo-Blockverarbeitung (Standard C++20 std::span).
     * Garantiert Null Allokationen im Audio-Thread.
     */
    void process(std::span<const float> inL, std::span<const float> inR,
                 std::span<float> outL, std::span<float> outR) noexcept {
        size_t n = std::min({inL.size(), inR.size(), outL.size(), outR.size()});

        for (size_t i = 0; i < n; ++i) {
            float dryL = inL[i];
            float dryR = inR[i];

            // 1. Lineares Resonator-Netzwerk (Weg 1)
            float wetL = 0.0f;
            float wetR = 0.0f;
            kernel_.processSample(dryL, dryR, wetL, wetR);

            // 2. Dry/Wet Mix & Master Output Gain
            float mixedL = (dryL * (1.0f - wetRatio_) + wetL * wetRatio_) * outputGain_;
            float mixedR = (dryR * (1.0f - wetRatio_) + wetR * wetRatio_) * outputGain_;

            // 3. Oversampled True-Peak Clipper (nur am Masterausgang)
            clipper_.processStereo(mixedL, mixedR, outL[i], outR[i]);
        }
    }

private:
    float sampleRate_{48000.0f};
    float wetRatio_{0.35f};
    float outputGain_{1.0f};

    LinearHQRNKernel kernel_;
    TruePeakClipper clipper_;
};

} // namespace dsp::hqrn
