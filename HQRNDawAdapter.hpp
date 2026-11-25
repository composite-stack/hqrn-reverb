#pragma once

#include "HQRNKernels.hpp"
#include "HQRNReverbEngine.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace dsp::hqrn {


/**
 * Betriebsmodus des DAW-Puffer-Adapters.
 */
enum class DawAdapterMode {
    /**
     * Quantisierter Blockmodus (Fixed-Quantum / Uniform Block Mode).
     *
     * Puffert variable DAW-Host-Blöcke (N in [1, maxBlockSize]) und taktet die
     * interne Reverb-Engine und den 4x-FIR-Halbband-Oversampler in exakt konstanten,
     * SIMD-optimalen Chunks (z. B. Q = 64, 128, 256 oder 512 Frames).
     *
     * Eigenschaften:
     * - 100% Ausnutzung der SIMD-Vektor-Lanes (AVX2/NEON) ohne Scalar-Tail pro Host-Callback.
     * - Minimale Cache-Trashing-Effekte (L1-resident).
     * - Deterministische Ausführungszeit pro internem Block.
     * - Phasenstetige, bitgenaue Audio-Verarbeitung über beliebig fragmentierte Host-Puffer hinweg.
     * - Deterministische Latenz = Q + Clipper-Latenz (22 Samples bei aktivem True-Peak-Clipper).
     * - Durch Initialisierung (Priming) des Ausgangs-FIFOs mit Q Samples Stille
     *   garantierter kontinuierlicher Datenstrom ohne Pufferunterlauf.
     */
    QuantizedFifo,

    /**
     * Direkter Slicing-Modus (Zero-Additional-Latency Mode).
     *
     * Verarbeitet variable DAW-Blöcke direkt ohne zusätzlichen FIFO-Pipeline-Delay.
     * Große Blöcke (N > maxInternalChunk) werden in mundgerechte interne Chunks (<= 512)
     * zerlegt, um Pufferüberläufe in den internen Halbband-FIR-Scratchpads zu verhindern.
     *
     * Eigenschaften:
     * - 0 Samples zusätzliche Latenz (nur die physikalisch-kausale Gruppenlaufzeit
     *   des 4x-FIR-Halbbandfilters von 22 Samples).
     * - Ideal für Live-Tracking / Direktes Monitoring mit minimaler Latenz.
     * - Automatische Entkopplung über speicherausgerichtete Scratchpads (alignas(64))
     *   schützt vor ungerichteten Host-Puffern und In-Place-Aliasing.
     */
    Direct
};

[[nodiscard]] constexpr std::string_view getDawAdapterModeName(DawAdapterMode mode) noexcept {
    switch (mode) {
        case DawAdapterMode::QuantizedFifo: return "Quantized FIFO (Fixed-Block)";
        case DawAdapterMode::Direct:        return "Direct Slicing (Zero-Additional-Latency)";
        default:                            return "Unknown";
    }
}

/**
 * Cache-line-ausgerichteter, allokationsfreier Ringpuffer (FIFO) mit 2^k-Kapazität.
 * Garantiert branchless Masken-Wrapping und SIMD-vektorisierbare Block-Kopiervorgänge.
 */
template <typename T = float, size_t Alignment = 64>
class AlignedCircularBuffer {
public:
    AlignedCircularBuffer() = default;

    ~AlignedCircularBuffer() {
        freeBuffer();
    }

    AlignedCircularBuffer(const AlignedCircularBuffer&) = delete;
    AlignedCircularBuffer& operator=(const AlignedCircularBuffer&) = delete;

    AlignedCircularBuffer(AlignedCircularBuffer&& other) noexcept {
        moveFrom(std::move(other));
    }

    AlignedCircularBuffer& operator=(AlignedCircularBuffer&& other) noexcept {
        if (this != &other) {
            freeBuffer();
            moveFrom(std::move(other));
        }
        return *this;
    }

    /**
     * Allokiert den Ringpuffer auf eine 2^k-Kapazität >= minCapacity.
     * Muss im prepare()-Schritt (Non-Real-Time) aufgerufen werden.
     */
    void prepare(size_t minCapacity) {
        size_t cap = 256;
        while (cap < minCapacity) {
            cap <<= 1;
        }

        if (cap == capacity_ && buffer_ != nullptr) {
            clear();
            return;
        }

        freeBuffer();

        capacity_ = cap;
        mask_ = cap - 1;

#if defined(_MSC_VER)
        buffer_ = static_cast<T*>(_aligned_malloc(capacity_ * sizeof(T), Alignment));
#else
        void* ptr = nullptr;
        if (posix_memalign(&ptr, Alignment, capacity_ * sizeof(T)) == 0) {
            buffer_ = static_cast<T*>(ptr);
        } else {
            buffer_ = nullptr;
        }
#endif
        clear();
    }

    /**
     * Setzt Lese- und Schreibzeiger zurück und löscht den Pufferinhalt.
     */
    void clear() noexcept {
        readPos_ = 0;
        writePos_ = 0;
        if (buffer_ != nullptr && capacity_ > 0) {
            std::memset(buffer_, 0, capacity_ * sizeof(T));
        }
    }

    /**
     * Schreibt count Nullen in den Puffer und rückt den Schreibzeiger vor.
     * Dient zum Priming von Latenz (Pre-Roll für PDC).
     */
    void primeSilence(size_t count) noexcept {
        if (buffer_ == nullptr || count == 0) return;
        size_t w = static_cast<size_t>(writePos_ & mask_);
        size_t first = std::min(count, capacity_ - w);
        std::memset(buffer_ + w, 0, first * sizeof(T));
        if (count > first) {
            std::memset(buffer_, 0, (count - first) * sizeof(T));
        }
        writePos_ += count;
    }

    /**
     * Schreibt count Samples in den FIFO.
     */
    void push(const T* src, size_t count) noexcept {
        if (buffer_ == nullptr || count == 0) return;
        if (src == nullptr) {
            primeSilence(count);
            return;
        }
        size_t w = static_cast<size_t>(writePos_ & mask_);
        size_t first = std::min(count, capacity_ - w);
        std::memcpy(buffer_ + w, src, first * sizeof(T));
        if (count > first) {
            std::memcpy(buffer_, src + first, (count - first) * sizeof(T));
        }
        writePos_ += count;
    }

    /**
     * Liest count Samples aus dem FIFO und rückt den Lesezeiger vor.
     */
    void pop(T* dst, size_t count) noexcept {
        if (buffer_ == nullptr || count == 0) return;
        size_t r = static_cast<size_t>(readPos_ & mask_);
        size_t first = std::min(count, capacity_ - r);
        if (dst != nullptr) {
            std::memcpy(dst, buffer_ + r, first * sizeof(T));
            if (count > first) {
                std::memcpy(dst + first, buffer_, (count - first) * sizeof(T));
            }
        }
        readPos_ += count;
    }

    [[nodiscard]] inline size_t availableRead() const noexcept {
        return static_cast<size_t>(writePos_ - readPos_);
    }

    [[nodiscard]] inline size_t availableWrite() const noexcept {
        return capacity_ - availableRead();
    }

    [[nodiscard]] size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] const T* data() const noexcept { return buffer_; }
    [[nodiscard]] T* data() noexcept { return buffer_; }

private:
    void freeBuffer() noexcept {
        if (buffer_ != nullptr) {
#if defined(_MSC_VER)
            _aligned_free(buffer_);
#else
            free(buffer_);
#endif
            buffer_ = nullptr;
        }
        capacity_ = 0;
        mask_ = 0;
        readPos_ = 0;
        writePos_ = 0;
    }

    void moveFrom(AlignedCircularBuffer&& other) noexcept {
        buffer_ = other.buffer_;
        capacity_ = other.capacity_;
        mask_ = other.mask_;
        readPos_ = other.readPos_;
        writePos_ = other.writePos_;

        other.buffer_ = nullptr;
        other.capacity_ = 0;
        other.mask_ = 0;
        other.readPos_ = 0;
        other.writePos_ = 0;
    }

    T* buffer_{nullptr};
    size_t capacity_{0};
    size_t mask_{0};
    uint64_t readPos_{0};
    uint64_t writePos_{0};
};

/**
 * DAW-Puffer-Adapter für variable Host-Blockgrößen.
 *
 * Passt beliebige DAW-Blockgrößen (1 bis N_max, ungerade, nicht-Zweierpotenzen,
 * dynamisch variierend) zuverlässig an die internen FIR-Halbbandfilter und SIMD-Pipelines an.
 *
 * Garantiert:
 * 1. 100% Allokationsfreiheit im Audio-Thread (zero heap allocations in process()).
 * 2. In-Place-Sicherheit (input == output Pointer).
 * 3. 64-Byte Speicherausrichtung für ungerichtete Host-Puffer.
 * 4. Exakte Latenzmeldung (PDC) für hostseitige Latenzkompensation.
 * 5. Zwei Betriebsmodi: QuantizedFifo (maximale SIMD-Performance) und Direct (Zero-Additional-Latency).
 */
template <typename TEngine = HQRNReverbEngineAuto>
class HQRNDawAdapter {
public:
    static constexpr size_t MAX_INTERNAL_CHUNK = 512;
    static constexpr size_t DEFAULT_INTERNAL_QUANTUM = 128;
    static constexpr size_t DEFAULT_MAX_HOST_BLOCK = 8192;

    explicit HQRNDawAdapter(DawAdapterMode mode = DawAdapterMode::QuantizedFifo,
                            size_t internalQuantum = DEFAULT_INTERNAL_QUANTUM) noexcept
        : mode_(mode), internalQuantum_(clampQuantum(internalQuantum)) {}

    /**
     * Initialisiert den Adapter und allokiert alle benötigten Puffer vor.
     * Muss vor der ersten Audio-Verarbeitung aufgerufen werden (z. B. in prepareToPlay).
     *
     * @param sampleRate Abtastrate in Hz (z. B. 44100, 48000, 96000)
     * @param maxHostBlockSize Maximale vom Host erwartete Blockgröße (z. B. 1024, 2048, 8192)
     * @param internalQuantum Interne Blockquantisierung für QuantizedFifo (z. B. 64, 128, 256, 512)
     */
    void prepare(double sampleRate,
                 size_t maxHostBlockSize = DEFAULT_MAX_HOST_BLOCK,
                 size_t internalQuantum = DEFAULT_INTERNAL_QUANTUM) {
        sampleRate_ = static_cast<float>(sampleRate);
        maxHostBlockSize_ = std::max<size_t>(64, maxHostBlockSize);
        internalQuantum_ = clampQuantum(internalQuantum);

        // FIFO-Kapazität auf 2^k >= 2 * maxHostBlockSize + 4 * internalQuantum dimensionieren
        size_t requiredCap = 2 * maxHostBlockSize_ + 4 * internalQuantum_;
        inputFifoL_.prepare(requiredCap);
        inputFifoR_.prepare(requiredCap);
        outputFifoL_.prepare(requiredCap);
        outputFifoR_.prepare(requiredCap);

        engine_.prepare(sampleRate_);
        reset();
    }

    /**
     * Setzt alle internen Puffer, Zustände und FIFOs zurück.
     * Primit den Ausgangs-FIFO bei QuantizedFifo-Modus mit Latenz-Nullen.
     */
    void reset() noexcept {
        inputFifoL_.clear();
        inputFifoR_.clear();
        outputFifoL_.clear();
        outputFifoR_.clear();

        if (mode_ == DawAdapterMode::QuantizedFifo) {
            outputFifoL_.primeSilence(internalQuantum_);
            outputFifoR_.primeSilence(internalQuantum_);
        }

        engine_.reset();
    }

    /**
     * Umschalten des Adapter-Modus.
     * Führt ein sicheres Re-Priming der FIFOs durch.
     */
    void setMode(DawAdapterMode mode) noexcept {
        if (mode_ == mode) return;
        mode_ = mode;
        reset();
    }

    [[nodiscard]] DawAdapterMode getMode() const noexcept { return mode_; }

    /**
     * Ändern des internen Quantums (nur wirksam in QuantizedFifo).
     */
    void setInternalQuantum(size_t quantum) noexcept {
        size_t clamped = clampQuantum(quantum);
        if (internalQuantum_ == clamped) return;
        internalQuantum_ = clamped;
        reset();
    }

    [[nodiscard]] size_t getInternalQuantum() const noexcept { return internalQuantum_; }

    /**
     * Liefert die exakte Gesamtlatenz in Samples für die DAW-Plugin-Delay-Compensation (PDC).
     */
    [[nodiscard]] size_t getLatencySamples() const noexcept {
        size_t engineLatency = engine_.getLatencySamples();
        size_t fifoLatency = (mode_ == DawAdapterMode::QuantizedFifo) ? internalQuantum_ : 0;
        return fifoLatency + engineLatency;
    }

    [[nodiscard]] size_t getMaxHostBlockSize() const noexcept { return maxHostBlockSize_; }
    [[nodiscard]] size_t getFifoCapacity() const noexcept { return inputFifoL_.capacity(); }

    // ========================================================================
    // PROZESS-SCHNITTSTELLEN (100% allokationsfrei & noexcept)
    // ========================================================================

    /**
     * 1. Planare Multi-Kanal-Schnittstelle (Standard VST3 / CLAP / JUCE / AU).
     * Unterstützt Stereo-In/Stereo-Out, Mono-In/Stereo-Out sowie In-Place-Puffer.
     */
    void process(const float* const* inputs, float* const* outputs,
                 size_t numFrames, size_t numInChannels = 2, size_t numOutChannels = 2) noexcept {
        if (numFrames == 0 || outputs == nullptr || numOutChannels == 0) return;

        const float* inL = (numInChannels > 0 && inputs != nullptr && inputs[0] != nullptr) ? inputs[0] : nullptr;
        const float* inR = (numInChannels > 1 && inputs != nullptr && inputs[1] != nullptr) ? inputs[1] : inL;

        float* outL = outputs[0];
        float* outR = (numOutChannels > 1) ? outputs[1] : nullptr;

        if (outR != nullptr) {
            // Stereo Output
            processStereo(inL, inR, outL, outR, numFrames);
        } else {
            // Mono Output Fallback: Stereo durch Engine rechnen und 50/50 summieren
            constexpr size_t MONO_CHUNK = 512;
            alignas(64) float tmpL[MONO_CHUNK];
            alignas(64) float tmpR[MONO_CHUNK];

            for (size_t offset = 0; offset < numFrames; offset += MONO_CHUNK) {
                size_t n = std::min(MONO_CHUNK, numFrames - offset);
                const float* curInL = inL ? (inL + offset) : nullptr;
                const float* curInR = inR ? (inR + offset) : nullptr;

                processStereo(curInL, curInR, tmpL, tmpR, n);

                for (size_t i = 0; i < n; ++i) {
                    outL[offset + i] = 0.5f * (tmpL[i] + tmpR[i]);
                }
            }
        }
    }

    /**
     * 2. Moderne C++20 std::span Schnittstelle.
     */
    void process(std::span<const float> inL, std::span<const float> inR,
                 std::span<float> outL, std::span<float> outR) noexcept {
        size_t n = std::min({inL.size(), inR.size(), outL.size(), outR.size()});
        if (n == 0) return;
        processStereo(inL.data(), inR.data(), outL.data(), outR.data(), n);
    }

    /**
     * 3. Interleaved Stereo Schnittstelle (z. B. für ASIO, ALSA oder Audio-Treiber).
     */
    void processInterleaved(const float* inInterleaved, float* outInterleaved,
                            size_t numFrames) noexcept {
        if (numFrames == 0 || outInterleaved == nullptr) return;

        constexpr size_t CHUNK_FRAMES = 256;
        alignas(64) float deinterleavedL[CHUNK_FRAMES];
        alignas(64) float deinterleavedR[CHUNK_FRAMES];
        alignas(64) float processedL[CHUNK_FRAMES];
        alignas(64) float processedR[CHUNK_FRAMES];

        for (size_t offset = 0; offset < numFrames; offset += CHUNK_FRAMES) {
            size_t n = std::min(CHUNK_FRAMES, numFrames - offset);

            if (inInterleaved != nullptr) {
                const float* src = inInterleaved + 2 * offset;
                for (size_t i = 0; i < n; ++i) {
                    deinterleavedL[i] = src[2 * i];
                    deinterleavedR[i] = src[2 * i + 1];
                }
                processStereo(deinterleavedL, deinterleavedR, processedL, processedR, n);
            } else {
                processStereo(nullptr, nullptr, processedL, processedR, n);
            }

            float* dst = outInterleaved + 2 * offset;
            for (size_t i = 0; i < n; ++i) {
                dst[2 * i]     = processedL[i];
                dst[2 * i + 1] = processedR[i];
            }
        }
    }

    // ========================================================================
    // ENGINE-PARAMETER-FORWARDING
    // ========================================================================

    TEngine& engine() noexcept { return engine_; }
    const TEngine& engine() const noexcept { return engine_; }

    void setBaseFrequency(float fBaseHz) noexcept { engine_.setBaseFrequency(fBaseHz); }
    void setReverbTime(float t60Sec) noexcept { engine_.setReverbTime(t60Sec); }
    void setSourcePosition(float x, float y) noexcept { engine_.setSourcePosition(x, y); }
    void setListenerPosition(float x, float y, float headingRad = 0.0f) noexcept {
        engine_.setListenerPosition(x, y, headingRad);
    }
    void setMix(float wetRatio) noexcept { engine_.setMix(wetRatio); }
    void setOutputGainDb(float gainDb) noexcept { engine_.setOutputGainDb(gainDb); }
    void setClipperParams(float ceilingDb, float kneeDb = 3.0f, bool enable = true) noexcept {
        engine_.setClipperParams(ceilingDb, kneeDb, enable);
    }
    void setSimdPath(CpuSimdPath path) noexcept { engine_.setSimdPath(path); }

private:
    [[nodiscard]] static constexpr size_t clampQuantum(size_t q) noexcept {
        // Quantum muss mindestens 16 und maximal 512 sein, gerundet auf Vielfaches von 16
        size_t bounded = std::clamp<size_t>(q, 16, MAX_INTERNAL_CHUNK);
        return (bounded / 16) * 16;
    }

    void processStereo(const float* inL, const float* inR,
                       float* outL, float* outR,
                       size_t numFrames) noexcept {
        if (mode_ == DawAdapterMode::QuantizedFifo) {
            // Slicing für Blöcke größer als maxHostBlockSize_
            size_t remaining = numFrames;
            size_t offset = 0;
            while (remaining > 0) {
                size_t chunk = std::min(remaining, maxHostBlockSize_);
                const float* curInL = inL ? (inL + offset) : nullptr;
                const float* curInR = inR ? (inR + offset) : nullptr;
                float* curOutL = outL ? (outL + offset) : nullptr;
                float* curOutR = outR ? (outR + offset) : nullptr;

                processQuantizedSlice(curInL, curInR, curOutL, curOutR, chunk);

                offset += chunk;
                remaining -= chunk;
            }
        } else {
            processDirect(inL, inR, outL, outR, numFrames);
        }
    }

    void processQuantizedSlice(const float* inL, const float* inR,
                               float* outL, float* outR,
                               size_t numFrames) noexcept {
        // 1. Eingangsdaten in Eingangs-FIFO pushen (nullptr erzeugt Stille)
        inputFifoL_.push(inL, numFrames);
        inputFifoR_.push(inR, numFrames);

        // 2. Solange mindestens Q Frames im FIFO sind: Exakt Q Frames durch die Engine takten
        alignas(64) float chunkInL[MAX_INTERNAL_CHUNK];
        alignas(64) float chunkInR[MAX_INTERNAL_CHUNK];
        alignas(64) float chunkOutL[MAX_INTERNAL_CHUNK];
        alignas(64) float chunkOutR[MAX_INTERNAL_CHUNK];

        while (inputFifoL_.availableRead() >= internalQuantum_) {
            inputFifoL_.pop(chunkInL, internalQuantum_);
            inputFifoR_.pop(chunkInR, internalQuantum_);

            engine_.process(std::span<const float>(chunkInL, internalQuantum_),
                            std::span<const float>(chunkInR, internalQuantum_),
                            std::span<float>(chunkOutL, internalQuantum_),
                            std::span<float>(chunkOutR, internalQuantum_));

            outputFifoL_.push(chunkOutL, internalQuantum_);
            outputFifoR_.push(chunkOutR, internalQuantum_);
        }

        // 3. Exakt numFrames an Ausgangsdaten aus Ausgangs-FIFO an den Host liefern
        outputFifoL_.pop(outL, numFrames);
        outputFifoR_.pop(outR, numFrames);
    }

    void processDirect(const float* inL, const float* inR,
                       float* outL, float* outR,
                       size_t numFrames) noexcept {
        constexpr size_t DIRECT_CHUNK = 512;
        alignas(64) float chunkInL[DIRECT_CHUNK];
        alignas(64) float chunkInR[DIRECT_CHUNK];
        alignas(64) float chunkOutL[DIRECT_CHUNK];
        alignas(64) float chunkOutR[DIRECT_CHUNK];

        for (size_t offset = 0; offset < numFrames; offset += DIRECT_CHUNK) {
            size_t n = std::min(DIRECT_CHUNK, numFrames - offset);

            if (inL != nullptr) {
                std::memcpy(chunkInL, inL + offset, n * sizeof(float));
            } else {
                std::memset(chunkInL, 0, n * sizeof(float));
            }

            if (inR != nullptr) {
                std::memcpy(chunkInR, inR + offset, n * sizeof(float));
            } else {
                std::memset(chunkInR, 0, n * sizeof(float));
            }

            engine_.process(std::span<const float>(chunkInL, n),
                            std::span<const float>(chunkInR, n),
                            std::span<float>(chunkOutL, n),
                            std::span<float>(chunkOutR, n));

            if (outL != nullptr) {
                std::memcpy(outL + offset, chunkOutL, n * sizeof(float));
            }
            if (outR != nullptr) {
                std::memcpy(outR + offset, chunkOutR, n * sizeof(float));
            }
        }
    }

    DawAdapterMode mode_{DawAdapterMode::QuantizedFifo};
    size_t internalQuantum_{DEFAULT_INTERNAL_QUANTUM};
    size_t maxHostBlockSize_{DEFAULT_MAX_HOST_BLOCK};
    float sampleRate_{48000.0f};

    AlignedCircularBuffer<float, 64> inputFifoL_;
    AlignedCircularBuffer<float, 64> inputFifoR_;
    AlignedCircularBuffer<float, 64> outputFifoL_;
    AlignedCircularBuffer<float, 64> outputFifoR_;

    TEngine engine_;
};

using HQRNDawAdapterAuto   = HQRNDawAdapter<HQRNReverbEngineAuto>;
using HQRNDawAdapterAVX2   = HQRNDawAdapter<HQRNReverbEngineAVX2>;
using HQRNDawAdapterNEON   = HQRNDawAdapter<HQRNReverbEngineNEON>;
using HQRNDawAdapterScalar = HQRNDawAdapter<HQRNReverbEngineScalar>;

} // namespace dsp::hqrn

namespace hqrn = dsp::hqrn;

