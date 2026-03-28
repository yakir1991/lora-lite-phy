cmake_minimum_required(VERSION 3.16)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(host_sim_core STATIC
    src/capture.cpp
    src/chirp.cpp
    src/alignment.cpp
    src/deinterleaver.cpp
    src/fft_demod.cpp
    src/fft_demod_q15.cpp
    src/fft_demod_ref.cpp
    src/hamming.cpp
    src/scheduler.cpp
    src/lora_params.cpp
    src/soft_decode.cpp
    src/whitening.cpp
    src/lora_replay_header_encoder.cpp
    src/lora_replay_stage_processing.cpp
    third_party/kissfft/kiss_fft.c
)

add_library(host_sim ALIAS host_sim_core)

target_include_directories(host_sim_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/kissfft
)

add_executable(lora_replay
    src/lora_replay.cpp
)

target_link_libraries(lora_replay
    PRIVATE
        host_sim_core
)

add_executable(lora_tx
    src/lora_tx.cpp
)

target_link_libraries(lora_tx
    PRIVATE
        host_sim_core
)

if (BUILD_TESTING)
    set(LORA_REFERENCE_DATA_DIR "${PROJECT_SOURCE_DIR}/gr_lora_sdr/data/generated")

    function(add_lora_compare_test name basename)
        add_test(
            NAME ${name}
            COMMAND lora_replay
                --iq "${LORA_REFERENCE_DATA_DIR}/${basename}.cf32"
                --compare-root "${LORA_REFERENCE_DATA_DIR}/${basename}.cf32"
        )
        set_tests_properties(${name} PROPERTIES LABELS "host-sim")
    endfunction()

    add_lora_compare_test(lora_replay_sf7_cr1_short tx_rx_sf7_bw125000_cr1_snrm5p0_short)
    add_lora_compare_test(lora_replay_sf7_cr2_short tx_rx_sf7_bw125000_cr2_snrm5p0_short)
    add_lora_compare_test(lora_replay_sf9_cr3_short tx_rx_sf9_bw125000_cr3_snrm2p0_short)

    add_lora_compare_test(lora_replay_sf5_cr1 tx_rx_sf5_bw125000_cr1_snrm5p0)
    add_lora_compare_test(lora_replay_sf5_cr2 tx_rx_sf5_bw125000_cr2_snrm5p0)
    add_lora_compare_test(lora_replay_sf6_cr1 tx_rx_sf6_bw125000_cr1_snrm5p0)
    add_lora_compare_test(lora_replay_sf6_cr2 tx_rx_sf6_bw125000_cr2_snrm5p0)
    add_lora_compare_test(lora_replay_sf7_cr1_full tx_rx_sf7_bw125000_cr1_snrm5p0)
    add_lora_compare_test(lora_replay_sf7_cr2_full tx_rx_sf7_bw125000_cr2_snrm5p0)
    add_lora_compare_test(lora_replay_sf9_cr3_full tx_rx_sf9_bw125000_cr3_snrm2p0)
    add_lora_compare_test(lora_replay_sf10_cr4_full tx_rx_sf10_bw250000_cr4_snrp0p0)

    add_test(
        NAME host_sim_verify_manifest
        COMMAND python3 "${PROJECT_SOURCE_DIR}/tools/check_reference_manifest.py"
                "${PROJECT_SOURCE_DIR}/docs/reference_stage_manifest.json"
                --data-dir "${PROJECT_SOURCE_DIR}/gr_lora_sdr/data/generated"
    )
    set_tests_properties(host_sim_verify_manifest PROPERTIES LABELS "host-sim")

    add_custom_target(host-sim
        COMMAND ${CMAKE_CTEST_COMMAND} -L host-sim
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Run host_sim test suite (label host-sim)")

    add_custom_target(host-sim-regression
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -L host-sim
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Run host_sim regression suite (GNU Radio parity captures)")

    add_executable(host_sim_scheduler_smoke
        tests/test_scheduler_golden.cpp
    )
    target_link_libraries(host_sim_scheduler_smoke
        PRIVATE host_sim_core
    )
    target_compile_definitions(host_sim_scheduler_smoke
        PRIVATE PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    add_test(
        NAME host_sim_scheduler_smoke
        COMMAND host_sim_scheduler_smoke
    )
    set_tests_properties(host_sim_scheduler_smoke PROPERTIES LABELS "host-sim")

    add_executable(host_sim_numeric_traits
        tests/test_numeric_traits.cpp
    )
    target_link_libraries(host_sim_numeric_traits
        PRIVATE host_sim_core
    )
    add_test(
        NAME host_sim_numeric_traits
        COMMAND host_sim_numeric_traits
    )
    set_tests_properties(host_sim_numeric_traits PROPERTIES LABELS "host-sim")

    add_executable(host_sim_numeric_modes
        tests/test_numeric_modes.cpp
    )
    target_link_libraries(host_sim_numeric_modes
        PRIVATE host_sim_core
    )
    target_compile_definitions(host_sim_numeric_modes
        PRIVATE PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    add_test(
        NAME host_sim_numeric_modes
        COMMAND host_sim_numeric_modes
    )
    set_tests_properties(host_sim_numeric_modes PROPERTIES LABELS "host-sim")

    add_test(
        NAME host_sim_summary_metrics
        COMMAND python3 ${PROJECT_SOURCE_DIR}/tools/run_summary_metrics_check.py
                ${PROJECT_SOURCE_DIR}/docs/reference_stage_manifest.json
                ${PROJECT_SOURCE_DIR}/gr_lora_sdr/data/generated
                ${CMAKE_BINARY_DIR}/summary
                ${PROJECT_SOURCE_DIR}/docs/reference_summary_metrics.json
                --lora-replay ${CMAKE_CURRENT_BINARY_DIR}/lora_replay
    )
    set_tests_properties(host_sim_summary_metrics PROPERTIES LABELS "host-sim")

    # ----- OTA regression tests (offline decode of real-world captures) -----
    set(OTA_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/data/ota")
    set(OTA_METADATA "${OTA_DATA_DIR}/metadata.json")

    set(OTA_PAYLOADS "LoRa Test #520" "LoRa Test #521" "LoRa Test #522" "LoRa Test #523")
    foreach(pkt_idx RANGE 0 3)
        list(GET OTA_PAYLOADS ${pkt_idx} expected_payload)
        set(test_name "ota_rfm95_packet_${pkt_idx}")
        add_test(
            NAME ${test_name}
            COMMAND lora_replay
                --iq "${OTA_DATA_DIR}/packet_${pkt_idx}.cf32"
                --metadata "${OTA_METADATA}"
                --payload "${expected_payload}"
        )
        # OTA captures have reference-demod mismatches (expected with real noise)
        # so we match on payload + CRC instead of exit code.
        set_tests_properties(${test_name} PROPERTIES
            LABELS "ota"
            PASS_REGULAR_EXPRESSION "Payload ASCII: ${expected_payload}.*CRC.*OK"
        )
    endforeach()

    # ----- Golden OTA captures (param-sweep archive) -----
    # Each entry: test_suffix  capture_file  metadata_file  expected_payload
    set(OTA_GOLDEN_TESTS
        "sf6_bw125_cr5"   "sf6_bw125_cr5.cf32"   "sf6_bw125_cr5_meta.json"   "LoRa Test #0"
        "sf6_bw250_cr5"   "sf6_bw250_cr5.cf32"   "sf6_bw250_cr5_meta.json"   "LoRa Test #20"
        "sf6_bw500_cr5"   "sf6_bw500_cr5.cf32"   "sf6_bw500_cr5_meta.json"   "LoRa Test #70"
        "sf7_bw125_cr5"   "sf7_bw125_cr5.cf32"   "sf7_bw125_cr5_meta.json"   "LoRa Test #12"
        "sf7_bw250_cr5"   "sf7_bw250_cr5.cf32"   "sf7_bw250_cr5_meta.json"   "LoRa Test #2"
        "sf7_bw125_cr6"   "sf7_bw125_cr6.cf32"    "sf7_bw125_cr6_meta.json"   "LoRa Test #5"
        "sf7_bw125_cr7"   "sf7_bw125_cr7.cf32"    "sf7_bw125_cr7_meta.json"   "LoRa Test #6"
        "sf7_bw125_cr8"   "sf7_bw125_cr8.cf32"    "sf7_bw125_cr8_meta.json"   "LoRa Test #0"
        "sf7_bw500_cr5"   "sf7_bw500_cr5.cf32"    "sf7_bw500_cr5_meta.json"   "LoRa Test #113"
        "sf8_bw125_cr5"   "sf8_bw125_cr5.cf32"    "sf8_bw125_cr5_meta.json"   "LoRa Test #4"
        "sf8_bw125_cr6"   "sf8_bw125_cr6.cf32"    "sf8_bw125_cr6_meta.json"   "LoRa Test #8"
        "sf8_bw125_cr7"   "sf8_bw125_cr7.cf32"    "sf8_bw125_cr7_meta.json"   "LoRa Test #8"
        "sf8_bw125_cr8"   "sf8_bw125_cr8.cf32"    "sf8_bw125_cr8_meta.json"   "LoRa Test #3"
        "sf8_bw250_cr5"   "sf8_bw250_cr5.cf32"    "sf8_bw250_cr5_meta.json"   "LoRa Test #15"
        "sf8_bw500_cr5"   "sf8_bw500_cr5.cf32"    "sf8_bw500_cr5_meta.json"   "LoRa Test #9"
        "sf9_bw125_cr5"   "sf9_bw125_cr5.cf32"    "sf9_bw125_cr5_meta.json"   "LoRa Test #5"
        "sf9_bw125_cr6"   "sf9_bw125_cr6.cf32"    "sf9_bw125_cr6_meta.json"   "LoRa Test #3"
        "sf9_bw125_cr7"   "sf9_bw125_cr7.cf32"    "sf9_bw125_cr7_meta.json"   "LoRa Test #3"
        "sf9_bw125_cr8"   "sf9_bw125_cr8.cf32"    "sf9_bw125_cr8_meta.json"   "LoRa Test #3"
        "sf9_bw250_cr5"   "sf9_bw250_cr5.cf32"    "sf9_bw250_cr5_meta.json"   "LoRa Test #21"
        "sf9_bw500_cr5"   "sf9_bw500_cr5.cf32"    "sf9_bw500_cr5_meta.json"   "LoRa Test #17"
        "sf10_bw125_cr5"  "sf10_bw125_cr5.cf32"   "sf10_bw125_cr5_meta.json"  "LoRa Test #74"
        "sf10_bw125_cr6"  "sf10_bw125_cr6.cf32"   "sf10_bw125_cr6_meta.json"  "LoRa Test #3"
        "sf10_bw125_cr7"  "sf10_bw125_cr7.cf32"   "sf10_bw125_cr7_meta.json"  "LoRa Test #3"
        "sf10_bw125_cr8"  "sf10_bw125_cr8.cf32"   "sf10_bw125_cr8_meta.json"  "LoRa Test #3"
        "sf10_bw250_cr5"  "sf10_bw250_cr5.cf32"   "sf10_bw250_cr5_meta.json"  "LoRa Test #13"
        "sf10_bw500_cr5"  "sf10_bw500_cr5.cf32"   "sf10_bw500_cr5_meta.json"  "LoRa Test #15"
        "sf11_bw125_cr5"  "sf11_bw125_cr5.cf32"   "sf11_bw125_cr5_meta.json"  "LoRa Test #77"
        "sf11_bw125_cr6"  "sf11_bw125_cr6.cf32"   "sf11_bw125_cr6_meta.json"  "LoRa Test #3"
        "sf11_bw125_cr7"  "sf11_bw125_cr7.cf32"   "sf11_bw125_cr7_meta.json"  "LoRa Test #3"
        "sf11_bw125_cr8"  "sf11_bw125_cr8.cf32"   "sf11_bw125_cr8_meta.json"  "LoRa Test #3"
        "sf11_bw250_cr5"  "sf11_bw250_cr5.cf32"   "sf11_bw250_cr5_meta.json"  "LoRa Test #27"
        "sf11_bw500_cr5"  "sf11_bw500_cr5.cf32"   "sf11_bw500_cr5_meta.json"  "LoRa Test #37"
        "sf12_bw125_cr5"  "sf12_bw125_cr5.cf32"   "sf12_bw125_cr5_meta.json"  "LoRa Test #79"
        "sf12_bw125_cr6"  "sf12_bw125_cr6.cf32"   "sf12_bw125_cr6_meta.json"  "LoRa Test #3"
        "sf12_bw125_cr7"  "sf12_bw125_cr7.cf32"   "sf12_bw125_cr7_meta.json"  "LoRa Test #3"
        "sf12_bw125_cr8"  "sf12_bw125_cr8.cf32"   "sf12_bw125_cr8_meta.json"  "LoRa Test #3"
        "sf12_bw250_cr5"  "sf12_bw250_cr5.cf32"   "sf12_bw250_cr5_meta.json"  "LoRa Test #23"
        "sf12_bw500_cr5"  "sf12_bw500_cr5.cf32"   "sf12_bw500_cr5_meta.json"  "LoRa Test #46"
    )
    list(LENGTH OTA_GOLDEN_TESTS _golden_len)
    math(EXPR _golden_count "${_golden_len} / 4")
    math(EXPR _golden_last "${_golden_count} - 1")
    foreach(_i RANGE 0 ${_golden_last})
        math(EXPR _off "${_i} * 4")
        math(EXPR _off1 "${_off} + 1")
        math(EXPR _off2 "${_off} + 2")
        math(EXPR _off3 "${_off} + 3")
        list(GET OTA_GOLDEN_TESTS ${_off}  _suffix)
        list(GET OTA_GOLDEN_TESTS ${_off1} _capture)
        list(GET OTA_GOLDEN_TESTS ${_off2} _meta)
        list(GET OTA_GOLDEN_TESTS ${_off3} _payload)
        set(_test_name "ota_golden_${_suffix}")
        add_test(
            NAME ${_test_name}
            COMMAND lora_replay
                --iq "${OTA_DATA_DIR}/${_capture}"
                --metadata "${OTA_DATA_DIR}/${_meta}"
                --payload "${_payload}"
        )
        set_tests_properties(${_test_name} PROPERTIES
            LABELS "ota"
            PASS_REGULAR_EXPRESSION "Payload ASCII: ${_payload}.*CRC.*OK"
        )
    endforeach()

    # ----- Multi-packet streaming test -----
    # Concatenate two copies of the SF7/BW125/CR5 capture with a noise gap
    # and verify --multi decodes both packets.
    set(_multi_iq "${CMAKE_CURRENT_BINARY_DIR}/multi_sf7_test.cf32")
    set(_src_iq "${OTA_DATA_DIR}/sf7_bw125_cr5.cf32")
    add_test(
        NAME ota_multi_packet_sf7
        COMMAND ${CMAKE_COMMAND}
            -DLORA_REPLAY=$<TARGET_FILE:lora_replay>
            -DSRC_IQ=${_src_iq}
            -DMULTI_IQ=${_multi_iq}
            -DMETA=${OTA_DATA_DIR}/sf7_bw125_cr5_meta.json
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/multi_packet_test.cmake
    )
    set_tests_properties(ota_multi_packet_sf7 PROPERTIES
        LABELS "ota"
        PASS_REGULAR_EXPRESSION "MULTI_PACKET_OK"
    )
endif ()

# ===== TX round-trip tests (encode → decode) =====
set(TX_ROUNDTRIP_TESTS
    # suffix      SF  CR  BW      payload
    "sf7_cr1"      7   1  125000  "Hello LoRa"
    "sf8_cr2"      8   2  125000  "SF8 test"
    "sf9_cr3"      9   3  125000  "SF9"
    "sf10_cr4"    10   4  125000  "SF10 long payload test!"
    "sf12_cr1"    12   1  125000  "SF12"
    "sf7_bw250"    7   1  250000  "BW250"
    "sf7_bw500"    7   1  500000  "BW500k test"
)
list(LENGTH TX_ROUNDTRIP_TESTS _tx_total)
math(EXPR _tx_count "${_tx_total} / 5")
foreach(_i RANGE 0 ${_tx_count})
    if(_i EQUAL ${_tx_count})
        break()
    endif()
    math(EXPR _off  "${_i} * 5")
    math(EXPR _off1 "${_off} + 1")
    math(EXPR _off2 "${_off} + 2")
    math(EXPR _off3 "${_off} + 3")
    math(EXPR _off4 "${_off} + 4")
    list(GET TX_ROUNDTRIP_TESTS ${_off}  _suffix)
    list(GET TX_ROUNDTRIP_TESTS ${_off1} _sf)
    list(GET TX_ROUNDTRIP_TESTS ${_off2} _cr)
    list(GET TX_ROUNDTRIP_TESTS ${_off3} _bw)
    list(GET TX_ROUNDTRIP_TESTS ${_off4} _payload)
    set(_test_name "tx_roundtrip_${_suffix}")
    add_test(
        NAME ${_test_name}
        COMMAND ${CMAKE_COMMAND}
            -DLORA_TX=$<TARGET_FILE:lora_tx>
            -DLORA_REPLAY=$<TARGET_FILE:lora_replay>
            -DSF=${_sf}
            -DCR=${_cr}
            -DBW=${_bw}
            -DPAYLOAD=${_payload}
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/tx_roundtrip_${_suffix}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/tx_roundtrip_test.cmake
    )
    set_tests_properties(${_test_name} PROPERTIES
        LABELS "tx"
    )
endforeach()

# ----- TX round-trip tests at os=1 (Fs = BW) -----
set(TX_ROUNDTRIP_OS1_TESTS
    # suffix          SF  CR  BW      payload
    "sf7_cr1_os1"      7   1  125000  "os1 test!"
    "sf9_cr3_os1"      9   3  125000  "SF9 os1"
    "sf12_cr1_os1"    12   1  125000  "sf12"
)
list(LENGTH TX_ROUNDTRIP_OS1_TESTS _tx_os1_total)
math(EXPR _tx_os1_count "${_tx_os1_total} / 5")
foreach(_i RANGE 0 ${_tx_os1_count})
    if(_i EQUAL ${_tx_os1_count})
        break()
    endif()
    math(EXPR _off  "${_i} * 5")
    math(EXPR _off1 "${_off} + 1")
    math(EXPR _off2 "${_off} + 2")
    math(EXPR _off3 "${_off} + 3")
    math(EXPR _off4 "${_off} + 4")
    list(GET TX_ROUNDTRIP_OS1_TESTS ${_off}  _suffix)
    list(GET TX_ROUNDTRIP_OS1_TESTS ${_off1} _sf)
    list(GET TX_ROUNDTRIP_OS1_TESTS ${_off2} _cr)
    list(GET TX_ROUNDTRIP_OS1_TESTS ${_off3} _bw)
    list(GET TX_ROUNDTRIP_OS1_TESTS ${_off4} _payload)
    set(_test_name "tx_roundtrip_${_suffix}")
    add_test(
        NAME ${_test_name}
        COMMAND ${CMAKE_COMMAND}
            -DLORA_TX=$<TARGET_FILE:lora_tx>
            -DLORA_REPLAY=$<TARGET_FILE:lora_replay>
            -DSF=${_sf}
            -DCR=${_cr}
            -DBW=${_bw}
            -DSAMPLE_RATE=${_bw}
            -DPAYLOAD=${_payload}
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/tx_roundtrip_${_suffix}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/tx_roundtrip_test.cmake
    )
    set_tests_properties(${_test_name} PROPERTIES
        LABELS "tx"
    )
endforeach()

# ----- Soft-decode AWGN tests -----
# These run lora_tx with AWGN, then lora_replay --soft, requiring >= 50% CRC OK.
# SNR chosen near the knee of the PER curve where soft > hard.
set(TX_SOFT_DECODE_TESTS
    # suffix               SF  CR  BW      payload           SNR  SEEDS
    "soft_sf7_cr1_snr-9"    7   1  125000  "Hello soft"      -9   20
    "soft_sf7_cr4_snr-10"   7   4  125000  "Hello soft"      -10  20
)
list(LENGTH TX_SOFT_DECODE_TESTS _soft_total)
math(EXPR _soft_count "${_soft_total} / 7")
foreach(_i RANGE 0 ${_soft_count})
    if(_i EQUAL ${_soft_count})
        break()
    endif()
    math(EXPR _off  "${_i} * 7")
    math(EXPR _off1 "${_off} + 1")
    math(EXPR _off2 "${_off} + 2")
    math(EXPR _off3 "${_off} + 3")
    math(EXPR _off4 "${_off} + 4")
    math(EXPR _off5 "${_off} + 5")
    math(EXPR _off6 "${_off} + 6")
    list(GET TX_SOFT_DECODE_TESTS ${_off}  _suffix)
    list(GET TX_SOFT_DECODE_TESTS ${_off1} _sf)
    list(GET TX_SOFT_DECODE_TESTS ${_off2} _cr)
    list(GET TX_SOFT_DECODE_TESTS ${_off3} _bw)
    list(GET TX_SOFT_DECODE_TESTS ${_off4} _payload)
    list(GET TX_SOFT_DECODE_TESTS ${_off5} _snr)
    list(GET TX_SOFT_DECODE_TESTS ${_off6} _seeds)
    set(_test_name "tx_${_suffix}")
    add_test(
        NAME ${_test_name}
        COMMAND ${CMAKE_COMMAND}
            -DLORA_TX=$<TARGET_FILE:lora_tx>
            -DLORA_REPLAY=$<TARGET_FILE:lora_replay>
            -DSF=${_sf}
            -DCR=${_cr}
            -DBW=${_bw}
            -DPAYLOAD=${_payload}
            -DSNR=${_snr}
            -DSEEDS=${_seeds}
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/tx_${_suffix}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/tx_soft_decode_test.cmake
    )
    set_tests_properties(${_test_name} PROPERTIES
        LABELS "tx;soft"
    )
endforeach()

# ===== TX impairment tests (CFO, SFO, combined AWGN+CFO) =====
# Each entry: suffix  SF  CR  BW  payload  SEEDS  MIN_OK_RATIO  SNR  CFO  SFO  SAMPLE_RATE
set(TX_IMPAIRMENT_TESTS
    # Pure CFO (OS=1 — realistic for dedicated decoder)
    "cfo_5k"          7  1  125000  "Hello"  5  "1.0"  ""   5000   ""  125000
    "cfo_20k"         7  1  125000  "Hello"  5  "1.0"  ""  20000   ""  125000
    "cfo_40k"         7  1  125000  "Hello"  5  "1.0"  ""  40000   ""  125000
    # Pure SFO
    "sfo_10ppm"       7  1  125000  "Hello"  5  "1.0"  ""   ""     10  125000
    "sfo_20ppm"       7  1  125000  "Hello"  5  "1.0"  ""   ""     20  125000
    # Combined CFO + AWGN
    "cfo10k_snr-5"    7  1  125000  "Hello"  10  "0.8"  -5  10000  ""  125000
    "cfo20k_snr-5"    7  1  125000  "Hello"  10  "0.5"  -5  20000  ""  125000
    # Combined CFO + SFO
    "cfo3k_sfo10"     7  1  125000  "Hello"  5  "1.0"  ""   3000   10  125000
    # Triple: AWGN + CFO + SFO
    "awgn_cfo_sfo"    7  1  125000  "Hello"  10  "0.5"  -5   3000  10  125000
    # Higher SF with impairments
    "sf12_cfo20k"    12  1  125000  "Hello"  3  "1.0"  ""  20000   ""  125000
    "sf9_cfo10k_snr-8" 9  1  125000  "Hello"  10  "0.5"  -8  10000  ""  125000
)
list(LENGTH TX_IMPAIRMENT_TESTS _imp_total)
math(EXPR _imp_count "${_imp_total} / 11")
foreach(_i RANGE 0 ${_imp_count})
    if(_i EQUAL ${_imp_count})
        break()
    endif()
    math(EXPR _off  "${_i} * 11")
    math(EXPR _off1 "${_off} + 1")
    math(EXPR _off2 "${_off} + 2")
    math(EXPR _off3 "${_off} + 3")
    math(EXPR _off4 "${_off} + 4")
    math(EXPR _off5 "${_off} + 5")
    math(EXPR _off6 "${_off} + 6")
    math(EXPR _off7 "${_off} + 7")
    math(EXPR _off8 "${_off} + 8")
    math(EXPR _off9 "${_off} + 9")
    math(EXPR _off10 "${_off} + 10")
    list(GET TX_IMPAIRMENT_TESTS ${_off}  _suffix)
    list(GET TX_IMPAIRMENT_TESTS ${_off1} _sf)
    list(GET TX_IMPAIRMENT_TESTS ${_off2} _cr)
    list(GET TX_IMPAIRMENT_TESTS ${_off3} _bw)
    list(GET TX_IMPAIRMENT_TESTS ${_off4} _payload)
    list(GET TX_IMPAIRMENT_TESTS ${_off5} _seeds)
    list(GET TX_IMPAIRMENT_TESTS ${_off6} _min_ok)
    list(GET TX_IMPAIRMENT_TESTS ${_off7} _snr)
    list(GET TX_IMPAIRMENT_TESTS ${_off8} _cfo)
    list(GET TX_IMPAIRMENT_TESTS ${_off9} _sfo)
    list(GET TX_IMPAIRMENT_TESTS ${_off10} _sr)
    set(_test_name "tx_impair_${_suffix}")
    add_test(
        NAME ${_test_name}
        COMMAND ${CMAKE_COMMAND}
            -DLORA_TX=$<TARGET_FILE:lora_tx>
            -DLORA_REPLAY=$<TARGET_FILE:lora_replay>
            -DSF=${_sf}
            -DCR=${_cr}
            -DBW=${_bw}
            -DSAMPLE_RATE=${_sr}
            -DPAYLOAD=${_payload}
            -DSEEDS=${_seeds}
            -DMIN_OK_RATIO=${_min_ok}
            -DSNR=${_snr}
            -DCFO=${_cfo}
            -DSFO=${_sfo}
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/tx_impair_${_suffix}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/tx_impairment_test.cmake
    )
    set_tests_properties(${_test_name} PROPERTIES
        LABELS "tx;impairment"
    )
endforeach()
