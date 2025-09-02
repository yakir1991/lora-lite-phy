cmake_minimum_required(VERSION 3.16)
project(lora_lite_phy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)

# ---------- GoogleTest (for unit tests) ----------
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ---------- Library: lora_utils ----------
add_library(lora_utils
    src/utils/gray.cpp
    src/utils/whitening.cpp
    src/utils/hamming.cpp
    src/utils/interleaver.cpp
    src/utils/crc.cpp
    src/utils/header_crc.cpp
)
target_include_directories(lora_utils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---------- Tests ----------
enable_testing()
include(GoogleTest)

add_executable(test_gray tests/test_gray.cpp)
target_link_libraries(test_gray PRIVATE lora_utils GTest::gtest_main)
gtest_discover_tests(test_gray)

add_executable(test_whitening tests/test_whitening.cpp)
target_link_libraries(test_whitening PRIVATE lora_utils GTest::gtest_main)
gtest_discover_tests(test_whitening)

add_executable(test_hamming tests/test_hamming.cpp)
target_link_libraries(test_hamming PRIVATE lora_utils GTest::gtest_main)
gtest_discover_tests(test_hamming)

add_executable(test_interleaver tests/test_interleaver.cpp)
target_link_libraries(test_interleaver PRIVATE lora_utils GTest::gtest_main)
gtest_discover_tests(test_interleaver)

add_executable(test_crc tests/test_crc.cpp)
target_link_libraries(test_crc PRIVATE lora_utils GTest::gtest_main)
gtest_discover_tests(test_crc)

add_executable(test_header_crc tests/test_header_crc.cpp)
target_link_libraries(test_header_crc PRIVATE lora_utils GTest::gtest_main)
gtest_discover_tests(test_header_crc)
