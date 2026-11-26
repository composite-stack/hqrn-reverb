#ifndef HQRN_C_API_H
#define HQRN_C_API_H

/**
 * @file hqrn_c_api.h
 * @brief C-kompatible ABI- und WebAssembly-Bridge für die HQRN Reverb Engine.
 *
 * Ermöglicht den direkten, allokationsfreien Einsatz in WebAudio AudioWorklet,
 * C-basierten Plug-in-Wrappern (CLAP, VST3, AU) und FFI-Schnittstellen.
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define HQRN_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#if defined(_WIN32) || defined(__CYGWIN__)
#define HQRN_EXPORT __declspec(dllexport)
#else
#define HQRN_EXPORT __attribute__((visibility("default")))
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void* HQRNHandle;

/* ============================================================================
 * 1. LIFECYCLE & KONFIGURATION
 * ============================================================================ */

/**
 * Erzeugt eine neue HQRN-Reverb-Instanz mit DAW-Puffer-Adapter.
 *
 * @param sampleRate Abtastrate in Hz (z. B. 48000.0)
 * @param maxBlockSize Maximale erwartete Blockgröße (z. B. 128 für AudioWorklet, bis 8192)
 * @param internalQuantum Interne Quantisierung für den Halbband-Oversampler (Standard: 128)
 * @return Opaque Handle auf die Instanz oder NULL bei Allokationsfehler
 */
HQRN_EXPORT HQRNHandle hqrn_create(double sampleRate, uint32_t maxBlockSize, uint32_t internalQuantum);

/**
 * Gibt eine HQRN-Instanz frei.
 */
HQRN_EXPORT void hqrn_destroy(HQRNHandle handle);

/**
 * Rekonfiguriert die Instanz (z. B. bei Sample-Rate- oder Buffer-Size-Wechsel im Audio-Host).
 */
HQRN_EXPORT void hqrn_prepare(HQRNHandle handle, double sampleRate, uint32_t maxBlockSize, uint32_t internalQuantum);

/**
 * Setzt alle internen Resonatorzustände, Filterverzögerungen und FIFO-Puffer zurück.
 */
HQRN_EXPORT void hqrn_reset(HQRNHandle handle);

/**
 * Setzt den Puffer-Adapter-Modus (0 = QuantizedFifo, 1 = Direct Slicing).
 */
HQRN_EXPORT void hqrn_set_adapter_mode(HQRNHandle handle, int32_t mode);

/**
 * Gibt den aktuellen Adapter-Modus zurück (0 = QuantizedFifo, 1 = Direct Slicing).
 */
HQRN_EXPORT int32_t hqrn_get_adapter_mode(HQRNHandle handle);

/* ============================================================================
 * 2. ZERO-ALLOCATION WASM I/O PUFFER
 * ============================================================================ */

/**
 * Zeiger auf die intern vorallokierten I/O-Scratchpads im linearen Speicher.
 * JavaScript kann Audio direkt in diese Float32-Puffer kopieren.
 */
HQRN_EXPORT float* hqrn_get_input_buffer_l(HQRNHandle handle);
HQRN_EXPORT float* hqrn_get_input_buffer_r(HQRNHandle handle);
HQRN_EXPORT float* hqrn_get_output_buffer_l(HQRNHandle handle);
HQRN_EXPORT float* hqrn_get_output_buffer_r(HQRNHandle handle);
HQRN_EXPORT uint32_t hqrn_get_buffer_capacity(HQRNHandle handle);

/* ============================================================================
 * 3. AUDIO-VERARBEITUNG
 * ============================================================================ */

/**
 * Verarbeitet Audio unter Verwendung der internen I/O-Scratchpads.
 *
 * @param handle HQRN-Instanz
 * @param numFrames Anzahl zu verarbeitender Audio-Frames (<= buffer_capacity)
 */
HQRN_EXPORT void hqrn_process(HQRNHandle handle, uint32_t numFrames);

/**
 * Verarbeitet planar getrennte Puffer über externe Zeiger.
 */
HQRN_EXPORT void hqrn_process_planar(HQRNHandle handle,
                                     const float* inL, const float* inR,
                                     float* outL, float* outR,
                                     uint32_t numFrames);

/**
 * Verarbeitet interleaved Stereodaten (L0, R0, L1, R1...).
 */
HQRN_EXPORT void hqrn_process_interleaved(HQRNHandle handle,
                                         const float* inInterleaved,
                                         float* outInterleaved,
                                         uint32_t numFrames);

/* ============================================================================
 * 4. PARAMETER-STEUERUNG (DIREKT)
 * ============================================================================ */

HQRN_EXPORT void hqrn_set_base_frequency(HQRNHandle handle, float fBaseHz);
HQRN_EXPORT void hqrn_set_reverb_time(HQRNHandle handle, float t60Sec);
HQRN_EXPORT void hqrn_set_source_position(HQRNHandle handle, float x, float y);
HQRN_EXPORT void hqrn_set_listener_position(HQRNHandle handle, float x, float y, float headingRad);
HQRN_EXPORT void hqrn_set_coupling_phase(HQRNHandle handle, float thetaRad);
HQRN_EXPORT void hqrn_set_coupling_theta(HQRNHandle handle, float thetaRad);
HQRN_EXPORT float hqrn_get_coupling_phase(HQRNHandle handle);
HQRN_EXPORT float hqrn_get_coupling_theta(HQRNHandle handle);
HQRN_EXPORT void hqrn_set_mix(HQRNHandle handle, float wetRatio);
HQRN_EXPORT void hqrn_set_output_gain_db(HQRNHandle handle, float gainDb);
HQRN_EXPORT void hqrn_set_clipper_params(HQRNHandle handle, float ceilingDb, float kneeDb, int32_t enable);

/* ============================================================================
 * 5. PARAMETER-GLÄTTUNG (SLEWING FUER INTERAKTIVE UI / MAUSBEWEGUNGEN)
 * ============================================================================ */

HQRN_EXPORT void hqrn_set_source_target(HQRNHandle handle, float targetX, float targetY, float smoothTimeSec);
HQRN_EXPORT void hqrn_set_listener_target(HQRNHandle handle, float targetX, float targetY, float targetHeadingRad, float smoothTimeSec);
HQRN_EXPORT void hqrn_set_reverb_time_target(HQRNHandle handle, float targetT60Sec, float smoothTimeSec);
HQRN_EXPORT void hqrn_set_base_frequency_target(HQRNHandle handle, float targetFBaseHz, float smoothTimeSec);
HQRN_EXPORT void hqrn_set_coupling_phase_target(HQRNHandle handle, float targetThetaRad, float smoothTimeSec);
HQRN_EXPORT void hqrn_set_coupling_theta_target(HQRNHandle handle, float targetThetaRad, float smoothTimeSec);
HQRN_EXPORT void hqrn_set_mix_target(HQRNHandle handle, float targetWetRatio, float smoothTimeSec);

/* ============================================================================
 * 6. STATUS, LATENZ & TELEMETRIE
 * ============================================================================ */

HQRN_EXPORT uint32_t hqrn_get_latency_samples(HQRNHandle handle);
HQRN_EXPORT float hqrn_get_sample_rate(HQRNHandle handle);

/**
 * Schreibt die 16 aktuellen modalen Energien (q_u^2 + p_u^2) der Resonatoren
 * in das uebergebene Array aus 16 Floats.
 */
HQRN_EXPORT void hqrn_get_mode_energies(HQRNHandle handle, float* outEnergies);

/**
 * Liefert einen Zeiger auf den internen 16-Float-Puffer der Modenenergien
 * im linearen Speicher (fuer allokationsfreies Auslesen via HEAPF32 in WebAssembly).
 */
HQRN_EXPORT const float* hqrn_get_mode_energies_ptr(HQRNHandle handle);

/**
 * Liefert die modale Energie eines einzelnen Resonators (Index 0..15).
 */
HQRN_EXPORT float hqrn_get_mode_energy(HQRNHandle handle, uint32_t modeIndex);

/**
 * Liefert den aktuellen Modendichte-Eichfaktor g_rad (Weg A).
 */
HQRN_EXPORT float hqrn_get_grad(HQRNHandle handle);

/**
 * Liefert den aktuellen hyperbolischen Abstand zwischen Quelle und Hoerer d_H(z_S, z_L).
 */
HQRN_EXPORT float hqrn_get_source_listener_distance(HQRNHandle handle);

/* ============================================================================
 * 7. POINCARÉ-GEOMETRIE & TOPOLOGIE (FUER WEB-CANVAS / VISUALISIERUNG)
 * ============================================================================ */

/**
 * Schreibt die 16 (x, y) Koordinaten der Resonatorknoten auf der Poincaré-Scheibe
 * in ein Array aus 32 Floats ([x0, y0, x1, y1, ...]).
 */
HQRN_EXPORT void hqrn_get_node_coordinates(float* outCoordsXY);
HQRN_EXPORT uint32_t hqrn_get_node_count(void);

#ifdef __cplusplus
}
#endif

#endif /* HQRN_C_API_H */
