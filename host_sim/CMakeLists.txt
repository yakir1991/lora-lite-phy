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
    src/whitening.cpp
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
endif ()
