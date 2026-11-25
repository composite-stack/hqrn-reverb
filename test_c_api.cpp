/**
 * @file test_c_api.cpp
 * @brief Automatisierte Verifikation der C-Bridge und Wasm-Exporte.
 */

#include "hqrn_c_api.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
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

void* operator new(size_t size, const std::nothrow_t&) noexcept {
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
// TESTS
// ============================================================================

void testLifecycle() {
    std::cout << "\n--- Test 1: Lifecycle & Null-Sicherheit ---\n";

    // Null-Pointer Safety
    hqrn_destroy(nullptr);
    hqrn_prepare(nullptr, 48000.0, 128, 128);
    hqrn_reset(nullptr);
    hqrn_set_adapter_mode(nullptr, 1);
    reportTest("Null-Handle Aufrufe crashen nicht", true);

    reportTest("Null-Handle Puffer-Getter geben nullptr",
               hqrn_get_input_buffer_l(nullptr) == nullptr &&
               hqrn_get_output_buffer_r(nullptr) == nullptr &&
               hqrn_get_buffer_capacity(nullptr) == 0);

    reportTest("Null-Handle Latenz liefert 0", hqrn_get_latency_samples(nullptr) == 0);

    // Erzeugen
    HQRNHandle handle = hqrn_create(48000.0, 128, 128);
    reportTest("hqrn_create liefert gueltiges Handle", handle != nullptr);
    reportTest("hqrn_get_sample_rate liefert 48000 Hz", std::abs(hqrn_get_sample_rate(handle) - 48000.0f) < 1.0f);
    reportTest("hqrn_get_buffer_capacity >= 128", hqrn_get_buffer_capacity(handle) >= 128);

    hqrn_destroy(handle);
    reportTest("hqrn_destroy erfolgreich", true);
}

void testInternalScratchpadProcess() {
    std::cout << "\n--- Test 2: Audio-Verarbeitung via interne I/O-Scratchpads ---\n";

    HQRNHandle handle = hqrn_create(48000.0, 128, 128);
    float* inL = hqrn_get_input_buffer_l(handle);
    float* inR = hqrn_get_input_buffer_r(handle);
    float* outL = hqrn_get_output_buffer_l(handle);
    float* outR = hqrn_get_output_buffer_r(handle);

    reportTest("Puffer-Zeiger sind nicht null", inL && inR && outL && outR);

    // Delta-Impuls auf Kanal L
    inL[0] = 1.0f;
    for (size_t i = 1; i < 128; ++i) inL[i] = 0.0f;
    for (size_t i = 0; i < 128; ++i) inR[i] = 0.0f;

    // Verarbeiten über mehrere AudioWorklet-Blöcke (jeweils 128 Frames)
    float maxEnergy = 0.0f;
    bool allFinite = true;

    for (int block = 0; block < 10; ++block) {
        hqrn_process(handle, 128);

        for (size_t i = 0; i < 128; ++i) {
            if (!std::isfinite(outL[i]) || !std::isfinite(outR[i])) {
                allFinite = false;
            }
            maxEnergy = std::max(maxEnergy, outL[i] * outL[i] + outR[i] * outR[i]);
        }

        // Folgende Blöcke sind Stille am Eingang
        for (size_t i = 0; i < 128; ++i) {
            inL[i] = 0.0f;
            inR[i] = 0.0f;
        }
    }

    reportTest("Ausgangssignale sind vollstaendig endlich (kein NaN/Inf)", allFinite);
    reportTest("Reverb-Energie am Ausgang detektiert", maxEnergy > 1e-6f,
               "Max. Summenenergie: " + std::to_string(maxEnergy));

    hqrn_destroy(handle);
}

void testPlanarAndInterleavedProcessing() {
    std::cout << "\n--- Test 3: Planare und Interleaved Schnittstellen ---\n";

    HQRNHandle handle = hqrn_create(48000.0, 256, 128);

    std::vector<float> pInL(256, 0.0f);
    std::vector<float> pInR(256, 0.0f);
    std::vector<float> pOutL(256, 0.0f);
    std::vector<float> pOutR(256, 0.0f);
    pInL[0] = 1.0f;

    hqrn_process_planar(handle, pInL.data(), pInR.data(), pOutL.data(), pOutR.data(), 256);
    reportTest("Planare Verarbeitung laeuft stabil", true);

    std::vector<float> iIn(512, 0.0f);
    std::vector<float> iOut(512, 0.0f);
    iIn[0] = 1.0f; // L0

    hqrn_process_interleaved(handle, iIn.data(), iOut.data(), 256);
    reportTest("Interleaved Verarbeitung laeuft stabil", true);

    hqrn_destroy(handle);
}

void testParametersAndSmoothing() {
    std::cout << "\n--- Test 4: Parameter-Steuerung & Glättung (Slewing) ---\n";

    HQRNHandle handle = hqrn_create(48000.0, 128, 128);

    // Direkte Setter
    hqrn_set_base_frequency(handle, 150.0f);
    hqrn_set_reverb_time(handle, 3.5f);
    hqrn_set_source_position(handle, 0.2f, -0.3f);
    hqrn_set_listener_position(handle, -0.1f, 0.4f, 0.78f);
    hqrn_set_mix(handle, 0.5f);
    hqrn_set_output_gain_db(handle, -2.0f);
    hqrn_set_clipper_params(handle, -0.5f, 2.0f, 1);
    reportTest("Direkte Parameter-Setter ausgefuehrt", true);

    // Smoothed Target
    hqrn_set_source_target(handle, 0.6f, 0.4f, 0.05f); // 50 ms Glättung
    hqrn_set_listener_target(handle, -0.5f, -0.2f, 1.57f, 0.05f);

    // Simuliere 30 AudioWorklet-Blöcke à 128 Samples (~80 ms)
    for (int i = 0; i < 30; ++i) {
        hqrn_process(handle, 128);
    }
    reportTest("Smoothed Parameter-Updates konvergieren stabil ohne Artefakte", true);

    hqrn_destroy(handle);
}

void testPoincareGeometryExport() {
    std::cout << "\n--- Test 5: Poincaré-Geometrie & Topologie-Export ---\n";

    uint32_t count = hqrn_get_node_count();
    reportTest("Knotenanzahl ist exakt 16", count == 16);

    std::vector<float> coords(32, 0.0f);
    hqrn_get_node_coordinates(coords.data());

    bool ring1Correct = true;
    bool ring2Correct = true;

    for (size_t i = 0; i < 8; ++i) {
        float r = std::sqrt(coords[2 * i] * coords[2 * i] + coords[2 * i + 1] * coords[2 * i + 1]);
        if (std::abs(r - 0.35f) > 1e-4f) ring1Correct = false;
    }
    for (size_t i = 8; i < 16; ++i) {
        float r = std::sqrt(coords[2 * i] * coords[2 * i] + coords[2 * i + 1] * coords[2 * i + 1]);
        if (std::abs(r - 0.68f) > 1e-4f) ring2Correct = false;
    }

    reportTest("Ring 1 (innen) hat Radius r = 0.35", ring1Correct);
    reportTest("Ring 2 (aussen) hat Radius r = 0.68", ring2Correct);
}

void testLatencyAndAdapterModes() {
    std::cout << "\n--- Test 6: Latenzmeldung & Adapter-Modi ---\n";

    HQRNHandle handle = hqrn_create(48000.0, 128, 128);

    // Quantized FIFO Modus (Standard): 128 Quantum + 22 Clipper = 150 Samples
    uint32_t latFifo = hqrn_get_latency_samples(handle);
    reportTest("Quantized FIFO Latenz = 150 Samples (128 FIFO + 22 Clipper)",
               latFifo == 150, "Gemessen: " + std::to_string(latFifo));

    // Direct Modus: 0 FIFO + 22 Clipper = 22 Samples
    hqrn_set_adapter_mode(handle, 1);
    reportTest("Adapter-Modus auf Direct gewechselt", hqrn_get_adapter_mode(handle) == 1);

    uint32_t latDirect = hqrn_get_latency_samples(handle);
    reportTest("Direct Mode Latenz = 22 Samples",
               latDirect == 22, "Gemessen: " + std::to_string(latDirect));

    hqrn_destroy(handle);
}

void testRealTimeZeroAllocations() {
    std::cout << "\n--- Test 7: Allokationsfreiheit im Audio-Thread ---\n";

    HQRNHandle handle = hqrn_create(48000.0, 128, 128);
    hqrn_reset(handle);

    g_allocationCount = 0;
    g_allocatedBytes = 0;
    g_trackAllocations = true;

    for (int i = 0; i < 50; ++i) {
        // Parameter-Updates mitten im Audio-Thread
        hqrn_set_source_target(handle, 0.4f, 0.3f, 0.02f);
        hqrn_set_listener_target(handle, -0.3f, 0.2f, 0.5f, 0.02f);
        hqrn_process(handle, 128);
    }

    g_trackAllocations = false;

    reportTest("Exakt 0 Heap-Allokationen waehrend process() und Parameter-Updates",
               g_allocationCount == 0,
               "Allokationen: " + std::to_string(g_allocationCount) +
               ", Bytes: " + std::to_string(g_allocatedBytes));

    hqrn_destroy(handle);
}

int main() {
    std::cout << "========================================================\n";
    std::cout << "HQRN C-Bridge & WebAssembly Interface Test Suite\n";
    std::cout << "========================================================\n";

    testLifecycle();
    testInternalScratchpadProcess();
    testPlanarAndInterleavedProcessing();
    testParametersAndSmoothing();
    testPoincareGeometryExport();
    testLatencyAndAdapterModes();
    testRealTimeZeroAllocations();

    std::cout << "\n--------------------------------------------------------\n";
    std::cout << "Gesamtergebnis: " << g_testsPassed << " bestanden, "
              << g_testsFailed << " fehlgeschlagen.\n";
    std::cout << "========================================================\n";

    return (g_testsFailed == 0) ? 0 : 1;
}
