# HQRN Reverb Engine (Hyperbolic Quantum Resonator Network)

A cutting-edge, resonance-based acoustic reverberation engine formulated in C++20. Instead of traditional Feedback Delay Networks (FDNs) or planar wave models, HQRN employs **Quantum Walks on Unitary Graphs with Poincaré Disk Geometry**, modal Selberg eigenvalue frequencies on the Bolza surface, and conservative energy bounds.

## Key Architecture & Features

- **16-Node / 32-State Resonator Core**: Discrete-time state-space dynamics $x[n+1] = A x[n] + B u[n]$ with Walsh-Hadamard 16 ($H_4 = H_1^{\otimes 4}$) unitary spatial coupling and hyperbolic phase rotation.
- **Hyperbolic Geometry (Poincaré Disk)**: Source injection vector $B(z_S)$ and binaural receiver matrix $C_{\text{out}}(z_L)$ mapped via conformal hyperbolic distances $\delta_u = \left|\frac{z_u - z_S}{1 - \bar{z}_S z_u}\right|$ and geodesic geodesics $d_H = 2 \operatorname{artanh}(\delta_u)$.
- **Energy Staging & Out-of-Core Limiting**: Linear internal resonator state space with strict statistical observability Gramian scaling $\|C\|_F^2 = 2N(1 - \bar{\gamma}^2)$ and an oversampled $4\times$ True-Peak polyphase halfband FIR output clipper.
- **SIMD Microarchitecture & Runtime Dispatch**:
  - Handcrafted AVX2 + FMA and ARM NEON block-processing kernels (zero dynamic allocations during `process()`).
  - Automated CPU capability detection (`CpuDetector`) with graceful scalar fallback.
- **Battle-Tested CI & Diagnostic Metadata Pipeline**:
  - Native multi-architecture CI matrix: Linux x86_64 (AVX2/FMA), Linux ARM64 (NEON), macOS Apple Silicon (M-Series NEON).
  - Full AddressSanitizer (ASan) and UndefinedBehaviorSanitizer (UBSan) coverage.
  - Automated diagnostic metadata validator ensuring consistent Run-Attempt, Trigger-Event, and Git-Ref tracking across JSON, text, and Markdown summaries.
  - RFC 3986 property-based fuzzing and Unicode parity verification for all documentation anchors and Mermaid diagrams.

## Repository Structure

```
├── HQRNReverbEngine.hpp          # Top-level C++20 Header-only HQRN Engine
├── HQRNKernels.hpp               # Vectorized AVX2, NEON & Scalar DSP Kernels + CPU Dispatcher
├── benchmark_hqrn.cpp            # Engine throughput, latency & allocation benchmarks
├── benchmark_vector_paths.cpp    # Comparative SIMD kernel benchmarks
├── test_cpu_dispatch.cpp         # Automated test suite for CPU capability dispatch & fallback
├── hqrn_render.cpp               # C++20 Offline Audio Renderer & Acoustic Analyzer CLI
├── DIAGNOSTIC_METADATA_TESTING.md# Comprehensive 5-stage triage guide, Mermaid flow & test matrices
├── CMakeLists.txt                # CMake build configuration with ASan/UBSan & benchmark options
├── Makefile                      # Quick Makefile for local development and sanitizer runs
├── scripts/
│   ├── verify_diagnostic_metadata.py      # CI metadata consistency verifier
│   ├── test_verify_diagnostic_metadata.py # 15 isolated negative inconsistency test scenarios
│   ├── verify_acoustic_metrics.py        # Schroeder decay & acoustic plausibility verifier (EDT, T20, T30)
│   ├── test_verify_acoustic_metrics.py   # Regression and negative injection test suite for acoustic metrics
│   ├── verify_markdown_anchors.py         # Markdown and Mermaid anchor integrity verifier
│   ├── test_verify_markdown_anchors.py    # 28 regression & parity unit tests
│   └── fuzz_percent_encoding.py           # 7-mode property-based fuzzing harness
└── .github/workflows/
    └── ci.yml                             # Native x86_64, ARM64 & macOS GitHub Actions workflow
```


## Offline Audio Renderer & Acoustic Analyzer CLI (`hqrn_render`)

A standalone CLI utility (`hqrn_render.cpp`) for offline rendering and signal-theoretic validation without external audio framework dependencies:

- **RIFF WAVE Engine**: Reads & writes uncompressed 16-bit, 24-bit PCM and 32-bit float stereo WAV files.
- **Operating Modes**:
  - `impulse`: Computes Dirac impulse responses and performs backward Schroeder energy integration ($EDC$, $EDT$, $T_{20}$, $T_{30}$).
  - `burst`: Generates windowed synthetic noise bursts to analyze onset diffusion and modal dispersion.
  - `orbit`: Simulates dynamic geodesic sound trajectories on the Poincaré disk ($z_S(t)$ orbit) with hyperbolic metric tracking.
  - `file`: Offline processes arbitrary stereo/mono WAV input through the HQRN engine.
- **Acoustic & Performance Telemetry**:
  - Schroeder decay curve analysis ($EDT$, $T_{20}$, $T_{30}$) for modal decay calibration.
  - Peak & RMS level measurement in dBFS with True-Peak intervention flags.
  - High-precision rendering throughput and real-time multiplier ($N \times \text{Realtime}$).

### CLI Examples & Usage
```bash
# Compile via Makefile
make render

# Generate 3.5s Stereo Impulse Response (24-bit / 48 kHz, T60 = 2.2s)
./hqrn_render --mode impulse --t60 2.2 --fbase 120 --len 3.5 -o impulse.wav

# Process external WAV file with AVX2 SIMD path
./hqrn_render --mode file --input dry_recording.wav --simd avx2 --mix 0.35 -o wet_mix.wav

# Dynamic Poincaré Disk Orbit Trajectory
./hqrn_render --mode orbit --len 6.0 --t60 2.0 -o orbit_trajectory.wav

# Export Acoustic Metrics JSON & Verify EDT, T20, T30 Plausibility
./hqrn_render --mode impulse --t60 1.2 --len 2.0 -o impulse.wav --metrics-json metrics.json
python3 scripts/verify_acoustic_metrics.py --metrics-json metrics.json --wav impulse.wav --target-t60 1.2
```

## Quick Start & Testing

### Building with CMake
```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DENABLE_BENCHMARKS=ON ..
cmake --build .
ctest --output-on-failure
```

### Running with AddressSanitizer & UndefinedBehaviorSanitizer
```bash
cmake -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON ..
cmake --build .
ctest --output-on-failure
```

### Acoustic Metrics Plausibility & Verification
```bash
# Run automated acoustic metrics test suite (EDT, T20, T30 validity, tolerances, error output)
python3 scripts/test_verify_acoustic_metrics.py
# or via Makefile
make test-acoustic-metrics
```

### Documentation & Diagnostic Verification
```bash
# Verify diagnostic metadata consistency
python3 scripts/verify_diagnostic_metadata.py

# Run anchor and link verification with strict duplicate checking
python3 scripts/verify_markdown_anchors.py DIAGNOSTIC_METADATA_TESTING.md --required beispielpfad-1 beispielpfad-2 beispielpfad-3 beispielpfad-4 beispielpfad-5 --strict-duplicates

# Run 28 regression and Unicode parity tests
python3 scripts/test_verify_markdown_anchors.py

# Run 7-mode property-based fuzzing (2000 iterations each)
python3 scripts/fuzz_percent_encoding.py --iterations 2000
```
