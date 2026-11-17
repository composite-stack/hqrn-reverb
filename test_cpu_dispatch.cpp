#include "HQRNKernels.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <new>
#include <span>
#include <string>
#include <vector>

namespace {

// ============================================================================
// HEAP ALLOCATION MONITOR (Echtzeit-Audiothread Verifikation)
// ============================================================================
std::atomic<std::size_t> gAllocations{0};
std::atomic<bool> gTrackAllocations{false};

int gTestPassCount = 0;
int gTestFailCount = 0;

void reportTest(const std::string& name, bool passed, const std::string& details = "") {
    if (passed) {
        std::cout << "  [PASS] " << name << (details.empty() ? "" : " (" + details + ")") << "\n";
        ++gTestPassCount;
    } else {
        std::cout << "  [FAIL] " << name << (details.empty() ? "" : " -> " + details) << "\n";
        ++gTestFailCount;
    }
}

} // namespace

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void* operator new(std::size_t size) {
    if (gTrackAllocations.load(std::memory_order_relaxed))
        gAllocations.fetch_add(1, std::memory_order_relaxed);
    if (void* ptr = std::malloc(size)) return ptr;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    if (gTrackAllocations.load(std::memory_order_relaxed))
        gAllocations.fetch_add(1, std::memory_order_relaxed);
    if (void* ptr = std::malloc(size)) return ptr;
    throw std::bad_alloc();
}
void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { std::free(ptr); }
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

// ============================================================================
// 1. TESTSUITE: CPU-CAPABILITY AUSWAHL & FALLBACK-MATRIX
// ============================================================================
void testCpuDetectorSelectionMatrix() {
    using namespace dsp::hqrn;
    std::cout << "\n[TEST 1] CPU-DETECTOR CAPABILITY-MATRIX & FALLBACK-PRIORITAET:\n";

    // Fall 1: Vollwertiges AVX2 + FMA
    {
        CpuCapabilities caps{ .hasAvx2 = true, .hasFma = true, .hasNeon = false };
        CpuSimdPath selected = CpuDetector::selectPath(caps);
        reportTest("AVX2 + FMA verfuegbar", selected == CpuSimdPath::AVX2,
                   "Pfad: " + std::string(getCpuSimdPathName(selected)));
    }

    // Fall 2: AVX2 gesetzt, aber FMA fehlt (Partieller Hardware-Defekt / Virtualisierung)
    {
        CpuCapabilities caps{ .hasAvx2 = true, .hasFma = false, .hasNeon = false };
        CpuSimdPath selected = CpuDetector::selectPath(caps);
        reportTest("AVX2 ohne FMA -> Fallback auf Skalar", selected == CpuSimdPath::Scalar,
                   "Schutz vor FMA-SIGILL: " + std::string(getCpuSimdPathName(selected)));
    }

    // Fall 3: FMA gesetzt, aber AVX2 fehlt
    {
        CpuCapabilities caps{ .hasAvx2 = false, .hasFma = true, .hasNeon = false };
        CpuSimdPath selected = CpuDetector::selectPath(caps);
        reportTest("FMA ohne AVX2 -> Fallback auf Skalar", selected == CpuSimdPath::Scalar,
                   "Pfad: " + std::string(getCpuSimdPathName(selected)));
    }

    // Fall 4: Kein AVX2, aber ARM NEON vorhanden
    {
        CpuCapabilities caps{ .hasAvx2 = false, .hasFma = false, .hasNeon = true };
        CpuSimdPath selected = CpuDetector::selectPath(caps);
        reportTest("ARM NEON verfuegbar", selected == CpuSimdPath::NEON,
                   "Pfad: " + std::string(getCpuSimdPathName(selected)));
    }

    // Fall 5: Weder AVX2 noch NEON vorhanden (Legacy x86/ARMv6/v7)
    {
        CpuCapabilities caps{ .hasAvx2 = false, .hasFma = false, .hasNeon = false };
        CpuSimdPath selected = CpuDetector::selectPath(caps);
        reportTest("Keine SIMD-Features -> Fallback auf Skalar", selected == CpuSimdPath::Scalar,
                   "Pfad: " + std::string(getCpuSimdPathName(selected)));
    }

    // Fall 6: Hardware-Erkennung auf aktueller Maschine liefert konsistente Werte
    {
        const auto hostCaps = CpuDetector::detectCapabilities();
        const auto hostPath = CpuDetector::detectBestPath();
        const bool consistent = (hostPath == CpuDetector::selectPath(hostCaps));
        reportTest("Host-Hardware-Erkennung konsistent", consistent,
                   "Host-Pfad: " + std::string(getCpuSimdPathName(hostPath)));
    }
}

// ============================================================================
// 2. TESTSUITE: FIR-HALBBANDFILTER AUSWAHL & FALLBACK (BlockHalfbandAuto)
// ============================================================================
void testBlockHalfbandSelectionAndFallback() {
    using namespace dsp::hqrn;
    std::cout << "\n[TEST 2] FIR-HALBBANDFILTER AUSWAHL & FALLBACK (BlockHalfbandAuto):\n";

    constexpr size_t N = 256;
    std::vector<float> input(N);
    for (size_t i = 0; i < N; ++i) {
        input[i] = std::sin(static_cast<float>(i) * 0.08f) + 0.5f * std::cos(static_cast<float>(i) * 0.17f);
    }

    // Referenzberechnung mit reinem skalarem Filter
    BlockHalfbandScalar refScalar;
    std::vector<float> refUp(2 * N, 0.0f);
    std::vector<float> refDown(N, 0.0f);
    refScalar.upsampleBlock(input.data(), refUp.data(), N);
    refScalar.downsampleBlock(refUp.data(), refDown.data(), N);

    // Test a: Auto-Filter mit AVX2
    {
        BlockHalfbandAuto filter(CpuSimdPath::AVX2);
        std::vector<float> up(2 * N), down(N);
        filter.upsampleBlock(input.data(), up.data(), N);
        filter.downsampleBlock(up.data(), down.data(), N);

        float maxDiffUp = 0.0f, maxDiffDown = 0.0f;
        for (size_t i = 0; i < 2 * N; ++i) maxDiffUp = std::max(maxDiffUp, std::abs(refUp[i] - up[i]));
        for (size_t i = 0; i < N; ++i) maxDiffDown = std::max(maxDiffDown, std::abs(refDown[i] - down[i]));

        reportTest("FIR AVX2 numerische Uebereinstimmung mit Skalar", (maxDiffUp < 1e-6f && maxDiffDown < 1e-6f),
                   "Max Fehler Up: " + std::to_string(maxDiffUp) + ", Down: " + std::to_string(maxDiffDown));
    }

    // Test b: Auto-Filter mit NEON
    {
        BlockHalfbandAuto filter(CpuSimdPath::NEON);
        std::vector<float> up(2 * N), down(N);
        filter.upsampleBlock(input.data(), up.data(), N);
        filter.downsampleBlock(up.data(), down.data(), N);

        float maxDiffUp = 0.0f, maxDiffDown = 0.0f;
        for (size_t i = 0; i < 2 * N; ++i) maxDiffUp = std::max(maxDiffUp, std::abs(refUp[i] - up[i]));
        for (size_t i = 0; i < N; ++i) maxDiffDown = std::max(maxDiffDown, std::abs(refDown[i] - down[i]));

        reportTest("FIR NEON numerische Uebereinstimmung mit Skalar", (maxDiffUp < 1e-6f && maxDiffDown < 1e-6f),
                   "Max Fehler Up: " + std::to_string(maxDiffUp) + ", Down: " + std::to_string(maxDiffDown));
    }

    // Test c: Auto-Filter Fallback bei deaktiviertem AVX2
    {
        CpuCapabilities noAvx{ .hasAvx2 = false, .hasFma = false, .hasNeon = false };
        BlockHalfbandAuto filter(noAvx);
        reportTest("FIR Fallback initialisiert Skalarpfad", filter.getPath() == CpuSimdPath::Scalar,
                   "Aktiver Pfad: " + std::string(filter.getPathName()));

        std::vector<float> up(2 * N), down(N);
        filter.upsampleBlock(input.data(), up.data(), N);
        filter.downsampleBlock(up.data(), down.data(), N);

        float maxDiff = 0.0f;
        for (size_t i = 0; i < 2 * N; ++i) maxDiff = std::max(maxDiff, std::abs(refUp[i] - up[i]));
        reportTest("FIR Fallback Skalar exakt bit-identisch", maxDiff == 0.0f,
                   "Max Diff: " + std::to_string(maxDiff));
    }

    // Test d: Dynamisches Umschalten zur Laufzeit waehrend Signalfluss
    {
        BlockHalfbandAuto streamFilter;
        streamFilter.setPath(CpuSimdPath::AVX2);
        std::vector<float> blockUp(2 * N);
        streamFilter.upsampleBlock(input.data(), blockUp.data(), N);

        // Umschalten auf Fallback mitten im Stream
        streamFilter.applyCapabilities(CpuCapabilities{ .hasAvx2 = false, .hasFma = false, .hasNeon = false });
        streamFilter.upsampleBlock(input.data(), blockUp.data(), N);

        bool hasNaN = false, hasInf = false;
        for (float v : blockUp) {
            if (std::isnan(v)) hasNaN = true;
            if (std::isinf(v)) hasInf = true;
        }
        reportTest("FIR dynamischer Fallback-Wechsel stoerungsfrei", (!hasNaN && !hasInf),
                   "NaN/Inf-frei, aktiver Pfad: " + std::string(streamFilter.getPathName()));
    }
}

// ============================================================================
// 3. TESTSUITE: TRUE-PEAK CLIPPER AUSWAHL & FALLBACK (TruePeakClipperAuto)
// ============================================================================
void testTruePeakClipperSelectionAndFallback() {
    using namespace dsp::hqrn;
    std::cout << "\n[TEST 3] TRUE-PEAK CLIPPER AUSWAHL & FALLBACK (TruePeakClipperAuto):\n";

    constexpr size_t blockSize = 128;
    constexpr size_t totalFrames = 2048;
    constexpr float ceilingDb = -0.1f;
    const float ceilingLinear = std::pow(10.0f, ceilingDb / 20.0f); // ~0.988553f

    // Stark uebersteuertes Eingangssignal (+6 dB Peak)
    std::vector<float> hotL(totalFrames), hotR(totalFrames);
    for (size_t i = 0; i < totalFrames; ++i) {
        hotL[i] = 2.0f * std::sin(static_cast<float>(i) * 0.05f);
        hotR[i] = 1.8f * std::cos(static_cast<float>(i) * 0.07f);
    }

    auto runClipperTest = [&](const std::string& name, CpuSimdPath path) {
        TruePeakClipperAuto clipper(path);
        clipper.prepare(48000.0f);
        clipper.setCeilingDb(ceilingDb);
        clipper.setKneeDb(3.0f);

        std::vector<float> outL(totalFrames), outR(totalFrames);
        for (size_t offset = 0; offset < totalFrames; offset += blockSize) {
            clipper.processBlock(hotL.data() + offset, hotR.data() + offset,
                                 outL.data() + offset, outR.data() + offset, blockSize);
        }

        float maxPeakL = 0.0f, maxPeakR = 0.0f;
        for (size_t i = 0; i < totalFrames; ++i) {
            maxPeakL = std::max(maxPeakL, std::abs(outL[i]));
            maxPeakR = std::max(maxPeakR, std::abs(outR[i]));
        }

        const bool limited = (maxPeakL <= ceilingLinear + 1e-4f) && (maxPeakR <= ceilingLinear + 1e-4f);
        reportTest("Clipper Peak-Limitierung [" + name + "]", limited,
                   "Max Peak: " + std::to_string(std::max(maxPeakL, maxPeakR)) + " <= " + std::to_string(ceilingLinear));
    };

    runClipperTest("AVX2 Pfad", CpuSimdPath::AVX2);
    runClipperTest("NEON Pfad", CpuSimdPath::NEON);
    runClipperTest("Skalar Pfad", CpuSimdPath::Scalar);

    // Fallback-Initialisierung via Mock-Capabilities
    {
        CpuCapabilities brokenCpu{ .hasAvx2 = true, .hasFma = false, .hasNeon = false }; // FMA fehlt
        TruePeakClipperAuto clipper(brokenCpu);
        reportTest("Clipper Mock-Fallback (FMA fehlt -> Skalar)", clipper.getPath() == CpuSimdPath::Scalar,
                   "Pfad: " + std::string(clipper.getPathName()));
    }

    // Latenz-Konsistenz über alle Pfade und Fallback
    {
        auto measureLatency = [](CpuSimdPath p) {
            TruePeakClipperAuto c(p);
            c.prepare(48000.0f);
            c.setCeilingDb(-0.1f);
            c.setKneeDb(3.0f);

            std::vector<float> inL(64, 0.0f), inR(64, 0.0f), oL(64), oR(64);
            inL[0] = inR[0] = 1.0f;
            float maxV = 0.0f;
            int peakIdx = -1;

            for (size_t blk = 0; blk < 2; ++blk) {
                if (blk > 0) { inL[0] = inR[0] = 0.0f; }
                c.processBlock(inL.data(), inR.data(), oL.data(), oR.data(), 64);
                for (size_t i = 0; i < 64; ++i) {
                    if (std::abs(oL[i]) > maxV) {
                        maxV = std::abs(oL[i]);
                        peakIdx = static_cast<int>(blk * 64 + i);
                    }
                }
            }
            return peakIdx;
        };

        const int latAvx    = measureLatency(CpuSimdPath::AVX2);
        const int latNeon   = measureLatency(CpuSimdPath::NEON);
        const int latScalar = measureLatency(CpuSimdPath::Scalar);

        const bool latencyMatch = (latAvx == 22 && latNeon == 22 && latScalar == 22);
        reportTest("Clipper Latenz-Konsistenz (alle Pfade 22 Samples)", latencyMatch,
                   "AVX: " + std::to_string(latAvx) + ", NEON: " + std::to_string(latNeon) + ", Skalar: " + std::to_string(latScalar));
    }
}

// ============================================================================
// 4. TESTSUITE: GESAMT-ENGINE AUTO-DISPATCH & ALLOKATIONSFREIHEIT
// ============================================================================
void testHQRNReverbEngineAutoAndAllocations() {
    using namespace dsp::hqrn;
    std::cout << "\n[TEST 4] GESAMT-REVERB-ENGINE AUTO-DISPATCH & ALLOKATIONSFREIHEIT:\n";

    constexpr size_t N = 512;
    std::vector<float> inL(N, 0.0f), inR(N, 0.0f), outL(N), outR(N);
    inL[0] = inR[0] = 1.0f; // Dirac-Impuls

    HQRNReverbEngineAuto engine;
    engine.prepare(48000.0f);
    engine.setReverbTime(1.5f);
    engine.setMix(1.0f); // 100% Wet

    // Test a: Impulsantwort über 10 Blöcke (5120 Samples) erzeugen und Stabilität prüfen
    float energyFirstHalf = 0.0f;
    float energySecondHalf = 0.0f;

    for (size_t b = 0; b < 20; ++b) {
        engine.process(inL, inR, outL, outR);
        inL[0] = inR[0] = 0.0f;

        for (size_t i = 0; i < N; ++i) {
            float val = outL[i] * outL[i] + outR[i] * outR[i];
            if (b < 10) energyFirstHalf += val;
            else energySecondHalf += val;
        }
    }

    const bool energyDecayed = (energySecondHalf < energyFirstHalf) && (energyFirstHalf > 0.0f);
    reportTest("Engine asymptotische Energieabnahme (T60-Verhalten)", energyDecayed,
               "E_first=" + std::to_string(energyFirstHalf) + ", E_second=" + std::to_string(energySecondHalf));

    // Test b: Dynamisches Umschalten der Engine auf Skalar-Fallback
    engine.applyCapabilities(CpuCapabilities{ .hasAvx2 = false, .hasFma = false, .hasNeon = false });
    reportTest("Engine Fallback auf Skalar umschaltbar", engine.getSimdPath() == CpuSimdPath::Scalar,
               "Aktiver Pfad: " + std::string(engine.getSimdPathName()));

    engine.process(inL, inR, outL, outR);
    bool outputFinite = true;
    for (size_t i = 0; i < N; ++i) {
        if (!std::isfinite(outL[i]) || !std::isfinite(outR[i])) outputFinite = false;
    }
    reportTest("Engine Fallback-Ausgabe numerisch finit", outputFinite, "Keine NaNs oder Infs");

    // Test c: Allokationsfreiheit im Audiothread
    gAllocations.store(0, std::memory_order_relaxed);
    gTrackAllocations.store(true, std::memory_order_relaxed);

    for (int i = 0; i < 1000; ++i) {
        engine.process(inL, inR, outL, outR);
    }

    gTrackAllocations.store(false, std::memory_order_relaxed);
    const size_t allocs = gAllocations.load(std::memory_order_relaxed);

    reportTest("Engine 100% allokationsfrei (1000 process-Aufrufe)", allocs == 0,
               std::to_string(allocs) + " Allokationen");
}

int main() {
    std::cout << "===============================================================================\n"
              << "HQRN AUTOMATISIERTE TEST-SUITE: CPU-AUSWAHL, SIMD-PFADE & FALLBACKS\n"
              << "===============================================================================\n";

    testCpuDetectorSelectionMatrix();
    testBlockHalfbandSelectionAndFallback();
    testTruePeakClipperSelectionAndFallback();
    testHQRNReverbEngineAutoAndAllocations();

    std::cout << "\n===============================================================================\n"
              << "TEST-ERGEBNIS: " << gTestPassCount << " BESTANDEN, " << gTestFailCount << " FEHLGESCHLAGEN\n"
              << "===============================================================================\n";

    return (gTestFailCount == 0) ? 0 : 1;
}
