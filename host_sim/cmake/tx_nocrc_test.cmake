# tx_nocrc_test.cmake
# Encode a payload with --no-crc and decode, verifying payload match.
# Expects: LORA_TX, LORA_REPLAY, SF, CR, BW, PAYLOAD, WORK_DIR

file(MAKE_DIRECTORY "${WORK_DIR}")

if(NOT DEFINED SAMPLE_RATE OR "${SAMPLE_RATE}" STREQUAL "")
    math(EXPR SAMPLE_RATE "${BW} * 4")
endif()

string(LENGTH "${PAYLOAD}" PAYLOAD_LEN)

math(EXPR _two_pow_sf "1 << ${SF}")
math(EXPR _sym_dur_us "${_two_pow_sf} * 1000000 / ${BW}")
if(_sym_dur_us GREATER 16000)
    set(LDRO "true")
else()
    set(LDRO "false")
endif()

set(TX_IQ "${WORK_DIR}/tx.cf32")
set(META  "${WORK_DIR}/meta.json")

file(WRITE "${META}"
    "{\"sf\":${SF},\"bw\":${BW},\"sample_rate\":${SAMPLE_RATE},\"cr\":${CR},\"payload_len\":${PAYLOAD_LEN},\"has_crc\":false,\"implicit_header\":false,\"ldro\":${LDRO},\"preamble_len\":8,\"sync_word\":18}")

# TX encode without CRC
execute_process(
    COMMAND "${LORA_TX}"
        --sf "${SF}" --cr "${CR}" --bw "${BW}"
        --sample-rate "${SAMPLE_RATE}"
        --payload "${PAYLOAD}"
        --no-crc
        --output "${TX_IQ}"
    OUTPUT_VARIABLE _tx_out ERROR_VARIABLE _tx_err RESULT_VARIABLE _tx_rc TIMEOUT 30)
if(NOT _tx_rc EQUAL 0)
    message(FATAL_ERROR "TX encode failed:\n${_tx_err}")
endif()
message("TX: ${_tx_out}")

# RX decode
execute_process(
    COMMAND "${LORA_REPLAY}"
        --iq "${TX_IQ}"
        --metadata "${META}"
        --payload "${PAYLOAD}"
    OUTPUT_VARIABLE _rx_out ERROR_VARIABLE _rx_err RESULT_VARIABLE _rx_rc TIMEOUT 60)
message("RX: ${_rx_out}")

# Check payload match (no CRC field, so we check ASCII output)
string(FIND "${_rx_out}" "Payload ASCII: ${PAYLOAD}" _match_pos)
if(_match_pos EQUAL -1)
    message(FATAL_ERROR "No-CRC decode FAILED: payload mismatch\nExpected: ${PAYLOAD}\nOutput: ${_rx_out}")
endif()

message("TX_NOCRC_OK: payload='${PAYLOAD}' decoded correctly without CRC")

file(REMOVE "${TX_IQ}")
