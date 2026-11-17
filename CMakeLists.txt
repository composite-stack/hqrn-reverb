cmake_minimum_required(VERSION 3.20)
project(HQRN_Audio_DSP LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_SANITIZERS "Enable AddressSanitizer (ASan) and UndefinedBehaviorSanitizer (UBSan)" OFF)
option(ENABLE_BENCHMARKS "Build benchmark targets" OFF)

include(CheckCXXCompilerFlag)

# Warning flags
if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Sanitizer configuration
if(ENABLE_SANITIZERS)
    message(STATUS "Configuring with ASan and UBSan sanitizers enabled")
    if(MSVC)
        add_compile_options(/fsanitize=address)
    else()
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all -g)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

# Architecture & SIMD detection
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i[3-6]86")
    message(STATUS "Configuring for x86_64 architecture")
    if(MSVC)
        set(SIMD_COMPILE_FLAGS /arch:AVX2 /O2)
    else()
        set(SIMD_COMPILE_FLAGS -mavx2 -mfma)
    endif()
    set(SCALAR_COMPILE_FLAGS -mno-avx2 -mno-fma -DHQRN_FORCE_SCALAR=1)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
    message(STATUS "Configuring for ARM64/AArch64 architecture (Native NEON)")
    set(SIMD_COMPILE_FLAGS "")
    set(SCALAR_COMPILE_FLAGS -DHQRN_FORCE_SCALAR=1)
else()
    message(STATUS "Configuring for Generic/Scalar architecture")
    set(SIMD_COMPILE_FLAGS "")
    set(SCALAR_COMPILE_FLAGS -DHQRN_FORCE_SCALAR=1)
endif()

enable_testing()

# ============================================================================
# Target 1: Native SIMD Dispatch Test
# ============================================================================
add_executable(test_cpu_dispatch test_cpu_dispatch.cpp)
target_include_directories(test_cpu_dispatch PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(SIMD_COMPILE_FLAGS)
    target_compile_options(test_cpu_dispatch PRIVATE ${SIMD_COMPILE_FLAGS})
endif()

add_test(NAME test_cpu_dispatch COMMAND test_cpu_dispatch)

# ============================================================================
# Target 2: Explicit Scalar Fallback Test (all platforms)
# ============================================================================
add_executable(test_cpu_dispatch_scalar test_cpu_dispatch.cpp)
target_include_directories(test_cpu_dispatch_scalar PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(SCALAR_COMPILE_FLAGS)
    target_compile_options(test_cpu_dispatch_scalar PRIVATE ${SCALAR_COMPILE_FLAGS})
endif()

add_test(NAME test_cpu_dispatch_scalar COMMAND test_cpu_dispatch_scalar)

# ============================================================================
# Optional Benchmark Target
# ============================================================================
if(ENABLE_BENCHMARKS)
    add_executable(benchmark_vector_paths benchmark_vector_paths.cpp)
    target_include_directories(benchmark_vector_paths PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if(SIMD_COMPILE_FLAGS)
        target_compile_options(benchmark_vector_paths PRIVATE ${SIMD_COMPILE_FLAGS})
    endif()
endif()

# ============================================================================
# Target 3: Diagnostic Metadata Consistency Negative Test Suite
# ============================================================================
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME test_metadata_verifier_negative
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_verify_diagnostic_metadata.py)
endif()

# ============================================================================
# Target 4: Documentation Anchor and Link Integrity Test
# ============================================================================
if(Python3_FOUND)
    add_test(NAME test_doc_anchors
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_markdown_anchors.py
                     ${CMAKE_CURRENT_SOURCE_DIR}/DIAGNOSTIC_METADATA_TESTING.md
                     --required beispielpfad-1 beispielpfad-2 beispielpfad-3 beispielpfad-4 beispielpfad-5 --strict-duplicates)
    add_test(NAME test_doc_anchors_suite
             COMMAND  /scripts/test_verify_markdown_anchors.py)
    add_test(NAME test_fuzz_percent_encoding
             COMMAND  /scripts/fuzz_percent_encoding.py --iterations 1000)
endif()
