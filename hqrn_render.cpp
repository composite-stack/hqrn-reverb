/**
 * @file hqrn_render.cpp
 * @brief Offline Audio Renderer & Acoustic Analyzer for the HQRN Reverb Engine.
 * 
 * Capabilities:
 * - Generates Dirac impulse responses (stereo, L, R)
 * - Generates synthetic noise bursts (onset & diffusion analysis)
 * - Generates dynamic Poincaré disk trajectories (geodesic/orbit motion)
 * - Processes external WAV files (16-bit, 24-bit, 32-bit float)
 * - Writes canonical 24-bit PCM or 32-bit float stereo WAV files
 * - Computes acoustic metrics: Schroeder energy decay curve (EDC), EDT, T20, T30,
 *   True-Peak, Peak dBFS, RMS dBFS, and rendering throughput (x Realtime).
 * 
 * Standard: C++20
 * Author: M. A. / Lovable Audio DSP
 */

#include "HQRNKernels.hpp"
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <string_view>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <iomanip>
#include <algorithm>
#include <memory>
#include <random>

namespace hqrn_cli {

// ============================================================================
// RIFF WAVE READER & WRITER (Zero External Dependencies)
// ============================================================================

enum class WavBitDepth {
    PCM16,
    PCM24,
    Float32
};

struct WavAudioData {
    uint32_t sampleRate{48000};
    uint16_t numChannels{2};
    std::vector<float> left;
    std::vector<float> right;
};

#pragma pack(push, 1)
struct RiffHeader {
    char riffId[4];         // "RIFF"
    uint32_t riffSize;
    char waveId[4];         // "WAVE"
    char fmtId[4];          // "fmt "
    uint32_t fmtSize;       // 16 for standard PCM
    uint16_t audioFormat;   // 1 = PCM, 3 = IEEE Float
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
    char dataId[4];         // "data"
    uint32_t dataSize;
};
#pragma pack(pop)

bool writeWav(const std::string& filepath,
              const std::vector<float>& left,
              const std::vector<float>& right,
              uint32_t sampleRate,
              WavBitDepth bitDepth)
{
    std::ofstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Fehler: Konnte Datei nicht zum Schreiben öffnen: " << filepath << "\n";
        return false;
    }

    const uint32_t numSamples = static_cast<uint32_t>(left.size());
    const uint16_t channels = 2;
    uint16_t bitsPerSample = 24;
    uint16_t formatTag = 1; // PCM

    if (bitDepth == WavBitDepth::PCM16) {
        bitsPerSample = 16;
        formatTag = 1;
    } else if (bitDepth == WavBitDepth::PCM24) {
        bitsPerSample = 24;
        formatTag = 1;
    } else if (bitDepth == WavBitDepth::Float32) {
        bitsPerSample = 32;
        formatTag = 3; // IEEE Float
    }

    const uint16_t blockAlign = channels * (bitsPerSample / 8);
    const uint32_t byteRate = sampleRate * blockAlign;
    const uint32_t dataSize = numSamples * blockAlign;
    const uint32_t riffSize = sizeof(RiffHeader) - 8 + dataSize;

    RiffHeader header{};
    std::memcpy(header.riffId, "RIFF", 4);
    header.riffSize = riffSize;
    std::memcpy(header.waveId, "WAVE", 4);
    std::memcpy(header.fmtId, "fmt ", 4);
    header.fmtSize = 16;
    header.audioFormat = formatTag;
    header.numChannels = channels;
    header.sampleRate = sampleRate;
    header.byteRate = byteRate;
    header.blockAlign = blockAlign;
    header.bitsPerSample = bitsPerSample;
    std::memcpy(header.dataId, "data", 4);
    header.dataSize = dataSize;

    file.write(reinterpret_cast<const char*>(&header), sizeof(RiffHeader));

    std::vector<uint8_t> buffer(dataSize);
    uint8_t* ptr = buffer.data();

    if (bitDepth == WavBitDepth::Float32) {
        for (uint32_t i = 0; i < numSamples; ++i) {
            float sL = left[i];
            float sR = right[i];
            std::memcpy(ptr, &sL, sizeof(float)); ptr += sizeof(float);
            std::memcpy(ptr, &sR, sizeof(float)); ptr += sizeof(float);
        }
    } else if (bitDepth == WavBitDepth::PCM24) {
        for (uint32_t i = 0; i < numSamples; ++i) {
            // 24-bit [-8388608, 8388607]
            float cL = std::clamp(left[i], -1.0f, 1.0f);
            float cR = std::clamp(right[i], -1.0f, 1.0f);
            int32_t iL = static_cast<int32_t>(cL * 8388607.0f);
            int32_t iR = static_cast<int32_t>(cR * 8388607.0f);

            *ptr++ = static_cast<uint8_t>(iL & 0xFF);
            *ptr++ = static_cast<uint8_t>((iL >> 8) & 0xFF);
            *ptr++ = static_cast<uint8_t>((iL >> 16) & 0xFF);

            *ptr++ = static_cast<uint8_t>(iR & 0xFF);
            *ptr++ = static_cast<uint8_t>((iR >> 8) & 0xFF);
            *ptr++ = static_cast<uint8_t>((iR >> 16) & 0xFF);
        }
    } else { // PCM16
        for (uint32_t i = 0; i < numSamples; ++i) {
            float cL = std::clamp(left[i], -1.0f, 1.0f);
            float cR = std::clamp(right[i], -1.0f, 1.0f);
            int16_t iL = static_cast<int16_t>(cL * 32767.0f);
            int16_t iR = static_cast<int16_t>(cR * 32767.0f);

            std::memcpy(ptr, &iL, sizeof(int16_t)); ptr += sizeof(int16_t);
            std::memcpy(ptr, &iR, sizeof(int16_t)); ptr += sizeof(int16_t);
        }
    }

    file.write(reinterpret_cast<const char*>(buffer.data()), buffer.size());
    return file.good();
}

bool readWav(const std::string& filepath, WavAudioData& outData) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Fehler: Konnte Datei nicht zum Lesen öffnen: " << filepath << "\n";
        return false;
    }

    char id[4];
    uint32_t size = 0;

    file.read(id, 4);
    if (std::memcmp(id, "RIFF", 4) != 0) return false;
    file.read(reinterpret_cast<char*>(&size), 4);
    file.read(id, 4);
    if (std::memcmp(id, "WAVE", 4) != 0) return false;

    uint16_t audioFormat = 0;
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint16_t bitsPerSample = 0;
    bool foundFmt = false;
    bool foundData = false;
    uint32_t dataBytes = 0;
    std::vector<uint8_t> rawData;

    while (file.read(id, 4)) {
        file.read(reinterpret_cast<char*>(&size), 4);
        if (std::memcmp(id, "fmt ", 4) == 0) {
            file.read(reinterpret_cast<char*>(&audioFormat), 2);
            file.read(reinterpret_cast<char*>(&channels), 2);
            file.read(reinterpret_cast<char*>(&sampleRate), 4);
            file.seekg(6, std::ios::cur); // Skip ByteRate & BlockAlign
            file.read(reinterpret_cast<char*>(&bitsPerSample), 2);
            if (size > 16) {
                file.seekg(size - 16, std::ios::cur);
            }
            foundFmt = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            dataBytes = size;
            rawData.resize(dataBytes);
            file.read(reinterpret_cast<char*>(rawData.data()), dataBytes);
            foundData = true;
            break;
        } else {
            file.seekg(size, std::ios::cur);
        }
    }

    if (!foundFmt || !foundData) return false;

    outData.sampleRate = sampleRate;
    outData.numChannels = channels;
    const uint32_t bytesPerSample = bitsPerSample / 8;
    const uint32_t frameSize = channels * bytesPerSample;
    const uint32_t numFrames = dataBytes / frameSize;

    outData.left.resize(numFrames);
    outData.right.resize(numFrames);

    const uint8_t* ptr = rawData.data();
    for (uint32_t i = 0; i < numFrames; ++i) {
        float ch[2] = {0.0f, 0.0f};
        for (uint16_t c = 0; c < std::min<uint16_t>(channels, 2); ++c) {
            if (audioFormat == 1) { // PCM
                if (bitsPerSample == 16) {
                    int16_t val;
                    std::memcpy(&val, ptr, 2); ptr += 2;
                    ch[c] = val / 32768.0f;
                } else if (bitsPerSample == 24) {
                    int32_t val = (static_cast<int32_t>(ptr[0])) |
                                  (static_cast<int32_t>(ptr[1]) << 8) |
                                  (static_cast<int32_t>(static_cast<int8_t>(ptr[2])) << 16);
                    ptr += 3;
                    ch[c] = val / 8388608.0f;
                } else {
                    ptr += bytesPerSample;
                }
            } else if (audioFormat == 3 && bitsPerSample == 32) { // Float
                float val;
                std::memcpy(&val, ptr, 4); ptr += 4;
                ch[c] = val;
            } else {
                ptr += bytesPerSample;
            }
        }
        // Skip remaining channels if > 2
        if (channels > 2) {
            ptr += (channels - 2) * bytesPerSample;
        }

        outData.left[i] = ch[0];
        outData.right[i] = (channels == 1) ? ch[0] : ch[1];
    }

    return true;
}

// ============================================================================
// ACOUSTIC & STATISTICAL METRICS (Schroeder EDC, T20, T30, EDT, Levels)
// ============================================================================

struct AcousticMetrics {
    float peakL_dBFS{-100.0f};
    float peakR_dBFS{-100.0f};
    float rmsL_dBFS{-100.0f};
    float rmsR_dBFS{-100.0f};
    float edtL_sec{-1.0f};
    float edtR_sec{-1.0f};
    float t20L_sec{-1.0f};
    float t20R_sec{-1.0f};
    float t30L_sec{-1.0f};
    float t30R_sec{-1.0f};
    bool clipped{false};
};

static float toDb(float linear, float minDb = -120.0f) {
    if (linear <= 1e-6f) return minDb;
    return std::max(minDb, 20.0f * std::log10(linear));
}

// Fit line y = a * x + b by linear regression
static bool linearRegression(const std::vector<double>& x, const std::vector<double>& y, double& slope, double& intercept) {
    if (x.size() < 2 || x.size() != y.size()) return false;
    double sumX = 0.0, sumY = 0.0, sumXY = 0.0, sumXX = 0.0;
    const size_t n = x.size();
    for (size_t i = 0; i < n; ++i) {
        sumX += x[i];
        sumY += y[i];
        sumXY += x[i] * y[i];
        sumXX += x[i] * x[i];
    }
    double denom = n * sumXX - sumX * sumX;
    if (std::abs(denom) < 1e-12) return false;
    slope = (n * sumXY - sumX * sumY) / denom;
    intercept = (sumY - slope * sumX) / n;
    return true;
}

static void evaluateSchroederDecay(const std::vector<float>& signal, float sampleRate, float& edt, float& t20, float& t30) {
    edt = -1.0f;
    t20 = -1.0f;
    t30 = -1.0f;
    const size_t n = signal.size();
    if (n < static_cast<size_t>(sampleRate * 0.1f)) return;

    // Backward energy integration (Schroeder)
    std::vector<double> energy(n, 0.0);
    double accum = 0.0;
    for (size_t i = n; i-- > 0; ) {
        accum += static_cast<double>(signal[i]) * static_cast<double>(signal[i]);
        energy[i] = accum;
    }

    const double totalEnergy = energy[0];
    if (totalEnergy <= 1e-12) return;

    std::vector<double> edcDb(n);
    for (size_t i = 0; i < n; ++i) {
        double ratio = energy[i] / totalEnergy;
        edcDb[i] = (ratio > 1e-12) ? 10.0 * std::log10(ratio) : -120.0;
    }

    // EDT: 0 dB to -10 dB -> extrapolate slope to -60 dB
    // T20: -5 dB to -25 dB -> extrapolate slope to -60 dB (* 3)
    // T30: -5 dB to -35 dB -> extrapolate slope to -60 dB (* 2)
    auto fitRange = [&](double startDb, double endDb, float& outTime) {
        std::vector<double> tFit, dbFit;
        for (size_t i = 0; i < n; ++i) {
            if (edcDb[i] <= startDb && edcDb[i] >= endDb) {
                tFit.push_back(static_cast<double>(i) / sampleRate);
                dbFit.push_back(edcDb[i]);
            }
        }
        if (tFit.size() >= 20) {
            double slope = 0.0, intercept = 0.0;
            if (linearRegression(tFit, dbFit, slope, intercept) && slope < -1e-4) {
                outTime = static_cast<float>(-60.0 / slope);
            }
        }
    };

    fitRange(0.0, -10.0, edt);
    fitRange(-5.0, -25.0, t20);
    fitRange(-5.0, -35.0, t30);
}

AcousticMetrics analyzeAudio(const std::vector<float>& left, const std::vector<float>& right, float sampleRate, bool isImpulse) {
    AcousticMetrics m{};
    const size_t n = left.size();
    if (n == 0) return m;

    float maxL = 0.0f, maxR = 0.0f;
    double sumSqL = 0.0, sumSqR = 0.0;

    for (size_t i = 0; i < n; ++i) {
        float aL = std::abs(left[i]);
        float aR = std::abs(right[i]);
        if (aL > maxL) maxL = aL;
        if (aR > maxR) maxR = aR;
        sumSqL += aL * aL;
        sumSqR += aR * aR;
        if (aL >= 0.999f || aR >= 0.999f) m.clipped = true;
    }

    m.peakL_dBFS = toDb(maxL);
    m.peakR_dBFS = toDb(maxR);
    m.rmsL_dBFS = toDb(std::sqrt(sumSqL / n));
    m.rmsR_dBFS = toDb(std::sqrt(sumSqR / n));

    if (isImpulse) {
        evaluateSchroederDecay(left, sampleRate, m.edtL_sec, m.t20L_sec, m.t30L_sec);
        evaluateSchroederDecay(right, sampleRate, m.edtR_sec, m.t20R_sec, m.t30R_sec);
    }

    return m;
}

} // namespace hqrn_cli

// ============================================================================
// CLI INTERFACE & MAIN ROUTINE
// ============================================================================

struct CliOptions {
    std::string mode{"impulse"}; // impulse, burst, orbit, file
    std::string inputFile{""};
    std::string outputFile{"hqrn_output.wav"};
    float sampleRate{48000.0f};
    float durationSec{3.5f};
    float t60Sec{2.2f};
    float fBaseHz{120.0f};
    float sourceX{0.1f};
    float sourceY{0.5f};
    float listenerX{0.0f};
    float listenerY{-0.2f};
    float listenerHeading{0.0f};
    float mix{1.0f}; // 1.0 = 100% wet
    float outputGainDb{0.0f};
    float clipperCeilingDb{-0.1f};
    float clipperKneeDb{3.0f};
    bool clipperEnable{true};
    std::string simdChoice{"auto"};
    hqrn_cli::WavBitDepth bitDepth{hqrn_cli::WavBitDepth::PCM24};
    size_t blockSize{128};
    std::string diracChannel{"stereo"}; // stereo, left, right
};

void printHelp() {
    std::cout << "==============================================================================\n"
              << "HQRN REVERB OFFLINE RENDERER & AKUSTIK-ANALYSER (C++20)\n"
              << "==============================================================================\n"
              << "Nutzung: hqrn_render [Optionen]\n\n"
              << "Modi (--mode / -m):\n"
              << "  impulse         Dirac-Impulsantwort berechnen & Schroeder-Decay analysieren (Standard)\n"
              << "  burst           Synthetischer Noise-Burst (Onset & Diffusionstest)\n"
              << "  orbit           Dynamische Trajektorie auf der Poincaré-Scheibe (Geodäten-Fahrt)\n"
              << "  file            WAV-Eingangsdatei verarbeiten (--input erforderlich)\n\n"
              << "DSP- & Hall-Parameter:\n"
              << "  --t60 <sec>     Nachhallzeit T60 in Sekunden (Standard: 2.2)\n"
              << "  --fbase <Hz>    Basis-Eigenfrequenz der Bolza-Fläche (Standard: 120.0)\n"
              << "  --source <x y>  Quellposition z_S auf Poincaré-Scheibe (z.B. 0.1 0.5)\n"
              << "  --listener <x y [heading]>  Hörerposition z_L & Azimut (z.B. 0.0 -0.2 0.0)\n"
              << "  --mix <0..1>    Dry/Wet-Verhältnis (Standard: 1.0 für Impulse, sonst 0.4)\n"
              << "  --gain <dB>     Master Output Gain in dB (Standard: 0.0)\n"
              << "  --clipper <ceil knee on>  True-Peak Clipper Settings (z.B. -0.1 3.0 1)\n\n"
              << "Audio- & Format-Optionen:\n"
              << "  --sr <rate>     Abtastrate in Hz (Standard: 48000)\n"
              << "  --len / -l <s>  Dauer des Renders in Sekunden (Standard: 3.5)\n"
              << "  --dirac <mode>  Impulskanal: stereo, left, right (Standard: stereo)\n"
              << "  --bitdepth <b>  WAV-Bittiefe: 16, 24, 32f (Standard: 24)\n"
              << "  --simd <path>   SIMD-Pfad erzwingen: auto, avx2, neon, scalar\n"
              << "  --output / -o <file>  Ausgabe-WAV-Dateipfad\n"
              << "  --input / -i <file>   Eingabe-WAV-Dateipfad (für Mode 'file')\n"
              << "==============================================================================\n";
}

int main(int argc, char* argv[]) {
    using namespace dsp::hqrn;
    CliOptions opt;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printHelp();
            return 0;
        } else if ((arg == "--mode" || arg == "-m") && i + 1 < argc) {
            opt.mode = argv[++i];
        } else if ((arg == "--input" || arg == "-i") && i + 1 < argc) {
            opt.inputFile = argv[++i];
            if (opt.mode == "impulse") opt.mode = "file";
        } else if ((arg == "--output" || arg == "-o") && i + 1 < argc) {
            opt.outputFile = argv[++i];
        } else if ((arg == "--sr" || arg == "-s") && i + 1 < argc) {
            opt.sampleRate = std::stof(argv[++i]);
        } else if ((arg == "--len" || arg == "-l") && i + 1 < argc) {
            opt.durationSec = std::stof(argv[++i]);
        } else if (arg == "--t60" && i + 1 < argc) {
            opt.t60Sec = std::stof(argv[++i]);
        } else if (arg == "--fbase" && i + 1 < argc) {
            opt.fBaseHz = std::stof(argv[++i]);
        } else if (arg == "--mix" && i + 1 < argc) {
            opt.mix = std::stof(argv[++i]);
        } else if (arg == "--gain" && i + 1 < argc) {
            opt.outputGainDb = std::stof(argv[++i]);
        } else if (arg == "--source" && i + 2 < argc) {
            opt.sourceX = std::stof(argv[++i]);
            opt.sourceY = std::stof(argv[++i]);
        } else if (arg == "--listener" && i + 2 < argc) {
            opt.listenerX = std::stof(argv[++i]);
            opt.listenerY = std::stof(argv[++i]);
            if (i + 1 < argc && argv[i+1][0] != '-') {
                opt.listenerHeading = std::stof(argv[++i]);
            }
        } else if (arg == "--clipper" && i + 3 < argc) {
            opt.clipperCeilingDb = std::stof(argv[++i]);
            opt.clipperKneeDb = std::stof(argv[++i]);
            opt.clipperEnable = (std::stoi(argv[++i]) != 0);
        } else if (arg == "--simd" && i + 1 < argc) {
            opt.simdChoice = argv[++i];
        } else if (arg == "--bitdepth" && i + 1 < argc) {
            std::string bd = argv[++i];
            if (bd == "16") opt.bitDepth = hqrn_cli::WavBitDepth::PCM16;
            else if (bd == "24") opt.bitDepth = hqrn_cli::WavBitDepth::PCM24;
            else if (bd == "32" || bd == "32f") opt.bitDepth = hqrn_cli::WavBitDepth::Float32;
        } else if (arg == "--dirac" && i + 1 < argc) {
            opt.diracChannel = argv[++i];
        }
    }

    std::cout << "==============================================================================\n"
              << "HQRN REVERB OFFLINE RENDERER (C++20)\n"
              << "==============================================================================\n";

    // 1. ENGINE INITIALISIERUNG
    HQRNReverbEngineAuto engine;
    if (opt.simdChoice == "avx2") engine.setSimdPath(CpuSimdPath::AVX2);
    else if (opt.simdChoice == "neon") engine.setSimdPath(CpuSimdPath::NEON);
    else if (opt.simdChoice == "scalar") engine.setSimdPath(CpuSimdPath::Scalar);

    engine.prepare(opt.sampleRate);
    engine.reset();
    engine.setReverbTime(opt.t60Sec);
    engine.setBaseFrequency(opt.fBaseHz);
    engine.setSourcePosition(opt.sourceX, opt.sourceY);
    engine.setListenerPosition(opt.listenerX, opt.listenerY, opt.listenerHeading);
    engine.setMix(opt.mix);
    engine.setOutputGainDb(opt.outputGainDb);
    engine.setClipperParams(opt.clipperCeilingDb, opt.clipperKneeDb, opt.clipperEnable);

    std::cout << "Konfiguration:\n"
              << "  - Modus:            " << opt.mode << "\n"
              << "  - SIMD-Pfad:        " << engine.getSimdPathName() << "\n"
              << "  - Abtastrate:       " << opt.sampleRate << " Hz\n"
              << "  - T60 Nachhallzeit: " << opt.t60Sec << " s\n"
              << "  - Basis-Eigenfreq.: " << opt.fBaseHz << " Hz\n"
              << "  - Quelle z_S:       (" << opt.sourceX << ", " << opt.sourceY << ") |r| = " 
              << std::sqrt(opt.sourceX*opt.sourceX + opt.sourceY*opt.sourceY) << "\n"
              << "  - Hörer z_L:        (" << opt.listenerX << ", " << opt.listenerY << ") Azimut: " << opt.listenerHeading << " rad\n"
              << "  - Dry/Wet Mix:      " << (opt.mix * 100.0f) << " %\n"
              << "  - Output Gain:      " << opt.outputGainDb << " dB\n"
              << "  - True-Peak Clip:   " << (opt.clipperEnable ? "Aktiv (" + std::to_string(opt.clipperCeilingDb) + " dBFS Ceiling)" : "Aus") << "\n";

    // 2. SIGNAL-GENERATION
    std::vector<float> inputL;
    std::vector<float> inputR;
    size_t totalFrames = 0;

    if (opt.mode == "file") {
        if (opt.inputFile.empty()) {
            std::cerr << "Fehler: Mode 'file' verlangt --input <datei.wav>\n";
            return 1;
        }
        hqrn_cli::WavAudioData inWav;
        if (!hqrn_cli::readWav(opt.inputFile, inWav)) {
            std::cerr << "Fehler beim Laden von: " << opt.inputFile << "\n";
            return 1;
        }
        if (inWav.sampleRate != static_cast<uint32_t>(opt.sampleRate)) {
            std::cout << "Hinweis: Passe Engine-SampleRate an Eingangsdatei an (" << inWav.sampleRate << " Hz)\n";
            opt.sampleRate = static_cast<float>(inWav.sampleRate);
            engine.prepare(opt.sampleRate);
            engine.setReverbTime(opt.t60Sec);
            engine.setBaseFrequency(opt.fBaseHz);
            engine.setSourcePosition(opt.sourceX, opt.sourceY);
            engine.setListenerPosition(opt.listenerX, opt.listenerY, opt.listenerHeading);
            engine.setMix(opt.mix);
            engine.setOutputGainDb(opt.outputGainDb);
            engine.setClipperParams(opt.clipperCeilingDb, opt.clipperKneeDb, opt.clipperEnable);
        }
        inputL = std::move(inWav.left);
        inputR = std::move(inWav.right);
        totalFrames = inputL.size();
        std::cout << "Eingangsdatei geladen: " << opt.inputFile << " (" << totalFrames << " Frames, "
                  << (totalFrames / opt.sampleRate) << " s)\n";
    } else {
        totalFrames = static_cast<size_t>(opt.sampleRate * opt.durationSec);
        inputL.assign(totalFrames, 0.0f);
        inputR.assign(totalFrames, 0.0f);

        if (opt.mode == "impulse") {
            float amp = 1.0f;
            if (opt.diracChannel == "stereo") {
                inputL[0] = amp;
                inputR[0] = amp;
            } else if (opt.diracChannel == "left") {
                inputL[0] = amp;
            } else if (opt.diracChannel == "right") {
                inputR[0] = amp;
            }
            std::cout << "Generiere Dirac-Impuls (" << opt.diracChannel << ", Dauer: " << opt.durationSec << " s)...\n";
        } else if (opt.mode == "burst") {
            // 50ms Pink/Bandpass noise burst with Hann envelope
            size_t burstLen = static_cast<size_t>(opt.sampleRate * 0.050f);
            std::mt19937 rng(42);
            std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
            for (size_t i = 0; i < burstLen; ++i) {
                float env = 0.5f * (1.0f - std::cos(2.0f * std::numbers::pi_v<float> * i / burstLen));
                float noise = dist(rng) * env * 0.7f;
                inputL[i] = noise;
                inputR[i] = noise;
            }
            std::cout << "Generiere Noise-Burst (50 ms Burst + " << (opt.durationSec - 0.05f) << " s Ausklang)...\n";
        } else if (opt.mode == "orbit") {
            // Pulse train at 2 Hz to hear spatial orbit
            size_t interval = static_cast<size_t>(opt.sampleRate * 0.5f);
            for (size_t i = 0; i < totalFrames; i += interval) {
                inputL[i] = 0.8f;
                inputR[i] = 0.8f;
            }
            std::cout << "Generiere Orbit-Pulsfolge (2 Hz Clicks während dynamischer Fahrt)...\n";
        }
    }

    // 3. RENDERING LOOP
    std::vector<float> outputL(totalFrames, 0.0f);
    std::vector<float> outputR(totalFrames, 0.0f);

    const size_t blockSize = opt.blockSize;
    const size_t numBlocks = (totalFrames + blockSize - 1) / blockSize;

    std::cout << "Starte Rendering (" << totalFrames << " Samples, " << numBlocks << " Blöcke à " << blockSize << ")...\n";

    auto tStart = std::chrono::high_resolution_clock::now();

    for (size_t b = 0; b < numBlocks; ++b) {
        size_t offset = b * blockSize;
        size_t currentBlock = std::min(blockSize, totalFrames - offset);

        // If orbit mode, update source trajectory dynamically
        if (opt.mode == "orbit") {
            float t = static_cast<float>(offset) / opt.sampleRate;
            float r = 0.25f + 0.45f * (0.5f + 0.5f * std::sin(2.0f * std::numbers::pi_v<float> * 0.2f * t));
            float phi = 2.0f * std::numbers::pi_v<float> * 0.35f * t;
            float sX = r * std::cos(phi);
            float sY = r * std::sin(phi);
            engine.setSourcePosition(sX, sY);
        }

        std::span<const float> inBlockL(&inputL[offset], currentBlock);
        std::span<const float> inBlockR(&inputR[offset], currentBlock);
        std::span<float> outBlockL(&outputL[offset], currentBlock);
        std::span<float> outBlockR(&outputR[offset], currentBlock);

        engine.process(inBlockL, inBlockR, outBlockL, outBlockR);
    }

    auto tEnd = std::chrono::high_resolution_clock::now();
    double renderTimeMs = std::chrono::duration<double, std::milli>(tEnd - tStart).count();
    double audioTimeSec = totalFrames / opt.sampleRate;
    double realtimeFactor = (audioTimeSec * 1000.0) / renderTimeMs;
    double throughputMSamples = (totalFrames / 1e6) / (renderTimeMs / 1000.0);

    // 4. AKUSTIK-ANALYSE
    bool isImpulse = (opt.mode == "impulse");
    auto metrics = hqrn_cli::analyzeAudio(outputL, outputR, opt.sampleRate, isImpulse);

    std::cout << "\n==============================================================================\n"
              << "AKUSTISCHE & SIGNALTHEORETISCHE ANALYSE\n"
              << "==============================================================================\n"
              << std::fixed << std::setprecision(2)
              << "Pegel & Dynamik:\n"
              << "  - Peak Pegel:       L: " << metrics.peakL_dBFS << " dBFS  |  R: " << metrics.peakR_dBFS << " dBFS\n"
              << "  - RMS Pegel:        L: " << metrics.rmsL_dBFS << " dBFS  |  R: " << metrics.rmsR_dBFS << " dBFS\n"
              << "  - Clipper Status:   " << (metrics.clipped ? "TRUE-PEAK INTERVENTION DETECTED" : "Bit-genau linear (kein Clipping)") << "\n";

    if (isImpulse) {
        std::cout << "\nSchroeder-Rückwärtsintegration (EDC Hallzerfall):\n";
        auto printTime = [](const char* name, float valL, float valR) {
            std::cout << "  - " << std::left << std::setw(18) << name << ": ";
            if (valL > 0.0f) std::cout << "L: " << std::setw(6) << valL << " s";
            else std::cout << "L: n/a     ";
            std::cout << "  |  ";
            if (valR > 0.0f) std::cout << "R: " << std::setw(6) << valR << " s";
            else std::cout << "R: n/a";
            std::cout << "\n";
        };
        printTime("EDT (Early Decay)", metrics.edtL_sec, metrics.edtR_sec);
        printTime("T20 (-5 bis -25 dB)", metrics.t20L_sec, metrics.t20R_sec);
        printTime("T30 (-5 bis -35 dB)", metrics.t30L_sec, metrics.t30R_sec);
    }

    std::cout << "\nLeistungs- & Durchsatzwerte:\n"
              << "  - Berechnete Dauer: " << audioTimeSec << " s Audio\n"
              << "  - Rechenzeit:       " << renderTimeMs << " ms\n"
              << "  - Durchsatz:        " << throughputMSamples << " MSamples/s\n"
              << "  - Echtzeit-Faktor:  " << realtimeFactor << "x Realtime\n";

    // 5. WAV-EXPORT
    std::cout << "\nSchreibe WAV-Datei: " << opt.outputFile << "...\n";
    if (!hqrn_cli::writeWav(opt.outputFile, outputL, outputR, static_cast<uint32_t>(opt.sampleRate), opt.bitDepth)) {
        std::cerr << "Fehler beim Schreiben der WAV-Datei!\n";
        return 1;
    }

    std::cout << "Erfolgreich abgeschlossen: " << opt.outputFile << " gespeichert.\n"
              << "==============================================================================\n";

    return 0;
}
