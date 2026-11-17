#include "/mnt/documents/HQRNReverbEngine.hpp"
#include <iostream>
#include <vector>
#include <chrono>
#include <cmath>
#include <numeric>
#include <atomic>
#include <cassert>
#include <iomanip>

// ============================================================================
// HEAP ALLOCATION MONITOR (Intercept global operator new/delete)
// ============================================================================
static std::atomic<size_t> g_heapAllocCount{0};
static std::atomic<size_t> g_heapAllocBytes{0};
static bool g_trackAllocations = false;

void* operator new(size_t size) {
    if (g_trackAllocations) {
        g_heapAllocCount.fetch_add(1, std::memory_order_relaxed);
        g_heapAllocBytes.fetch_add(size, std::memory_order_relaxed);
    }
    void* ptr = std::malloc(size);
    if (!ptr) throw std::bad_alloc();
    return ptr;
}

void operator delete(void* ptr) noexcept {
    std::free(ptr);
}

void operator delete(void* ptr, size_t) noexcept {
    std::free(ptr);
}

// ============================================================================
// BENCHMARK SUITE
// ============================================================================

int main() {
    using namespace dsp::hqrn;

    constexpr float fs = 48000.0f;
    constexpr size_t blockSize = 128;
    constexpr size_t totalSeconds = 10;
    constexpr size_t totalSamples = static_cast<size_t>(fs * totalSeconds);
    constexpr size_t numBlocks = totalSamples / blockSize;

    std::cout << "==========================================================" << std::endl;
    std::cout << "HQRN C++20 DSP BENCHMARK: SIMD, LATENZ & ALLOKATIONSFREIHEIT" << std::endl;
    std::cout << "==========================================================" << std::endl;

    // 1. ALLOCATION FREEDOM VERIFICATION
    HQRNReverbEngine engine;
    engine.prepare(fs);
    engine.setReverbTime(1.8f);
    engine.setBaseFrequency(120.0f);
    engine.setMix(1.0f);
    engine.setClipperParams(-0.1f, 3.0f, true);

    std::vector<float> inL(blockSize, 0.5f);
    std::vector<float> inR(blockSize, -0.5f);
    std::vector<float> outL(blockSize, 0.0f);
    std::vector<float> outR(blockSize, 0.0f);

    // Warmup
    engine.process(inL, inR, outL, outR);

    // Start tracking allocations strictly around process()
    g_heapAllocCount = 0;
    g_heapAllocBytes = 0;
    g_trackAllocations = true;

    for (size_t b = 0; b < 1000; ++b) {
        engine.process(inL, inR, outL, outR);
    }

    g_trackAllocations = false;
    size_t allocsInProcess = g_heapAllocCount.load();
    size_t bytesInProcess = g_heapAllocBytes.load();

    std::cout << "\n[1] ALLOKATIONSFREIHEIT IM AUDIOTHREAD:" << std::endl;
    std::cout << "  - Allokationen in 1000 Block-Aufrufen: " << allocsInProcess << std::endl;
    std::cout << "  - Allokierte Bytes: " << bytesInProcess << " Bytes" << std::endl;
    std::cout << "  - Status: " << (allocsInProcess == 0 ? "PASSED (100% allokationsfrei)" : "FAILED") << std::endl;

    // 2. LATENZ-MESSUNG (Gruppenlaufzeit)
    // - Linearer Kern ist zeitinvarianter rekursiver Filter -> 0 Samples algorithmische Latenz.
    // - 4x True-Peak Clipper: 2 Kaskaden von 2x Halbbandfiltern.
    //   Stufe 1 (1x -> 2x): N=31 -> Filterdelay bei 2x = 15 Samples.
    //   Stufe 2 (2x -> 4x): N=31 -> Filterdelay bei 4x = 15 Samples (bei 2x = 7.5 Samples).
    //   Upsampling Gesamt = 7.5 + 3.75 = 11.25 Samples bei 1x.
    //   Dezimation Gesamt = 3.75 + 7.5 = 11.25 Samples bei 1x.
    //   Gesamtlatenz des 4x Clippers = 15 + 7.5 = 22.5 Samples bzw. mit phasenrichtiger Kaskade.
    // Messung durch Einzelimpuls-Erkennung:
    LinearHQRNKernel kernelOnly;
    kernelOnly.prepare(fs);
    kernelOnly.reset();

    float kOutL = 0.0f, kOutR = 0.0f;
    kernelOnly.processSample(1.0f, 1.0f, kOutL, kOutR);
    bool kernelZeroLatency = (kOutL != 0.0f || kOutR != 0.0f);

    TruePeakClipper clipperOnly;
    clipperOnly.prepare(fs);
    clipperOnly.setCeilingDb(-0.1f);
    clipperOnly.setKneeDb(3.0f);
    clipperOnly.setEnabled(true);

    int clipperLatency = -1;
    for (int n = 0; n < 200; ++n) {
        float in = (n == 0) ? 1.0f : 0.0f;
        float cOutL = 0.0f, cOutR = 0.0f;
        clipperOnly.processStereo(in, in, cOutL, cOutR);
        if (clipperLatency == -1 && std::abs(cOutL) > 0.1f) {
            clipperLatency = n;
        }
    }

    std::cout << "\n[2] LATENZ & GRUPPENLAUFZEIT (bei " << fs << " Hz):" << std::endl;
    std::cout << "  - Resonatorkern: " << (kernelZeroLatency ? "0 Samples (Echtzeit / IIR)" : "Latenz vorhanden") << std::endl;
    std::cout << "  - 4x True-Peak Clipper: " << clipperLatency << " Samples ("
              << (clipperLatency * 1000.0f / fs) << " ms)" << std::endl;
    std::cout << "  - Gesamt-Engine: " << clipperLatency << " Samples ("
              << (clipperLatency * 1000.0f / fs) << " ms)" << std::endl;

    // 3. THROUGHPUT & SIMD BENCHMARKS
    // Wir benchmarken:
    // a) Nur Linearer HQRN-Kern
    // b) Nur 4x True-Peak Clipper
    // c) Gesamte Engine (Kernel + Gain + Clipper)

    // A. KERNEL ALONE
    auto startKernel = std::chrono::high_resolution_clock::now();
    for (size_t b = 0; b < numBlocks; ++b) {
        for (size_t i = 0; i < blockSize; ++i) {
            float dummyL = 0.0f, dummyR = 0.0f;
            kernelOnly.processSample(inL[i], inR[i], dummyL, dummyR);
            outL[i] = dummyL;
            outR[i] = dummyR;
        }
    }
    auto endKernel = std::chrono::high_resolution_clock::now();
    double kernelTimeMs = std::chrono::duration<double, std::milli>(endKernel - startKernel).count();
    double kernelThroughputMS = (totalSamples / 1e6) / (kernelTimeMs / 1000.0);
    double kernelCpuUsage = (kernelTimeMs / (totalSeconds * 1000.0)) * 100.0;

    // B. CLIPPER ALONE
    auto startClipper = std::chrono::high_resolution_clock::now();
    for (size_t b = 0; b < numBlocks; ++b) {
        for (size_t i = 0; i < blockSize; ++i) {
            float dummyL = 0.0f, dummyR = 0.0f;
            clipperOnly.processStereo(inL[i], inR[i], dummyL, dummyR);
            outL[i] = dummyL;
            outR[i] = dummyR;
        }
    }
    auto endClipper = std::chrono::high_resolution_clock::now();
    double clipperTimeMs = std::chrono::duration<double, std::milli>(endClipper - startClipper).count();
    double clipperThroughputMS = (totalSamples / 1e6) / (clipperTimeMs / 1000.0);
    double clipperCpuUsage = (clipperTimeMs / (totalSeconds * 1000.0)) * 100.0;

    // C. FULL ENGINE (Block-Processing mit std::span)
    auto startFull = std::chrono::high_resolution_clock::now();
    for (size_t b = 0; b < numBlocks; ++b) {
        engine.process(inL, inR, outL, outR);
    }
    auto endFull = std::chrono::high_resolution_clock::now();
    double fullTimeMs = std::chrono::duration<double, std::milli>(endFull - startFull).count();
    double fullThroughputMS = (totalSamples / 1e6) / (fullTimeMs / 1000.0);
    double fullCpuUsage = (fullTimeMs / (totalSeconds * 1000.0)) * 100.0;
    double cyclesPerSample = (fullTimeMs / 1000.0 * 3.0e9) / totalSamples; // Basis: 3.0 GHz Core

    std::cout << "\n[3] DURCHSATZ & RECHENZEIT (" << totalSeconds << "s Audio, " << totalSamples << " Stereo-Frames, Blockgröße " << blockSize << "):" << std::endl;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  a) Resonatorkern (32 Zustände, FWHT-16):" << std::endl;
    std::cout << "     - Rechenzeit: " << kernelTimeMs << " ms (" << kernelCpuUsage << "% eines Audio-Cores)" << std::endl;
    std::cout << "     - Durchsatz:  " << kernelThroughputMS << " MSamples/s (" << (kernelThroughputMS * 1e6 / fs) << "x Realtime)" << std::endl;

    std::cout << "  b) 4x True-Peak Clipper (Polyphasen-Halbband + Hermite-Knee):" << std::endl;
    std::cout << "     - Rechenzeit: " << clipperTimeMs << " ms (" << clipperCpuUsage << "% eines Audio-Cores)" << std::endl;
    std::cout << "     - Durchsatz:  " << clipperThroughputMS << " MSamples/s (" << (clipperThroughputMS * 1e6 / fs) << "x Realtime)" << std::endl;

    std::cout << "  c) Gesamt-Engine (Kernel + Gain-Staging + 4x Clipper):" << std::endl;
    std::cout << "     - Rechenzeit: " << fullTimeMs << " ms (" << fullCpuUsage << "% eines Audio-Cores)" << std::endl;
    std::cout << "     - Durchsatz:  " << fullThroughputMS << " MSamples/s (" << (fullThroughputMS * 1e6 / fs) << "x Realtime)" << std::endl;
    std::cout << "     - Geschätzte Takte/Sample: ~" << std::setprecision(1) << cyclesPerSample << " Zyklen @ 3.0 GHz" << std::endl;

    return 0;
}
