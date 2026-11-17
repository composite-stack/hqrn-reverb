cmake_minimum_required(VERSION 3.20)
project(HQRN_Audio_DSP
    VERSION 0.1.0
    DESCRIPTION "Hyperbolic Quantum Resonator Network (HQRN) Reverb Audio DSP Engine"
    HOMEPAGE_URL "https://github.com/composite-stack/hqrn-reverb"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_SANITIZERS "Enable AddressSanitizer (ASan) and UndefinedBehaviorSanitizer (UBSan)" OFF)
option(ENABLE_BENCHMARKS "Build benchmark targets" OFF)
option(BUILD_TESTING "Build test targets" ON)

include(CheckCXXCompilerFlag)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

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

# ============================================================================
# Target 0: Interface Library (Header-only C++20 DSP Core)
# ============================================================================
add_library(hqrn INTERFACE)
target_include_directories(hqrn INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/hqrn>
)
target_compile_features(hqrn INTERFACE cxx_std_20)

# ============================================================================
# Target 1 & 2: Testing Targets
# ============================================================================
if(BUILD_TESTING)
    enable_testing()

    add_executable(test_cpu_dispatch test_cpu_dispatch.cpp)
    target_link_libraries(test_cpu_dispatch PRIVATE hqrn)
    if(SIMD_COMPILE_FLAGS)
        target_compile_options(test_cpu_dispatch PRIVATE ${SIMD_COMPILE_FLAGS})
    endif()
    add_test(NAME test_cpu_dispatch COMMAND test_cpu_dispatch)

    add_executable(test_cpu_dispatch_scalar test_cpu_dispatch.cpp)
    target_link_libraries(test_cpu_dispatch_scalar PRIVATE hqrn)
    if(SCALAR_COMPILE_FLAGS)
        target_compile_options(test_cpu_dispatch_scalar PRIVATE ${SCALAR_COMPILE_FLAGS})
    endif()
    add_test(NAME test_cpu_dispatch_scalar COMMAND test_cpu_dispatch_scalar)

    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        add_test(NAME test_metadata_verifier_negative
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_verify_diagnostic_metadata.py)
        add_test(NAME test_doc_anchors
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_markdown_anchors.py
                         ${CMAKE_CURRENT_SOURCE_DIR}/DIAGNOSTIC_METADATA_TESTING.md
                         --required beispielpfad-1 beispielpfad-2 beispielpfad-3 beispielpfad-4 beispielpfad-5 --strict-duplicates)
        add_test(NAME test_doc_anchors_suite
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_verify_markdown_anchors.py)
        add_test(NAME test_fuzz_percent_encoding
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fuzz_percent_encoding.py --iterations 1000)
    endif()
endif()

# ============================================================================
# Target 3: Optional Benchmark Target
# ============================================================================
if(ENABLE_BENCHMARKS)
    add_executable(benchmark_vector_paths benchmark_vector_paths.cpp)
    target_link_libraries(benchmark_vector_paths PRIVATE hqrn)
    if(SIMD_COMPILE_FLAGS)
        target_compile_options(benchmark_vector_paths PRIVATE ${SIMD_COMPILE_FLAGS})
    endif()
endif()

# ============================================================================
# Installation Rules
# ============================================================================
install(TARGETS hqrn
    EXPORT HQRNTargets
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(FILES
    HQRNReverbEngine.hpp
    HQRNKernels.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/hqrn
)

install(FILES
    README.md
    DIAGNOSTIC_METADATA_TESTING.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
)

if(TARGET benchmark_vector_paths)
    install(TARGETS benchmark_vector_paths
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

if(TARGET test_cpu_dispatch)
    install(TARGETS test_cpu_dispatch
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

# ============================================================================
# CMake Export & Package Configuration
# ============================================================================
set(CONFIG_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/HQRN")

install(EXPORT HQRNTargets
    FILE HQRNTargets.cmake
    NAMESPACE HQRN::
    DESTINATION ${CONFIG_INSTALL_DIR}
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/HQRNConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/HQRNConfig.cmake"
    "@PACKAGE_INIT@\ninclude(\"\${CMAKE_CURRENT_LIST_DIR}/HQRNTargets.cmake\")\n"
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/HQRNConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/HQRNConfigVersion.cmake"
    DESTINATION ${CONFIG_INSTALL_DIR}
)

# ============================================================================
# CPack Packaging Configuration
# ============================================================================
set(CPACK_PACKAGE_NAME "hqrn")
set(CPACK_PACKAGE_VENDOR "Composite Stack")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Hyperbolic Quantum Resonator Network (HQRN) Reverb Engine")
set(CPACK_GENERATOR "TGZ;ZIP")
include(CPack)
