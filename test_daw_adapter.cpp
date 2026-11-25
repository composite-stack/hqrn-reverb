#include <execinfo.h>
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
#include "HQRNDawAdapter.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

// ============================================================================
// ALLOKATIONS-TRACKER FUER ECHTZEIT-VERIFIKATION
// ============================================================================

static bool g_trackAllocations = false;
static uint64_t g_allocationCount = 0;
static uint64_t g_allocatedBytes = 0;

void* operator new(size_t size) {
    if (g_trackAllocations) {

        ++g_allocationCount;
        g_allocatedBytes += size;
    }
    return std::malloc(size);
}

void operator delete(void* ptr) noexcept {
    std::free(ptr);
}

void operator delete(void* ptr, size_t) noexcept {
    std::free(ptr);
}

// ============================================================================
// TEST-HELPER
// ============================================================================

static int g_testsPassed = 0;
static int g_testsFailed = 0;

void reportTest(const std::string& name, bool passed, const std::string& details = "") {
    if (passed) {
        std::cout << "  [PASS] " << name;
        if (!details.empty()) {
            std::cout << " (" << details << ")";
        }
        std::cout << "\n";
        ++g_testsPassed;
    } else {
        std::cerr << "  [FAIL] " << name;
        if (!details.empty()) {
            std::cerr << " -> " << details;
        }
        std::cerr << "\n";
        ++g_testsFailed;
    }
}

// ============================================================================
// TEST 1: GRUNDFUNKTIONEN DES ALIGNED CIRCULAR BUFFERS
// ============================================================================

void testAlignedCircularBuffer() {
    std::cout << "\n[TEST 1] ALIGNED CIRCULAR BUFFER (FIFO):\n";

    hqrn::AlignedCircularBuffer<float, 64> fifo;
    fifo.prepare(100);

    reportTest("Zweierpotenz-Kapazitaet (min 100 -> 256)", fifo.capacity() == 256,
               "Cap: " + std::to_string(fifo.capacity()));

    reportTest("64-Byte Speicherausrichtung", (reinterpret_cast<uintptr_t>(fifo.data()) % 64) == 0,
               "Addr: " + std::to_string(reinterpret_cast<uintptr_t>(fifo.data())));

    std::vector<float> inData(150);
    std::iota(inData.begin(), inData.end(), 1.0f);

    fifo.push(inData.data(), 150);
    reportTest("Push & AvailableRead", fifo.availableRead() == 150,
               "Avail: " + std::to_string(fifo.availableRead()));

    std::vector<float> outData(100);
    fifo.pop(outData.data(), 100);
    reportTest("Pop 100 Samples & Rest", fifo.availableRead() == 50,
               "Rest: " + std::to_string(fifo.availableRead()));

    bool matchFirst100 = true;
    for (size_t i = 0; i < 100; ++i) {
        if (outData[i] != inData[i]) {
            matchFirst100 = false;
            break;
        }
    }
    reportTest("Daten-Integritaet nach Wrap-Around", matchFirst100);

    // Weiteren Push ueber Ring-Grenze hinweg testen
    std::vector<float> inData2(150);
    std::iota(inData2.begin(), inData2.end(), 1000.0f);
    fifo.push(inData2.data(), 150);

    std::vector<float> outRemaining(200);
    fifo.pop(outRemaining.data(), 200);

    bool matchAll = true;
    for (size_t i = 0; i < 50; ++i) {
        if (outRemaining[i] != inData[100 + i]) matchAll = false;
    }
    for (size_t i = 0; i < 150; ++i) {
        if (outRemaining[50 + i] != inData2[i]) matchAll = false;
    }
    reportTest("Zirkulaere Kontinuitaet ueber Puffer-Grenzen", matchAll && fifo.availableRead() == 0);
}

// ============================================================================
// TEST 2: VARIABLE BLOCKGROESSEN & SIGNAL-KONTINUITAET (DIRECT MODUS)
// ============================================================================

void testDirectModeVariableBlocks() {
    std::cout << "\n[TEST 2] DIRECT-SLICING-MODUS (VARIABLE BLOCKGROESSEN):\n";

    constexpr size_t TOTAL_FRAMES = 4096;
    std::vector<float> inputL(TOTAL_FRAMES), inputR(TOTAL_FRAMES);

    // Impuls am Anfang fuer eindeutige Antwort
    inputL[0] = 1.0f;
    inputR[0] = 1.0f;

    // Referenz: Monolithischer Durchlauf (1 Block mit 4096 Frames)
    hqrn::HQRNDawAdapter<hqrn::HQRNReverbEngineScalar> refAdapter(hqrn::DawAdapterMode::Direct);
    refAdapter.prepare(48000.0, 4096);
    refAdapter.setMix(0.5f);

    std::vector<float> refOutL(TOTAL_FRAMES), refOutR(TOTAL_FRAMES);
    std::span<const float> inSpanL(inputL), inSpanR(inputR);
    std::span<float> refSpanL(refOutL), refSpanR(refOutR);
    refAdapter.process(inSpanL, inSpanR, refSpanL, refSpanR);

    // Test: Fragmentierter Durchlauf mit stark variierenden Blockgroessen
    hqrn::HQRNDawAdapter<hqrn::HQRNReverbEngineScalar> varAdapter(hqrn::DawAdapterMode::Direct);
    varAdapter.prepare(48000.0, 4096);
    varAdapter.setMix(0.5f);

    std::vector<float> varOutL(TOTAL_FRAMES), varOutR(TOTAL_FRAMES);

    // Typische DAW-Fragmentierung: ungerade, klein, gross
    const std::vector<size_t> blockSizes = {
        1, 7, 13, 32, 63, 64, 100, 127, 128, 255, 256, 441, 512, 1024, 73, 17, 988
    };

    size_t offset = 0;
    size_t blockIdx = 0;
    while (offset < TOTAL_FRAMES) {
        size_t blk = std::min(blockSizes[blockIdx % blockSizes.size()], TOTAL_FRAMES - offset);
        std::span<const float> curInL(inputL.data() + offset, blk);
        std::span<const float> curInR(inputR.data() + offset, blk);
        std::span<float> curOutL(varOutL.data() + offset, blk);
        std::span<float> curOutR(varOutR.data() + offset, blk);

        varAdapter.process(curInL, curInR, curOutL, curOutR);
        offset += blk;
        ++blockIdx;
    }

    // Differenz messen
    float maxDiffL = 0.0f;
    float maxDiffR = 0.0f;
    for (size_t i = 0; i < TOTAL_FRAMES; ++i) {
        maxDiffL = std::max(maxDiffL, std::abs(refOutL[i] - varOutL[i]));
        maxDiffR = std::max(maxDiffR, std::abs(refOutR[i] - varOutR[i]));
    }

    reportTest("Direct-Modus Identitaet bei variablen Blockgroessen (L)", maxDiffL < 1e-6f,
               "Max Diff: " + std::to_string(maxDiffL));
    reportTest("Direct-Modus Identitaet bei variablen Blockgroessen (R)", maxDiffR < 1e-6f,
               "Max Diff: " + std::to_string(maxDiffR));

    // Latenzpruefung
    reportTest("Direct-Modus gemeldete Latenz (22 Samples)", varAdapter.getLatencySamples() == 22,
               "Latenz: " + std::to_string(varAdapter.getLatencySamples()));
}

// ============================================================================
// TEST 3: QUANTIZED FIFO MODUS & DETERMINISTISCHE VERARBEITUNG
// ============================================================================

void testQuantizedFifoMode() {
    std::cout << "\n[TEST 3] QUANTIZED-FIFO-MODUS (FESTE CHUNKS & PRE-ROLL LATENCY):\n";

    constexpr size_t QUANTUM = 128;
    constexpr size_t TOTAL_FRAMES = 4096;

    std::vector<float> inputL(TOTAL_FRAMES), inputR(TOTAL_FRAMES);
    inputL[0] = 1.0f;
    inputR[0] = 1.0f;

    // Referenz: Blockweise exakt im Quantum getaktet
    hqrn::HQRNDawAdapter<hqrn::HQRNReverbEngineScalar> refFifo(hqrn::DawAdapterMode::QuantizedFifo, QUANTUM);
    refFifo.prepare(48000.0, 4096, QUANTUM);
    refFifo.setMix(0.5f);

    std::vector<float> refOutL(TOTAL_FRAMES), refOutR(TOTAL_FRAMES);
    for (size_t offset = 0; offset < TOTAL_FRAMES; offset += QUANTUM) {
        size_t blk = std::min(QUANTUM, TOTAL_FRAMES - offset);
        std::span<const float> curInL(inputL.data() + offset, blk);
        std::span<const float> curInR(inputR.data() + offset, blk);
        std::span<float> curOutL(refOutL.data() + offset, blk);
        std::span<float> curOutR(refOutR.data() + offset, blk);
        refFifo.process(curInL, curInR, curOutL, curOutR);
    }

    // Test: Fragmentierter Durchlauf mit chaotischen Blockgroessen (1, 7, 53, 441, ...)
    hqrn::HQRNDawAdapter<hqrn::HQRNReverbEngineScalar> varFifo(hqrn::DawAdapterMode::QuantizedFifo, QUANTUM);
    varFifo.prepare(48000.0, 4096, QUANTUM);
    varFifo.setMix(0.5f);

    std::vector<float> varOutL(TOTAL_FRAMES), varOutR(TOTAL_FRAMES);

    std::mt19937 rng(1337);
    std::uniform_int_distribution<size_t> dist(1, 300);

    size_t offset = 0;
    while (offset < TOTAL_FRAMES) {
        size_t blk = std::min(dist(rng), TOTAL_FRAMES - offset);
        std::span<const float> curInL(inputL.data() + offset, blk);
        std::span<const float> curInR(inputR.data() + offset, blk);
        std::span<float> curOutL(varOutL.data() + offset, blk);
        std::span<float> curOutR(varOutR.data() + offset, blk);

        varFifo.process(curInL, curInR, curOutL, curOutR);
        offset += blk;
    }

    float maxDiffL = 0.0f;
    float maxDiffR = 0.0f;
    for (size_t i = 0; i < TOTAL_FRAMES; ++i) {
        maxDiffL = std::max(maxDiffL, std::abs(refOutL[i] - varOutL[i]));
        maxDiffR = std::max(maxDiffR, std::abs(refOutR[i] - varOutR[i]));
    }

    reportTest("Quantized-FIFO Identitaet bei Zufallsbloecken (L)", maxDiffL < 1e-6f,
               "Max Diff: " + std::to_string(maxDiffL));
    reportTest("Quantized-FIFO Identitaet bei Zufallsbloecken (R)", maxDiffR < 1e-6f,
               "Max Diff: " + std::to_string(maxDiffR));

    // Latenzpruefung: Q (128) + Clipper (22) = 150 Samples
    reportTest("Quantized-FIFO PDC-Latenzmeldung (128 + 22 = 150)", varFifo.getLatencySamples() == 150,
               "Latenz: " + std::to_string(varFifo.getLatencySamples()));

    // Pruefen, dass die ersten Q Samples exakt Stille sind (Pre-Roll)
    bool preRollSilent = true;
    for (size_t i = 0; i < QUANTUM; ++i) {
        if (std::abs(varOutL[i]) > 0.0f || std::abs(varOutR[i]) > 0.0f) {
            preRollSilent = false;
            break;
        }
    }
    reportTest("Pre-Roll Stille in den ersten Q Samples", preRollSilent);
}

// ============================================================================
// TEST 4: IN-PLACE-VERARBEITUNGSSICHERHEIT
// ============================================================================

void testInPlaceProcessing() {
    std::cout << "\n[TEST 4] IN-PLACE PUFFER-SICHERHEIT (in == out Pointer):\n";

    constexpr size_t N = 1024;
    std::vector<float> inL(N, 0.5f), inR(N, -0.5f);

    // Out-of-place Referenz
    hqrn::HQRNDawAdapterAuto oopAdapter;
    oopAdapter.prepare(48000.0, N);
    std::vector<float> oopOutL(N), oopOutR(N);

    const float* const inPtrs[2] = { inL.data(), inR.data() };
    float* const outPtrs[2] = { oopOutL.data(), oopOutR.data() };
    oopAdapter.process(inPtrs, outPtrs, N);

    // In-place Test
    hqrn::HQRNDawAdapterAuto ipAdapter;
    ipAdapter.prepare(48000.0, N);
    std::vector<float> ipBufL = inL;
    std::vector<float> ipBufR = inR;

    float* const ipPtrs[2] = { ipBufL.data(), ipBufR.data() };
    ipAdapter.process(ipPtrs, ipPtrs, N); // in == out

    float diffL = 0.0f, diffR = 0.0f;
    for (size_t i = 0; i < N; ++i) {
        diffL = std::max(diffL, std::abs(oopOutL[i] - ipBufL[i]));
        diffR = std::max(diffR, std::abs(oopOutR[i] - ipBufR[i]));
    }

    reportTest("In-Place Identitaet zu Out-of-Place (L)", diffL < 1e-6f, "Diff: " + std::to_string(diffL));
    reportTest("In-Place Identitaet zu Out-of-Place (R)", diffR < 1e-6f, "Diff: " + std::to_string(diffR));
}

// ============================================================================
// TEST 5: KANAL-KONFIGURATIONEN & INTERLEAVED SUPPORT
// ============================================================================

void testChannelConfigurations() {
    std::cout << "\n[TEST 5] KANAL-LAYOUTS (MONO/STEREO/INTERLEAVED):\n";

    constexpr size_t N = 512;
    std::vector<float> monoIn(N, 0.7f);
    std::vector<float> stereoOutL(N), stereoOutR(N);

    hqrn::HQRNDawAdapterAuto adapter;
    adapter.prepare(48000.0, N);

    // 1. Mono in (1 Channel) -> Stereo out (2 Channels)
    const float* const monoPtrs[1] = { monoIn.data() };
    float* const stereoPtrs[2] = { stereoOutL.data(), stereoOutR.data() };

    adapter.process(monoPtrs, stereoPtrs, N, 1, 2);

    bool stereoActive = false;
    for (size_t i = 0; i < N; ++i) {
        if (std::abs(stereoOutL[i]) > 0.0f || std::abs(stereoOutR[i]) > 0.0f) {
            stereoActive = true;
            break;
        }
    }
    reportTest("Mono-In / Stereo-Out erzeugt gueltigen Stereo-Stream", stereoActive);

    // 2. Interleaved Processing
    std::vector<float> interIn(2 * N, 0.3f);
    std::vector<float> interOut(2 * N, 0.0f);

    hqrn::HQRNDawAdapterAuto interAdapter;
    interAdapter.prepare(48000.0, N);
    interAdapter.processInterleaved(interIn.data(), interOut.data(), N);

    bool interActive = false;
    for (size_t i = 0; i < 2 * N; ++i) {
        if (std::abs(interOut[i]) > 0.0f) {
            interActive = true;
            break;
        }
    }
    reportTest("Interleaved Stereo-Verarbeitung aktiv und fehlerfrei", interActive);
}

// ============================================================================
// TEST 6: STRIKTE ECHTZEIT-ALLOKATIONSFREIHEIT
// ============================================================================

void testZeroAllocations() {
    std::cout << "\n[TEST 6] AUDIO-THREAD ECHTZEIT-ALLOKATIONSFREIHEIT:\n";

    hqrn::HQRNDawAdapterAuto adapter(hqrn::DawAdapterMode::QuantizedFifo, 128);
    adapter.prepare(48000.0, 1024, 128);

    std::vector<float> inL(1024, 0.1f), inR(1024, 0.1f);
    std::vector<float> outL(1024), outR(1024);

    const float* const inPtrs[2] = { inL.data(), inR.data() };
    float* const outPtrs[2] = { outL.data(), outR.data() };

    // 1000 Aufrufe mit variablen Blockgroessen (std::array fuer 0 Test-Allokationen)
    constexpr std::array<size_t, 9> varBlocks = { 1, 7, 32, 64, 128, 255, 300, 512, 1024 };

    g_allocationCount = 0;
    g_allocatedBytes = 0;
    g_trackAllocations = true;

    for (int iter = 0; iter < 1000; ++iter) {
        size_t blk = varBlocks[iter % varBlocks.size()];
        adapter.process(inPtrs, outPtrs, blk);
    }

    g_trackAllocations = false;

    reportTest("1000 process()-Aufrufe mit variablen Bloecken ohne Allokation",
               g_allocationCount == 0,
               "Allocs: " + std::to_string(g_allocationCount) + ", Bytes: " + std::to_string(g_allocatedBytes));
}

// ============================================================================
// TEST 7: RESET-VERHALTEN & FLUSH
// ============================================================================

void testResetBehavior() {
    std::cout << "\n[TEST 7] RESET & FLUSH VERHALTEN:\n";

    hqrn::HQRNDawAdapterAuto adapter(hqrn::DawAdapterMode::QuantizedFifo, 128);
    adapter.prepare(48000.0, 512, 128);

    std::vector<float> inL(512, 1.0f), inR(512, 1.0f);
    std::vector<float> outL(512), outR(512);

    // Audio reinschicken
    adapter.process(std::span<const float>(inL), std::span<const float>(inR),
                    std::span<float>(outL), std::span<float>(outR));

    // Reset aufrufen
    adapter.reset();

    // Nach Reset sollte Stille eingespeist auch exakt Stille ergeben
    std::vector<float> silenceIn(512, 0.0f);
    std::vector<float> postResetOutL(512, 99.0f), postResetOutR(512, 99.0f);

    adapter.process(std::span<const float>(silenceIn), std::span<const float>(silenceIn),
                    std::span<float>(postResetOutL), std::span<float>(postResetOutR));

    bool completelySilent = true;
    for (size_t i = 0; i < 512; ++i) {
        if (std::abs(postResetOutL[i]) > 1e-12f || std::abs(postResetOutR[i]) > 1e-12f) {
            completelySilent = false;
            break;
        }
    }
    reportTest("Reset loescht alle Zustaende & FIFOs vollstaendig (Stille nach Reset)", completelySilent);
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "===============================================================================\n";
    std::cout << "HQRN AUTOMATISIERTE TEST-SUITE: DAW-PUFFER-ADAPTER (VARIABLE BLOCKGROESSEN)\n";
    std::cout << "===============================================================================\n";

    testAlignedCircularBuffer();
    testDirectModeVariableBlocks();
    testQuantizedFifoMode();
    testInPlaceProcessing();
    testChannelConfigurations();
    testZeroAllocations();
    testResetBehavior();

    std::cout << "\n===============================================================================\n";
    std::cout << "TEST-ERGEBNIS: " << g_testsPassed << " BESTANDEN, " << g_testsFailed << " FEHLGESCHLAGEN\n";
    std::cout << "===============================================================================\n";

    return (g_testsFailed == 0) ? 0 : 1;
}
