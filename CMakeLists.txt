cmake_minimum_required(VERSION 3.16)
project(lora_lite_phy LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
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

# ---------- Library: lora_phy (tx/rx loopback) ----------
add_library(lora_phy
    src/workspace.cpp
    src/tx/loopback_tx.cpp
    src/tx/frame_tx.cpp
    src/rx/demodulator.cpp
    src/rx/decimate.cpp
    src/rx/preamble.cpp
    src/rx/frame_decode.cpp
    src/rx/header_decode.cpp
    src/rx/payload_decode.cpp
    src/rx/frame.cpp
    src/rx/header.cpp
    src/rx/sync.cpp
)
target_link_libraries(lora_phy PUBLIC lora_utils liquid)
target_include_directories(lora_phy PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---------- Benchmarks ----------
add_executable(benchmarks benchmarks/tx_rx_bench.cpp)
target_link_libraries(benchmarks PRIVATE lora_phy)

# Tools
# Debug helper for investigating header auto OS4 loopback
add_executable(dbg_loopback_header_auto tools/dbg_loopback_header_auto.cpp)
target_link_libraries(dbg_loopback_header_auto PRIVATE lora_phy)

# CLI decoder tool
add_executable(lora_decode tools/lora_decode.cpp)
target_link_libraries(lora_decode PRIVATE lora_phy)

# Golden vector generator (C++) was removed; target disabled
# add_executable(create_golden_vectors tools/create_golden_vectors.cpp)
# target_link_libraries(create_golden_vectors PRIVATE lora_phy)

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

add_executable(test_sync_word tests/test_sync_word.cpp)
target_link_libraries(test_sync_word PRIVATE lora_phy GTest::gtest_main)
gtest_discover_tests(test_sync_word)

add_executable(test_loopback tests/test_loopback.cpp)
target_link_libraries(test_loopback PRIVATE lora_phy GTest::gtest_main)
gtest_discover_tests(test_loopback)
add_executable(test_reference_vectors tests/test_reference_vectors.cpp)
target_link_libraries(test_reference_vectors PRIVATE lora_phy GTest::gtest_main)
# --- Reference vectors support ---

set(VECTORS_DIR ${CMAKE_SOURCE_DIR}/vectors)


add_custom_target(export_vectors
  COMMAND ${CMAKE_COMMAND} -E env ROOT=${CMAKE_SOURCE_DIR} bash ${CMAKE_SOURCE_DIR}/scripts/export_vectors.sh
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Generating reference vectors via external/gr_lora_sdr flowgraph"
)


add_custom_target(mark_export_exec
  COMMAND ${CMAKE_COMMAND} -E env chmod +x ${CMAKE_SOURCE_DIR}/scripts/export_vectors.sh
)
add_dependencies(export_vectors mark_export_exec)


add_custom_target(copy_vectors ALL
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/vectors
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${VECTORS_DIR} ${CMAKE_BINARY_DIR}/vectors
  COMMENT "Copying vectors/ -> build/"
)


gtest_discover_tests(test_reference_vectors
  TEST_PREFIX "reference."
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  DISCOVERY_TIMEOUT 60
)


add_executable(test_awgn_sweep tests/awgn_sweep.cpp)
target_link_libraries(test_awgn_sweep PRIVATE lora_phy GTest::gtest)
gtest_discover_tests(
  test_awgn_sweep
  TEST_PREFIX ""
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  PROPERTIES LABELS "slow"
)



add_executable(test_header_end_to_end tests/test_header_end_to_end.cpp)
target_link_libraries(test_header_end_to_end PRIVATE lora_phy GTest::gtest_main)
gtest_discover_tests(test_header_end_to_end)
add_executable(test_header_parse tests/test_header_parse.cpp)
target_link_libraries(test_header_parse PRIVATE lora_phy GTest::gtest_main)
gtest_discover_tests(test_header_parse)
add_executable(test_preamble tests/test_preamble.cpp)
target_link_libraries(test_preamble PRIVATE lora_phy GTest::gtest_main)
gtest_discover_tests(test_preamble)
add_executable(test_reference_vectors_os tests/test_reference_vectors_os.cpp)
target_link_libraries(test_reference_vectors_os PRIVATE lora_phy GTest::gtest_main)
gtest_discover_tests(test_reference_vectors_os
  TEST_PREFIX "reference_os."
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  DISCOVERY_TIMEOUT 60
)

add_executable(test_reference_vectors_header_os tests/test_reference_vectors_header_os.cpp)
target_link_libraries(test_reference_vectors_header_os PRIVATE lora_phy GTest::gtest_main)
gtest_discover_tests(test_reference_vectors_header_os
  TEST_PREFIX "reference_os_hdr."
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  DISCOVERY_TIMEOUT 60
)

# ---------- CLI tests (smoke) ----------
add_test(NAME cli.json.os1
  COMMAND bash -lc "${CMAKE_BINARY_DIR}/gen_frame_vectors --sf 7 --cr 45 --payload ${CMAKE_SOURCE_DIR}/vectors/sf7_cr45_payload.bin --out ${CMAKE_BINARY_DIR}/cli_os1.bin --os 1 --preamble 8 && ${CMAKE_BINARY_DIR}/lora_decode --in ${CMAKE_BINARY_DIR}/cli_os1.bin --sf 7 --cr 45 --format f32 --json"
)
set_tests_properties(cli.json.os1 PROPERTIES PASS_REGULAR_EXPRESSION "\"success\": true")


add_test(NAME cli.json.os1.partial
  COMMAND bash -lc "${CMAKE_BINARY_DIR}/gen_frame_vectors --sf 7 --cr 45 --payload ${CMAKE_SOURCE_DIR}/vectors/sf7_cr45_payload.bin --out ${CMAKE_BINARY_DIR}/cli_os1_hdr.bin --os 1 --preamble 8 && python3 ${CMAKE_SOURCE_DIR}/scripts/corrupt_tail.py --in ${CMAKE_BINARY_DIR}/cli_os1_hdr.bin --out ${CMAKE_BINARY_DIR}/cli_os1_hdr_corrupt.bin --tail-floats 256 && ${CMAKE_BINARY_DIR}/lora_decode --in ${CMAKE_BINARY_DIR}/cli_os1_hdr_corrupt.bin --sf 7 --cr 45 --format f32 --json --allow-partial"
)
set_tests_properties(cli.json.os1.partial PROPERTIES PASS_REGULAR_EXPRESSION ".*\"success\": false.*\"reason\": \"(payload_crc_failed|fec_decode_failed)\".*")

# CLI OS4 partial decode (induce CRC failure and verify JSON reason)
add_test(NAME cli.json.os4.partial
  COMMAND bash -lc "${CMAKE_BINARY_DIR}/gen_frame_vectors --sf 7 --cr 45 --payload ${CMAKE_SOURCE_DIR}/vectors/sf7_cr45_payload.bin --out ${CMAKE_BINARY_DIR}/cli_os4_hdr.bin --os 4 --preamble 8 && python3 ${CMAKE_SOURCE_DIR}/scripts/corrupt_tail.py --in ${CMAKE_BINARY_DIR}/cli_os4_hdr.bin --out ${CMAKE_BINARY_DIR}/cli_os4_hdr_corrupt.bin --tail-floats 512 && ${CMAKE_BINARY_DIR}/lora_decode --in ${CMAKE_BINARY_DIR}/cli_os4_hdr_corrupt.bin --sf 7 --cr 45 --format f32 --json --allow-partial --sync auto"
)
set_tests_properties(cli.json.os4.partial PROPERTIES PASS_REGULAR_EXPRESSION ".*\"success\": false.*\"reason\": \"(payload_crc_failed|fec_decode_failed)\".*")
add_executable(test_reference_vectors_header_os_varlen tests/test_reference_vectors_header_os_varlen.cpp)
target_link_libraries(test_reference_vectors_header_os_varlen PRIVATE lora_phy GTest::gtest_main)

# Tool: gen_frame_vectors
add_executable(gen_frame_vectors tools/gen_frame_vectors.cpp)
target_link_libraries(gen_frame_vectors PRIVATE lora_phy)
gtest_discover_tests(test_reference_vectors_header_os_varlen
  TEST_PREFIX "reference_os_hdr_len."
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  DISCOVERY_TIMEOUT 60
)

# Cross-validate full (limited reps by default)
add_test(NAME cross.validate.full
  COMMAND bash -lc "CROSS_REPS=1 python3 ${CMAKE_SOURCE_DIR}/scripts/cross_validate_benchmark.py --root ${CMAKE_SOURCE_DIR} --build ${CMAKE_BINARY_DIR}"
)
set_tests_properties(cross.validate.full PROPERTIES LABELS perf)
