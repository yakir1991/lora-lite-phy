# tx_soft_decode_test.cmake
# Encode a payload with lora_tx (with AWGN), then decode with lora_replay --soft.
# Verifies CRC OK across multiple seeds to test soft-decode reliability.
# Expects: LORA_TX, LORA_REPLAY, SF, CR, BW, PAYLOAD, SNR, SEEDS, WORK_DIR
# Optional: SAMPLE_RATE (defaults to BW*4)

file(MAKE_DIRECTORY "${WORK_DIR}")

if(NOT DEFINED SAMPLE_RATE OR "${SAMPLE_RATE}" STREQUAL "")
    set(SAMPLE_RATE_MULT 4)
    math(EXPR SAMPLE_RATE "${BW} * ${SAMPLE_RATE_MULT}")
endif()

string(LENGTH "${PAYLOAD}" PAYLOAD_LEN)

# Compute LDRO
math(EXPR _two_pow_sf "1 << ${SF}")
math(EXPR _sym_dur_us "${_two_pow_sf} * 1000000 / ${BW}")
if(_sym_dur_us GREATER 16000)
    set(LDRO "true")
else()
    set(LDRO "false")
endif()

set(META_CONTENT "{\"sf\":${SF},\"bw\":${BW},\"sample_rate\":${SAMPLE_RATE},\"cr\":${CR},\"payload_len\":${PAYLOAD_LEN},\"has_crc\":true,\"implicit_header\":false,\"ldro\":${LDRO},\"preamble_len\":8,\"sync_word\":18}")
set(META "${WORK_DIR}/meta.json")
file(WRITE "${META}" "${META_CONTENT}")

set(OK_COUNT 0)
foreach(_seed RANGE 1 ${SEEDS})
    set(TX_IQ "${WORK_DIR}/tx_seed${_seed}.cf32")

    # TX encode with AWGN
    execute_process(
        COMMAND "${LORA_TX}"
            --sf "${SF}" --cr "${CR}" --bw "${BW}"
            --sample-rate "${SAMPLE_RATE}"
            --payload "${PAYLOAD}"
            --snr "${SNR}" --seed "${_seed}"
            --output "${TX_IQ}"
        OUTPUT_QUIET
        ERROR_VARIABLE _tx_err
        RESULT_VARIABLE _tx_rc
        TIMEOUT 30
    )
    if(NOT _tx_rc EQUAL 0)
        message(FATAL_ERROR "TX encode failed (seed=${_seed}):\n${_tx_err}")
    endif()

    # RX decode with --soft
    execute_process(
        COMMAND "${LORA_REPLAY}"
            --iq "${TX_IQ}"
            --metadata "${META}"
            --payload "${PAYLOAD}"
            --soft
        OUTPUT_VARIABLE _rx_out
        ERROR_VARIABLE _rx_err
        RESULT_VARIABLE _rx_rc
        TIMEOUT 60
    )

    string(FIND "${_rx_out}" "CRC" _crc_pos)
    if(_crc_pos GREATER -1)
        string(FIND "${_rx_out}" "OK" _ok_pos)
        string(FIND "${_rx_out}" "MISMATCH" _mis_pos)
        if(_ok_pos GREATER -1 AND _mis_pos EQUAL -1)
            math(EXPR OK_COUNT "${OK_COUNT} + 1")
        endif()
    endif()

    # Clean up IQ file to save disk space
    file(REMOVE "${TX_IQ}")
endforeach()

# Require at least half of seeds to decode successfully
math(EXPR MIN_OK "${SEEDS} / 2")
if(OK_COUNT LESS MIN_OK)
    message(FATAL_ERROR "Soft decode FAILED: only ${OK_COUNT}/${SEEDS} CRC OK (need >= ${MIN_OK}). SF=${SF} CR=${CR} SNR=${SNR}")
endif()

message("Soft decode test PASSED: ${OK_COUNT}/${SEEDS} CRC OK. SF=${SF} CR=${CR} SNR=${SNR}")
