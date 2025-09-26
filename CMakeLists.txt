cmake_minimum_required(VERSION 3.22)
project(lora_lite_phy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(LORA_LITE_BUILD_TESTS "Build unit tests" ON)

# Basic include path
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(lora_lite STATIC
    src/whitening.cpp
    src/crc16.cpp
    src/gray.cpp
    src/hamming.cpp
    src/interleaver.cpp
  src/frame_sync_lite.cpp
  src/receiver_lite.cpp
)

add_executable(lora_lite_loopback src/main_loopback.cpp)
target_link_libraries(lora_lite_loopback PRIVATE lora_lite)
add_executable(run_vector src/run_vector.cpp)
target_link_libraries(run_vector PRIVATE lora_lite)

if(LORA_LITE_BUILD_TESTS)
  enable_testing()
  foreach(T IN ITEMS whitening crc gray hamming interleaver fft_demod_lite)
    add_executable(test_${T} tests/test_${T}.cpp)
    target_link_libraries(test_${T} PRIVATE lora_lite)
    add_test(NAME test_${T} COMMAND test_${T})
  endforeach()
  add_executable(test_receiver_lite tests/test_receiver_lite.cpp)
  target_link_libraries(test_receiver_lite PRIVATE lora_lite)
  add_test(NAME test_receiver_lite COMMAND test_receiver_lite)
  add_executable(test_fft_demod_parity tests/test_fft_demod_parity.cpp)
  target_link_libraries(test_fft_demod_parity PRIVATE lora_lite)
  add_test(NAME test_fft_demod_parity COMMAND test_fft_demod_parity)
  add_executable(test_fft_demod_cfo_parity tests/test_fft_demod_cfo_parity.cpp)
  target_link_libraries(test_fft_demod_cfo_parity PRIVATE lora_lite)
  add_test(NAME test_fft_demod_cfo_parity COMMAND test_fft_demod_cfo_parity)
  add_executable(test_fft_demod_param_grid tests/test_fft_demod_param_grid.cpp)
  target_link_libraries(test_fft_demod_param_grid PRIVATE lora_lite)
  add_test(NAME test_fft_demod_param_grid COMMAND test_fft_demod_param_grid)
  add_executable(test_gray_parity tests/test_gray_parity.cpp)
  target_link_libraries(test_gray_parity PRIVATE lora_lite)
  add_test(NAME test_gray_parity COMMAND test_gray_parity)
  add_executable(test_deinterleaver_parity tests/test_deinterleaver_parity.cpp)
  target_link_libraries(test_deinterleaver_parity PRIVATE lora_lite)
  add_test(NAME test_deinterleaver_parity COMMAND test_deinterleaver_parity)
  add_executable(test_hamming_parity tests/test_hamming_parity.cpp)
  target_link_libraries(test_hamming_parity PRIVATE lora_lite)
  add_test(NAME test_hamming_parity COMMAND test_hamming_parity)
  
  add_executable(test_hamming_param_grid tests/test_hamming_param_grid.cpp)
  target_link_libraries(test_hamming_param_grid PRIVATE lora_lite)
  add_test(NAME hamming_param_grid COMMAND test_hamming_param_grid)

  add_executable(test_full_chain_parity tests/test_full_chain_parity.cpp)
  target_link_libraries(test_full_chain_parity PRIVATE lora_lite)
  add_test(NAME test_full_chain_parity COMMAND test_full_chain_parity)
  add_executable(test_receiver_debug_bits tests/test_receiver_debug_bits.cpp)
  target_link_libraries(test_receiver_debug_bits PRIVATE lora_lite)
  add_test(NAME test_receiver_debug_bits COMMAND test_receiver_debug_bits)
endif()

message(STATUS "lora_lite configured (tests=${LORA_LITE_BUILD_TESTS})")
