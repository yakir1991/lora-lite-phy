cmake_minimum_required(VERSION 3.16)
project(lora_cpp_receiver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static library with the core LoRa receiver implementation.
add_library(lora_cpp_receiver STATIC
    src/chirp_generator.cpp
    src/fft_utils.cpp
    src/frame_sync.cpp
    src/header_decoder.cpp
    src/hamming.cpp
    src/iq_loader.cpp
    src/preamble_detector.cpp
    src/payload_decoder.cpp
    src/receiver.cpp
    src/streaming_receiver.cpp
    src/sync_word_detector.cpp
)

# Public include path for headers under include/.
target_include_directories(lora_cpp_receiver
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Optional Liquid-DSP acceleration (FFT). When enabled, the library will attempt to use
# Liquid-DSP for power-of-two FFTs; otherwise it falls back to a builtin radix-2 implementation.
# Set LORA_REQUIRE_LIQUID=ON to make failure to use Liquid a hard error at runtime.
option(LORA_WITH_LIQUID_DSP "Enable Liquid-DSP accelerated FFT" ON)
option(LORA_REQUIRE_LIQUID "Require Liquid-DSP at runtime (no fallback)" OFF)
option(LORA_EMBEDDED_PROFILE "Enable embedded-friendly build defaults (no Liquid-DSP, minimal STL)" OFF)

if (LORA_WITH_LIQUID_DSP)
    set(LIQUID_FOUND FALSE)
    find_package(PkgConfig QUIET)
    if (PkgConfig_FOUND)
        # Try both common pkg-config names for Liquid-DSP
        pkg_check_modules(LIQUIDDSP QUIET liquid)
        if (NOT LIQUIDDSP_FOUND)
            pkg_check_modules(LIQUIDDSP QUIET liquid-dsp)
        endif()
        if (LIQUIDDSP_FOUND)
            set(LIQUID_FOUND TRUE)
            target_include_directories(lora_cpp_receiver PRIVATE ${LIQUIDDSP_INCLUDE_DIRS})
            target_link_libraries(lora_cpp_receiver PUBLIC ${LIQUIDDSP_LIBRARIES})
            message(STATUS "Liquid-DSP enabled via pkg-config: ${LIQUIDDSP_VERSION}")
        endif()
    endif()

    if (NOT LIQUID_FOUND)
        # Fallback: use vendored build under external/liquid-dsp/build
        set(LIQUID_VENDOR_BUILD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../external/liquid-dsp/build)
        set(LIQUID_VENDOR_LIB ${LIQUID_VENDOR_BUILD_DIR}/libliquid.so)
        set(LIQUID_VENDOR_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../external/liquid-dsp/include)
        if (EXISTS ${LIQUID_VENDOR_LIB} AND EXISTS ${LIQUID_VENDOR_INCLUDE}/liquid.h)
            set(LIQUID_FOUND TRUE)
            target_include_directories(lora_cpp_receiver PRIVATE ${LIQUID_VENDOR_INCLUDE})
            target_link_libraries(lora_cpp_receiver PUBLIC ${LIQUID_VENDOR_LIB})
            message(STATUS "Liquid-DSP enabled via vendored build: ${LIQUID_VENDOR_LIB}")
        endif()
    endif()

    if (LIQUID_FOUND)
        target_compile_definitions(lora_cpp_receiver PRIVATE LORA_ENABLE_LIQUID_DSP)
    else()
        message(FATAL_ERROR "LORA_WITH_LIQUID_DSP=ON but Liquid-DSP not found (pkg-config nor vendored build). Install libliquid or build external/liquid-dsp.")
    endif()
endif()

# If using vendored Liquid-DSP, add RPATH to executables so they can find libliquid.so at runtime
if (DEFINED LIQUID_VENDOR_BUILD_DIR)
    foreach(tgt IN ITEMS run_stage_tests decode_cli sync_mags)
        if (TARGET ${tgt})
            set_target_properties(${tgt} PROPERTIES BUILD_RPATH ${LIQUID_VENDOR_BUILD_DIR})
        endif()
    endforeach()
endif()

if (LORA_REQUIRE_LIQUID)
    target_compile_definitions(lora_cpp_receiver PRIVATE LORA_REQUIRE_LIQUID)
endif()

if (LORA_EMBEDDED_PROFILE)
    # Embedded profile is intended for cross-compilation to MCUs/SoCs where dynamic allocation
    # and heavy STL features should be avoided. Pair with a toolchain file as needed.
    if (LORA_WITH_LIQUID_DSP)
        message(WARNING "LORA_EMBEDDED_PROFILE=ON: forcing LORA_WITH_LIQUID_DSP=OFF for minimal dependencies")
    endif()
    set(LORA_WITH_LIQUID_DSP OFF CACHE BOOL "" FORCE)
    target_compile_definitions(lora_cpp_receiver PUBLIC LORA_EMBEDDED_PROFILE)
    set_target_properties(lora_cpp_receiver PROPERTIES POSITION_INDEPENDENT_CODE OFF)
endif()

# Expose the source directory to code (used by tests to find vectors).
target_compile_definitions(lora_cpp_receiver
    PUBLIC
        LORA_CPP_RECEIVER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

# Minimal dependency-free stage tests exercising the pipeline.
add_executable(run_stage_tests
    tests/run_stage_tests.cpp
)

target_link_libraries(run_stage_tests PRIVATE lora_cpp_receiver)

# Provide the same SOURCE_DIR define to the test target.
target_compile_definitions(run_stage_tests PRIVATE LORA_CPP_RECEIVER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

# Simple CLI tool for decoding cf32 files using the library.
add_executable(decode_cli
    tools/decode_cli.cpp
)

target_link_libraries(decode_cli PRIVATE lora_cpp_receiver)

# Diagnostic tool: print sync word analysis magnitudes for a given vector
add_executable(sync_mags tools/sync_mags.cpp)
target_link_libraries(sync_mags PRIVATE lora_cpp_receiver)

add_executable(streaming_harness tools/streaming_harness.cpp)
target_link_libraries(streaming_harness PRIVATE lora_cpp_receiver)

# Warnings on all targets, MSVC and GCC/Clang.
if (MSVC)
    target_compile_options(lora_cpp_receiver PRIVATE /W4 /permissive-)
    target_compile_options(run_stage_tests PRIVATE /W4 /permissive-)
    target_compile_options(decode_cli PRIVATE /W4 /permissive-)
else()
    target_compile_options(lora_cpp_receiver PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(run_stage_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(decode_cli PRIVATE -Wall -Wextra -Wpedantic)
endif()
