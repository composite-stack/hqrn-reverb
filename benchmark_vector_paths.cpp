#include "HQRNKernels.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <new>
#include <span>
#include <vector>

namespace {
std::atomic<std::size_t> gAllocations{0};
std::atomic<bool> gTrackAllocations{false};

struct Result {
    const char* path;
    double ms;
    double mSamplesPerSecond;
    double realtime;
    double speedup;
};

template <class Clipper>
double benchmarkClipper(std::size_t frames, std::size_t blockSize) {
    Clipper clipper;
    clipper.prepare(48000.0f);
    clipper.setCeilingDb(-0.1f);
    clipper.setKneeDb(3.0f);

    std::vector<float> inL(blockSize), inR(blockSize), outL(blockSize), outR(blockSize);
    for (std::size_t i = 0; i < blockSize; ++i) {
        inL[i] = 1.1f * std::sin(static_cast<float>(i) * 0.071f);
        inR[i] = 0.9f * std::cos(static_cast<float>(i) * 0.053f);
    }

    const std::size_t blocks = frames / blockSize;
    for (int i = 0; i < 64; ++i)
        clipper.processBlock(inL.data(), inR.data(), outL.data(), outR.data(), blockSize);

    volatile float checksum = 0.0f;
    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t b = 0; b < blocks; ++b) {
        clipper.processBlock(inL.data(), inR.data(), outL.data(), outR.data(), blockSize);
        checksum = checksum + outL[b % blockSize] + outR[(b * 7) % blockSize];
    }
    const auto end = std::chrono::steady_clock::now();
    (void)checksum;
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

template <class Clipper>
int measureLatency() {
    Clipper clipper;
    clipper.prepare(48000.0f);
    clipper.setCeilingDb(-0.1f);
    clipper.setKneeDb(3.0f);

    constexpr std::size_t blockSize = 64;
    std::vector<float> inL(blockSize), inR(blockSize), outL(blockSize), outR(blockSize);
    constexpr std::size_t totalFrames = 128;
    float peak = 0.0f;
    int peakIndex = -1;
    for (std::size_t offset = 0; offset < totalFrames; offset += blockSize) {
        std::fill(inL.begin(), inL.end(), 0.0f);
        std::fill(inR.begin(), inR.end(), 0.0f);
        if (offset == 0) inL[0] = inR[0] = 1.0f;
        clipper.processBlock(inL.data(), inR.data(), outL.data(), outR.data(), blockSize);
        for (std::size_t i = 0; i < blockSize; ++i) {
            const float value = std::max(std::abs(outL[i]), std::abs(outR[i]));
            if (value > peak) {
                peak = value;
                peakIndex = static_cast<int>(offset + i);
            }
        }
    }
    return peak > 0.0f ? peakIndex : -1;
}

template <class Clipper>
std::vector<float> renderReference(const std::vector<float>& inL,
                                   const std::vector<float>& inR,
                                   std::size_t blockSize) {
    Clipper clipper;
    clipper.prepare(48000.0f);
    clipper.setCeilingDb(-0.1f);
    clipper.setKneeDb(3.0f);
    std::vector<float> outL(inL.size()), outR(inR.size());
    for (std::size_t offset = 0; offset < inL.size(); offset += blockSize) {
        const std::size_t n = std::min(blockSize, inL.size() - offset);
        clipper.processBlock(inL.data() + offset, inR.data() + offset,
                             outL.data() + offset, outR.data() + offset, n);
    }
    outL.insert(outL.end(), outR.begin(), outR.end());
    return outL;
}

void printResult(const Result& result) {
    std::cout << "  " << std::left << std::setw(22) << result.path
              << " | " << std::right << std::setw(9) << std::fixed << std::setprecision(2) << result.ms << " ms"
              << " | " << std::setw(9) << std::setprecision(2) << result.mSamplesPerSecond << " MS/s"
              << " | " << std::setw(8) << std::setprecision(1) << result.realtime << "x"
              << " | " << std::setw(7) << std::setprecision(2) << result.speedup << "x\n";
}
} // namespace

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

int main() {
    using namespace dsp::hqrn;
    constexpr std::size_t totalFrames = 480000; // 10 seconds at 48 kHz
    constexpr std::size_t blockSize = 256;
    constexpr double sampleRate = 48000.0;

    std::cout << "===============================================================================\n"
              << "HQRN CPU-LAUFZEIT-ERKENNUNG & VEKTOR-PFAD BENCHMARK\n"
              << "===============================================================================\n";

    // 1. CPU-Erkennung ausgeben
    const bool avx2Supported = CpuDetector::hasAVX2();
    const bool neonNative = CpuDetector::hasNativeNEON();
    const CpuSimdPath detectedPath = CpuDetector::detectBestPath();

    std::cout << "[1] CPU-FEATURE ERKENNUNG ZUR LAUFZEIT:\n"
              << "  - x86 AVX2 + FMA:          " << (avx2Supported ? "JA (unterstuetzt)" : "NEIN") << "\n"
              << "  - ARM NEON (nativ):        " << (neonNative ? "JA (unterstuetzt)" : "NEIN (nicht nativ / x86-Emulation)") << "\n"
              << "  - Automatisch gewaehlt:    " << getCpuSimdPathName(detectedPath) << "\n\n";

    // 2. Durchsatz der Ausgangsstufe
    const double scalarMs = benchmarkClipper<TruePeakClipperScalar>(totalFrames, blockSize);
    const double avx2Ms   = benchmarkClipper<TruePeakClipperAVX2>(totalFrames, blockSize);
    const double neonMs   = benchmarkClipper<TruePeakClipperNEON>(totalFrames, blockSize);
    const double autoMs   = benchmarkClipper<TruePeakClipperAuto>(totalFrames, blockSize);
    const double referenceMs = scalarMs;

    std::cout << "[2] DURCHSATZ 4x TRUE-PEAK AUSGANGSSTUFE (FIR + SOFT-CLIPPER):\n"
              << "  " << std::left << std::setw(22) << "Pfad" << " | " << std::setw(12) << "Rechenzeit"
              << " | " << std::setw(12) << "Durchsatz" << " | " << std::setw(8) << "Echtzeit"
              << " | Speedup\n  " << std::string(80, '-') << '\n';
    auto makeResult = [&](const char* path, double ms) {
        const double mSamples = (static_cast<double>(totalFrames) / 1.0e6) / (ms / 1000.0);
        return Result{path, ms, mSamples, mSamples * 1.0e6 / sampleRate, referenceMs / ms};
    };
    printResult(makeResult("Skalar", scalarMs));
    printResult(makeResult("AVX2 (statisch)", avx2Ms));
    printResult(makeResult("NEON (x86-Emulation)", neonMs));
    printResult(makeResult("Auto-Dispatch", autoMs));

    // 3. Blockgrößen-Vergleich
    std::cout << "\n[3] BLOCKGROESSEN-VERGLEICH (Durchsatz in MS/s):\n"
              << "  " << std::left << std::setw(10) << "Block"
              << std::setw(15) << "Skalar"
              << std::setw(15) << "AVX2"
              << std::setw(15) << "NEON"
              << std::setw(15) << "Auto-Dispatch\n";
    for (const std::size_t bs : {64u, 128u, 256u, 512u}) {
        const std::size_t frames = (totalFrames / bs) * bs;
        const double s = benchmarkClipper<TruePeakClipperScalar>(frames, bs);
        const double a = benchmarkClipper<TruePeakClipperAVX2>(frames, bs);
        const double n = benchmarkClipper<TruePeakClipperNEON>(frames, bs);
        const double d = benchmarkClipper<TruePeakClipperAuto>(frames, bs);
        auto rate = [&](double ms) { return (static_cast<double>(frames) / 1.0e6) / (ms / 1000.0); };
        std::cout << "  " << std::left << std::setw(10) << bs
                  << std::setw(15) << std::fixed << std::setprecision(2) << rate(s)
                  << std::setw(15) << rate(a)
                  << std::setw(15) << rate(n)
                  << std::setw(15) << rate(d) << '\n';
    }

    // 4. Latenz-Verifikation
    std::cout << "\n[4] IMPULS-PEAK LATENZ (Streaming-Blockverarbeitung):\n"
              << "  - Skalar:        " << measureLatency<TruePeakClipperScalar>() << " Samples\n"
              << "  - AVX2:          " << measureLatency<TruePeakClipperAVX2>() << " Samples\n"
              << "  - NEON:          " << measureLatency<TruePeakClipperNEON>() << " Samples\n"
              << "  - Auto-Dispatch: " << measureLatency<TruePeakClipperAuto>() << " Samples\n";

    // 5. Numerische Konsistenz
    std::vector<float> testL(4096), testR(4096);
    for (std::size_t i = 0; i < testL.size(); ++i) {
        testL[i] = 1.25f * std::sin(static_cast<float>(i) * 0.037f);
        testR[i] = 1.15f * std::cos(static_cast<float>(i) * 0.061f);
    }
    const auto ref  = renderReference<TruePeakClipperScalar>(testL, testR, 256);
    const auto avx  = renderReference<TruePeakClipperAVX2>(testL, testR, 256);
    const auto neon = renderReference<TruePeakClipperNEON>(testL, testR, 256);
    const auto aut  = renderReference<TruePeakClipperAuto>(testL, testR, 256);
    float maxAvxError = 0.0f, maxNeonError = 0.0f, maxAutoError = 0.0f;
    for (std::size_t i = 0; i < ref.size(); ++i) {
        maxAvxError  = std::max(maxAvxError, std::abs(ref[i] - avx[i]));
        maxNeonError = std::max(maxNeonError, std::abs(ref[i] - neon[i]));
        maxAutoError = std::max(maxAutoError, std::abs(ref[i] - aut[i]));
    }
    std::cout << "\n[5] NUMERISCHE DIFFERENZ ZUR SKALAREN REFERENZ (Maximaler Peak-Fehler):\n"
              << "  - AVX2 vs. Skalar:          " << std::scientific << maxAvxError << '\n'
              << "  - NEON vs. Skalar:          " << maxNeonError << '\n'
              << "  - Auto-Dispatch vs. Skalar: " << maxAutoError << '\n';

    // 6. Allokationsfreiheit
    TruePeakClipperScalar allocScalar;
    TruePeakClipperAVX2   allocAVX2;
    TruePeakClipperNEON   allocNEON;
    TruePeakClipperAuto   allocAuto;
    HQRNReverbEngineAuto  allocEngine;

    allocScalar.prepare(sampleRate);
    allocAVX2.prepare(sampleRate);
    allocNEON.prepare(sampleRate);
    allocAuto.prepare(sampleRate);
    allocEngine.prepare(sampleRate);

    std::vector<float> inBlockL(blockSize, 0.25f), inBlockR(blockSize, -0.25f);
    std::vector<float> outBlockL(blockSize), outBlockR(blockSize);

    auto countClipperAllocations = [&](auto& clipper) {
        gAllocations.store(0, std::memory_order_relaxed);
        gTrackAllocations.store(true, std::memory_order_relaxed);
        for (int i = 0; i < 1000; ++i)
            clipper.processBlock(inBlockL.data(), inBlockR.data(), outBlockL.data(), outBlockR.data(), blockSize);
        gTrackAllocations.store(false, std::memory_order_relaxed);
        return gAllocations.load(std::memory_order_relaxed);
    };

    gAllocations.store(0, std::memory_order_relaxed);
    gTrackAllocations.store(true, std::memory_order_relaxed);
    for (int i = 0; i < 1000; ++i) {
        allocEngine.process(inBlockL, inBlockR, outBlockL, outBlockR);
    }
    gTrackAllocations.store(false, std::memory_order_relaxed);
    const std::size_t engineAllocs = gAllocations.load(std::memory_order_relaxed);

    std::cout << "\n[6] ECHTZEIT-AUDIOTHREAD VERIFIKATION (Heap-Allokationen in 1000 Bloecken):\n"
              << "  - TruePeakClipperScalar: " << countClipperAllocations(allocScalar) << " Allokationen\n"
              << "  - TruePeakClipperAVX2:   " << countClipperAllocations(allocAVX2) << " Allokationen\n"
              << "  - TruePeakClipperNEON:   " << countClipperAllocations(allocNEON) << " Allokationen\n"
              << "  - TruePeakClipperAuto:   " << countClipperAllocations(allocAuto) << " Allokationen\n"
              << "  - HQRNReverbEngineAuto:  " << engineAllocs << " Allokationen\n";

    std::cout << "\n===============================================================================\n"
              << "BENCHMARK ERFOLGREICH BEENDET.\n"
              << "===============================================================================\n";

    return (maxAutoError < 1.0e-4f) ? 0 : 1;
}
