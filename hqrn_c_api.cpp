/**
 * @file hqrn_c_api.cpp
 * @brief Implementierung der C-Bridge und Wasm-Exporte für die HQRN Reverb Engine.
 */

#include "hqrn_c_api.h"
#include "HQRNDawAdapter.hpp"
#include "HQRNReverbEngine.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>

namespace {

/**
 * Ein-Pol-Glättung (Low-pass Slewing) für blockweise Parameter-Updates.
 */
struct SmoothedParam {
    float current{0.0f};
    float target{0.0f};
    float lambda{1.0f}; // 1.0 = sofortiger Sprung (kein Glätten)

    void setImmediate(float val) noexcept {
        current = val;
        target = val;
        lambda = 1.0f;
    }

    void setTarget(float tgt, float smoothTimeSec, float sampleRate, uint32_t blockSize) noexcept {
        target = tgt;
        if (smoothTimeSec <= 1e-4f || sampleRate <= 0.0f) {
            lambda = 1.0f;
        } else {
            // Blockbasiertes Zeitintervall: dt = blockSize / sampleRate
            float dt = static_cast<float>(blockSize) / sampleRate;
            // Zeitkonstante tau: ca. 3 * tau entspricht 95% Einschwingzeit
            float tau = std::max(1e-4f, smoothTimeSec / 3.0f);
            lambda = 1.0f - std::exp(-dt / tau);
            lambda = std::clamp(lambda, 0.0001f, 1.0f);
        }
    }

    bool step() noexcept {
        if (std::abs(target - current) < 1e-6f) {
            current = target;
            return false;
        }
        current += lambda * (target - current);
        return true;
    }
};

/**
 * Interner Kontext für eine HQRN-Instanz.
 */
struct HQRNContext {
    dsp::hqrn::HQRNDawAdapter<dsp::hqrn::HQRNReverbEngine> adapter;
    float sampleRate{48000.0f};
    uint32_t maxBlockSize{128};
    uint32_t internalQuantum{128};

    // Pre-allocated Zero-Allocation Scratchpads für Wasm/JS I/O
    std::vector<float> inputL;
    std::vector<float> inputR;
    std::vector<float> outputL;
    std::vector<float> outputR;

    // Smoothed Parameters
    SmoothedParam sourceX{0.0f, 0.0f, 1.0f};
    SmoothedParam sourceY{0.0f, 0.0f, 1.0f};
    SmoothedParam listenerX{0.0f, 0.0f, 1.0f};
    SmoothedParam listenerY{0.0f, 0.0f, 1.0f};
    SmoothedParam listenerHeading{0.0f, 0.0f, 1.0f};
    SmoothedParam reverbTime{2.0f, 2.0f, 1.0f};
    SmoothedParam baseFreq{100.0f, 100.0f, 1.0f};
    SmoothedParam mix{0.35f, 0.35f, 1.0f};

    HQRNContext(double sr, uint32_t maxBlock, uint32_t quantum)
        : adapter(dsp::hqrn::DawAdapterMode::QuantizedFifo, quantum),
          sampleRate(static_cast<float>(sr)),
          maxBlockSize(std::max(16u, maxBlock)),
          internalQuantum(quantum) {
        allocateBuffers(maxBlockSize);
        adapter.prepare(sr, maxBlockSize, internalQuantum);
    }

    void allocateBuffers(uint32_t capacity) {
        maxBlockSize = std::max(16u, capacity);
        inputL.assign(maxBlockSize, 0.0f);
        inputR.assign(maxBlockSize, 0.0f);
        outputL.assign(maxBlockSize, 0.0f);
        outputR.assign(maxBlockSize, 0.0f);
    }

    void prepare(double sr, uint32_t maxBlock, uint32_t quantum) {
        sampleRate = static_cast<float>(sr);
        internalQuantum = quantum;
        allocateBuffers(maxBlock);
        adapter.prepare(sr, maxBlockSize, internalQuantum);
    }

    void updateSmoothing() noexcept {
        bool dirtySource = false;
        if (sourceX.step()) dirtySource = true;
        if (sourceY.step()) dirtySource = true;
        if (dirtySource) {
            adapter.setSourcePosition(sourceX.current, sourceY.current);
        }

        bool dirtyListener = false;
        if (listenerX.step()) dirtyListener = true;
        if (listenerY.step()) dirtyListener = true;
        if (listenerHeading.step()) dirtyListener = true;
        if (dirtyListener) {
            adapter.setListenerPosition(listenerX.current, listenerY.current, listenerHeading.current);
        }

        if (reverbTime.step()) {
            adapter.setReverbTime(reverbTime.current);
        }
        if (baseFreq.step()) {
            adapter.setBaseFrequency(baseFreq.current);
        }
        if (mix.step()) {
            adapter.setMix(mix.current);
        }
    }
};

inline HQRNContext* asContext(HQRNHandle handle) noexcept {
    return static_cast<HQRNContext*>(handle);
}

} // anonymous namespace

/* ============================================================================
 * C EXPORT IMPLEMENTIERUNG
 * ============================================================================ */

extern "C" {

HQRN_EXPORT HQRNHandle hqrn_create(double sampleRate, uint32_t maxBlockSize, uint32_t internalQuantum) {
    try {
        return new (std::nothrow) HQRNContext(sampleRate, maxBlockSize, internalQuantum);
    } catch (...) {
        return nullptr;
    }
}

HQRN_EXPORT void hqrn_destroy(HQRNHandle handle) {
    if (handle != nullptr) {
        delete asContext(handle);
    }
}

HQRN_EXPORT void hqrn_prepare(HQRNHandle handle, double sampleRate, uint32_t maxBlockSize, uint32_t internalQuantum) {
    if (auto* ctx = asContext(handle)) {
        ctx->prepare(sampleRate, maxBlockSize, internalQuantum);
    }
}

HQRN_EXPORT void hqrn_reset(HQRNHandle handle) {
    if (auto* ctx = asContext(handle)) {
        ctx->adapter.reset();
    }
}

HQRN_EXPORT void hqrn_set_adapter_mode(HQRNHandle handle, int32_t mode) {
    if (auto* ctx = asContext(handle)) {
        auto dawMode = (mode == 1) ? dsp::hqrn::DawAdapterMode::Direct
                                   : dsp::hqrn::DawAdapterMode::QuantizedFifo;
        ctx->adapter.setMode(dawMode);
    }
}

HQRN_EXPORT int32_t hqrn_get_adapter_mode(HQRNHandle handle) {
    if (const auto* ctx = asContext(handle)) {
        return (ctx->adapter.getMode() == dsp::hqrn::DawAdapterMode::Direct) ? 1 : 0;
    }
    return 0;
}

HQRN_EXPORT float* hqrn_get_input_buffer_l(HQRNHandle handle) {
    if (auto* ctx = asContext(handle)) {
        return ctx->inputL.data();
    }
    return nullptr;
}

HQRN_EXPORT float* hqrn_get_input_buffer_r(HQRNHandle handle) {
    if (auto* ctx = asContext(handle)) {
        return ctx->inputR.data();
    }
    return nullptr;
}

HQRN_EXPORT float* hqrn_get_output_buffer_l(HQRNHandle handle) {
    if (auto* ctx = asContext(handle)) {
        return ctx->outputL.data();
    }
    return nullptr;
}

HQRN_EXPORT float* hqrn_get_output_buffer_r(HQRNHandle handle) {
    if (auto* ctx = asContext(handle)) {
        return ctx->outputR.data();
    }
    return nullptr;
}

HQRN_EXPORT uint32_t hqrn_get_buffer_capacity(HQRNHandle handle) {
    if (const auto* ctx = asContext(handle)) {
        return ctx->maxBlockSize;
    }
    return 0;
}

HQRN_EXPORT void hqrn_process(HQRNHandle handle, uint32_t numFrames) {
    if (auto* ctx = asContext(handle)) {
        if (numFrames == 0) return;
        uint32_t n = std::min(numFrames, ctx->maxBlockSize);
        ctx->updateSmoothing();
        const float* inPtrs[2] = { ctx->inputL.data(), ctx->inputR.data() }; float* outPtrs[2] = { ctx->outputL.data(), ctx->outputR.data() }; ctx->adapter.process(inPtrs, outPtrs, n, 2, 2);
    }
}

HQRN_EXPORT void hqrn_process_planar(HQRNHandle handle,
                                     const float* inL, const float* inR,
                                     float* outL, float* outR,
                                     uint32_t numFrames) {
    if (auto* ctx = asContext(handle)) {
        if (numFrames == 0) return;
        ctx->updateSmoothing();
        const float* inPtrs[2] = { inL, inR };
        float* outPtrs[2] = { outL, outR };
        ctx->adapter.process(inPtrs, outPtrs, numFrames, 2, 2);
    }
}

HQRN_EXPORT void hqrn_process_interleaved(HQRNHandle handle,
                                         const float* inInterleaved,
                                         float* outInterleaved,
                                         uint32_t numFrames) {
    if (auto* ctx = asContext(handle)) {
        if (numFrames == 0) return;
        ctx->updateSmoothing();
        ctx->adapter.processInterleaved(inInterleaved, outInterleaved, numFrames);
    }
}

HQRN_EXPORT void hqrn_set_base_frequency(HQRNHandle handle, float fBaseHz) {
    if (auto* ctx = asContext(handle)) {
        ctx->baseFreq.setImmediate(fBaseHz);
        ctx->adapter.setBaseFrequency(fBaseHz);
    }
}

HQRN_EXPORT void hqrn_set_reverb_time(HQRNHandle handle, float t60Sec) {
    if (auto* ctx = asContext(handle)) {
        ctx->reverbTime.setImmediate(t60Sec);
        ctx->adapter.setReverbTime(t60Sec);
    }
}

HQRN_EXPORT void hqrn_set_source_position(HQRNHandle handle, float x, float y) {
    if (auto* ctx = asContext(handle)) {
        ctx->sourceX.setImmediate(x);
        ctx->sourceY.setImmediate(y);
        ctx->adapter.setSourcePosition(x, y);
    }
}

HQRN_EXPORT void hqrn_set_listener_position(HQRNHandle handle, float x, float y, float headingRad) {
    if (auto* ctx = asContext(handle)) {
        ctx->listenerX.setImmediate(x);
        ctx->listenerY.setImmediate(y);
        ctx->listenerHeading.setImmediate(headingRad);
        ctx->adapter.setListenerPosition(x, y, headingRad);
    }
}

HQRN_EXPORT void hqrn_set_mix(HQRNHandle handle, float wetRatio) {
    if (auto* ctx = asContext(handle)) {
        ctx->mix.setImmediate(wetRatio);
        ctx->adapter.setMix(wetRatio);
    }
}

HQRN_EXPORT void hqrn_set_output_gain_db(HQRNHandle handle, float gainDb) {
    if (auto* ctx = asContext(handle)) {
        ctx->adapter.setOutputGainDb(gainDb);
    }
}

HQRN_EXPORT void hqrn_set_clipper_params(HQRNHandle handle, float ceilingDb, float kneeDb, int32_t enable) {
    if (auto* ctx = asContext(handle)) {
        ctx->adapter.setClipperParams(ceilingDb, kneeDb, enable != 0);
    }
}

HQRN_EXPORT void hqrn_set_source_target(HQRNHandle handle, float targetX, float targetY, float smoothTimeSec) {
    if (auto* ctx = asContext(handle)) {
        ctx->sourceX.setTarget(targetX, smoothTimeSec, ctx->sampleRate, ctx->maxBlockSize);
        ctx->sourceY.setTarget(targetY, smoothTimeSec, ctx->sampleRate, ctx->maxBlockSize);
    }
}

HQRN_EXPORT void hqrn_set_listener_target(HQRNHandle handle, float targetX, float targetY, float targetHeadingRad, float smoothTimeSec) {
    if (auto* ctx = asContext(handle)) {
        ctx->listenerX.setTarget(targetX, smoothTimeSec, ctx->sampleRate, ctx->maxBlockSize);
        ctx->listenerY.setTarget(targetY, smoothTimeSec, ctx->sampleRate, ctx->maxBlockSize);
        ctx->listenerHeading.setTarget(targetHeadingRad, smoothTimeSec, ctx->sampleRate, ctx->maxBlockSize);
    }
}

HQRN_EXPORT void hqrn_set_reverb_time_target(HQRNHandle handle, float targetT60Sec, float smoothTimeSec) {
    if (auto* ctx = asContext(handle)) {
        ctx->reverbTime.setTarget(targetT60Sec, smoothTimeSec, ctx->sampleRate, ctx->maxBlockSize);
    }
}

HQRN_EXPORT void hqrn_set_base_frequency_target(HQRNHandle handle, float targetFBaseHz, float smoothTimeSec) {
    if (auto* ctx = asContext(handle)) {
        ctx->baseFreq.setTarget(targetFBaseHz, smoothTimeSec, ctx->sampleRate, ctx->maxBlockSize);
    }
}

HQRN_EXPORT void hqrn_set_mix_target(HQRNHandle handle, float targetWetRatio, float smoothTimeSec) {
    if (auto* ctx = asContext(handle)) {
        ctx->mix.setTarget(targetWetRatio, smoothTimeSec, ctx->sampleRate, ctx->maxBlockSize);
    }
}

HQRN_EXPORT uint32_t hqrn_get_latency_samples(HQRNHandle handle) {
    if (const auto* ctx = asContext(handle)) {
        return static_cast<uint32_t>(ctx->adapter.getLatencySamples());
    }
    return 0;
}

HQRN_EXPORT float hqrn_get_sample_rate(HQRNHandle handle) {
    if (const auto* ctx = asContext(handle)) {
        return ctx->sampleRate;
    }
    return 0.0f;
}

HQRN_EXPORT void hqrn_get_node_coordinates(float* outCoordsXY) {
    if (outCoordsXY == nullptr) return;
    auto nodes = dsp::hqrn::createNodeLayout();
    for (size_t i = 0; i < 16; ++i) {
        outCoordsXY[2 * i]     = nodes[i].x;
        outCoordsXY[2 * i + 1] = nodes[i].y;
    }
}

HQRN_EXPORT uint32_t hqrn_get_node_count(void) {
    return 16;
}

} // extern "C"
