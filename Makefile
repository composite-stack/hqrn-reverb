CXX ?= g++
CXXFLAGS ?= -std=c++20 -O3 -Wall -Wextra -Wpedantic
SAN_FLAGS ?= -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all -g

.PHONY: all test test-avx2 test-scalar test-arm-neon test-asan-ubsan test-asan-avx2 test-asan-scalar test-asan-neon clean

all: test_cpu_dispatch test_cpu_dispatch_scalar test_daw_adapter test_daw_adapter_scalar

test_cpu_dispatch: test_cpu_dispatch.cpp HQRNKernels.hpp HQRNReverbEngine.hpp
	$(CXX) $(CXXFLAGS) -mavx2 -mfma test_cpu_dispatch.cpp -o test_cpu_dispatch

test_cpu_dispatch_scalar: test_cpu_dispatch.cpp HQRNKernels.hpp HQRNReverbEngine.hpp
	$(CXX) $(CXXFLAGS) -mno-avx2 -mno-fma -DHQRN_FORCE_SCALAR=1 test_cpu_dispatch.cpp -o test_cpu_dispatch_scalar

test_cpu_dispatch_neon: test_cpu_dispatch.cpp HQRNKernels.hpp HQRNReverbEngine.hpp
	$(CXX) $(CXXFLAGS) test_cpu_dispatch.cpp -o test_cpu_dispatch_neon

# Sanitizer Binaries (ASan + UBSan)
test_cpu_dispatch_asan: test_cpu_dispatch.cpp HQRNKernels.hpp HQRNReverbEngine.hpp
	$(CXX) $(CXXFLAGS) $(SAN_FLAGS) -mavx2 -mfma test_cpu_dispatch.cpp -o test_cpu_dispatch_asan

test_cpu_dispatch_scalar_asan: test_cpu_dispatch.cpp HQRNKernels.hpp HQRNReverbEngine.hpp
	$(CXX) $(CXXFLAGS) $(SAN_FLAGS) -mno-avx2 -mno-fma -DHQRN_FORCE_SCALAR=1 test_cpu_dispatch.cpp -o test_cpu_dispatch_scalar_asan

test_cpu_dispatch_neon_asan: test_cpu_dispatch.cpp HQRNKernels.hpp HQRNReverbEngine.hpp
	$(CXX) $(CXXFLAGS) $(SAN_FLAGS) test_cpu_dispatch.cpp -o test_cpu_dispatch_neon_asan

test: test-avx2 test-scalar test-adapter test-adapter-scalar

test-avx2: test_cpu_dispatch
	./test_cpu_dispatch

test-scalar: test_cpu_dispatch_scalar
	./test_cpu_dispatch_scalar

test-arm-neon: test_cpu_dispatch_neon
	./test_cpu_dispatch_neon

# Sanitizer Execution (ASan + UBSan)
test-asan-ubsan: test-asan-avx2 test-asan-scalar test-asan-adapter test-asan-adapter-scalar

test-asan-avx2: test_cpu_dispatch_asan
	ASAN_OPTIONS="halt_on_error=1:abort_on_error=1:detect_leaks=1" \
	UBSAN_OPTIONS="halt_on_error=1:abort_on_error=1:print_stacktrace=1" \
	./test_cpu_dispatch_asan

test-asan-scalar: test_cpu_dispatch_scalar_asan
	ASAN_OPTIONS="halt_on_error=1:abort_on_error=1:detect_leaks=1" \
	UBSAN_OPTIONS="halt_on_error=1:abort_on_error=1:print_stacktrace=1" \
	./test_cpu_dispatch_scalar_asan

test-asan-neon: test_cpu_dispatch_neon_asan
	ASAN_OPTIONS="halt_on_error=1:abort_on_error=1:detect_leaks=1" \
	UBSAN_OPTIONS="halt_on_error=1:abort_on_error=1:print_stacktrace=1" \
	./test_cpu_dispatch_neon_asan



# DAW Buffer Adapter Targets
test_daw_adapter: test_daw_adapter.cpp HQRNKernels.hpp HQRNReverbEngine.hpp HQRNDawAdapter.hpp
	$(CXX) $(CXXFLAGS) -mavx2 -mfma test_daw_adapter.cpp -o test_daw_adapter

test_daw_adapter_scalar: test_daw_adapter.cpp HQRNKernels.hpp HQRNReverbEngine.hpp HQRNDawAdapter.hpp
	$(CXX) $(CXXFLAGS) -mno-avx2 -mno-fma -DHQRN_FORCE_SCALAR=1 test_daw_adapter.cpp -o test_daw_adapter_scalar

test_daw_adapter_neon: test_daw_adapter.cpp HQRNKernels.hpp HQRNReverbEngine.hpp HQRNDawAdapter.hpp
	$(CXX) $(CXXFLAGS) test_daw_adapter.cpp -o test_daw_adapter_neon

test_daw_adapter_asan: test_daw_adapter.cpp HQRNKernels.hpp HQRNReverbEngine.hpp HQRNDawAdapter.hpp
	$(CXX) $(CXXFLAGS) $(SAN_FLAGS) -mavx2 -mfma test_daw_adapter.cpp -o test_daw_adapter_asan

test_daw_adapter_scalar_asan: test_daw_adapter.cpp HQRNKernels.hpp HQRNReverbEngine.hpp HQRNDawAdapter.hpp
	$(CXX) $(CXXFLAGS) $(SAN_FLAGS) -mno-avx2 -mno-fma -DHQRN_FORCE_SCALAR=1 test_daw_adapter.cpp -o test_daw_adapter_scalar_asan

test_daw_adapter_neon_asan: test_daw_adapter.cpp HQRNKernels.hpp HQRNReverbEngine.hpp HQRNDawAdapter.hpp
	$(CXX) $(CXXFLAGS) $(SAN_FLAGS) test_daw_adapter.cpp -o test_daw_adapter_neon_asan

test-adapter: test_daw_adapter
	./test_daw_adapter

test-adapter-scalar: test_daw_adapter_scalar
	./test_daw_adapter_scalar

test-adapter-neon: test_daw_adapter_neon
	./test_daw_adapter_neon

test-asan-adapter: test_daw_adapter_asan
	ASAN_OPTIONS="halt_on_error=1:abort_on_error=1:detect_leaks=1" \
	UBSAN_OPTIONS="halt_on_error=1:abort_on_error=1:print_stacktrace=1" \
	./test_daw_adapter_asan

test-asan-adapter-scalar: test_daw_adapter_scalar_asan
	ASAN_OPTIONS="halt_on_error=1:abort_on_error=1:detect_leaks=1" \
	UBSAN_OPTIONS="halt_on_error=1:abort_on_error=1:print_stacktrace=1" \
	./test_daw_adapter_scalar_asan

UNAME_M := $(shell uname -m)

# Offline Renderer Target
.PHONY: render
render: hqrn_render

hqrn_render: hqrn_render.cpp HQRNKernels.hpp HQRNReverbEngine.hpp
ifeq ($(UNAME_M),x86_64)
	$(CXX) $(CXXFLAGS) -mavx2 -mfma hqrn_render.cpp -o hqrn_render
else
	$(CXX) $(CXXFLAGS) hqrn_render.cpp -o hqrn_render
endif

clean:
	rm -f test_cpu_dispatch test_cpu_dispatch_scalar test_cpu_dispatch_neon \
	      test_cpu_dispatch_asan test_cpu_dispatch_scalar_asan test_cpu_dispatch_neon_asan \
	      test_daw_adapter test_daw_adapter_scalar test_daw_adapter_neon \
	      test_daw_adapter_asan test_daw_adapter_scalar_asan test_daw_adapter_neon_asan \
	      hqrn_render test_render_impulse.wav *.wav

# Diagnostic Metadata Verifier Negative Test Suite
.PHONY: test-metadata-negative
test-metadata-negative:
	python3 scripts/test_verify_diagnostic_metadata.py

# Documentation Anchor Integrity Test
.PHONY: test-doc-anchors
test-doc-anchors:
	python3 scripts/verify_markdown_anchors.py DIAGNOSTIC_METADATA_TESTING.md --required beispielpfad-1 beispielpfad-2 beispielpfad-3 beispielpfad-4 beispielpfad-5 --strict-duplicates
	python3 scripts/test_verify_markdown_anchors.py

# Property-based and Fuzz Testing for Percent-Encoding & UTF-8
.PHONY: test-fuzz
test-fuzz:
	python3 scripts/fuzz_percent_encoding.py --iterations 2000 --verbose

# Acoustic Metrics Plausibility Test Suite
.PHONY: test-acoustic-metrics
test-acoustic-metrics:
	python3 scripts/test_verify_acoustic_metrics.py

# End-to-End Integration Tests (Renderer --metrics-json + Acoustic Verifier)
.PHONY: test-render-integration
test-render-integration: render
	python3 scripts/test_render_metrics_integration.py
