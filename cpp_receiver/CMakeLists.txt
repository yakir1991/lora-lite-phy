cmake_minimum_required(VERSION 3.16)
project(lora_cpp_receiver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(lora_cpp_receiver STATIC
    src/chirp_generator.cpp
    src/frame_sync.cpp
    src/header_decoder.cpp
    src/hamming.cpp
    src/iq_loader.cpp
    src/preamble_detector.cpp
    src/payload_decoder.cpp
    src/receiver.cpp
    src/sync_word_detector.cpp
)

target_include_directories(lora_cpp_receiver
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(lora_cpp_receiver
    PUBLIC
        LORA_CPP_RECEIVER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

add_executable(run_stage_tests
    tests/run_stage_tests.cpp
)

target_link_libraries(run_stage_tests PRIVATE lora_cpp_receiver)

target_compile_definitions(run_stage_tests PRIVATE LORA_CPP_RECEIVER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(decode_cli
    tools/decode_cli.cpp
)

target_link_libraries(decode_cli PRIVATE lora_cpp_receiver)

if (MSVC)
    target_compile_options(lora_cpp_receiver PRIVATE /W4 /permissive-)
    target_compile_options(run_stage_tests PRIVATE /W4 /permissive-)
    target_compile_options(decode_cli PRIVATE /W4 /permissive-)
else()
    target_compile_options(lora_cpp_receiver PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(run_stage_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(decode_cli PRIVATE -Wall -Wextra -Wpedantic)
endif()
