# tx_hex_roundtrip_test.cmake
# Encode a hex payload with lora_tx and decode with lora_replay,
# verifying payload_hex match from metadata.
# Expects: LORA_TX, LORA_REPLAY, SF, CR, BW, PAYLOAD_HEX, WORK_DIR
# Optional: SAMPLE_RATE (defaults to BW*4)

file(MAKE_DIRECTORY "${WORK_DIR}")

if(NOT DEFINED SAMPLE_RATE OR "${SAMPLE_RATE}" STREQUAL "")
    math(EXPR SAMPLE_RATE "${BW} * 4")
endif()
set(TX_IQ "${WORK_DIR}/tx.cf32")
set(META  "${WORK_DIR}/meta.json")

# Compute payload length from hex string (2 hex chars = 1 byte)
string(LENGTH "${PAYLOAD_HEX}" _hex_len)
math(EXPR PAYLOAD_LEN "${_hex_len} / 2")

# Compute LDRO
math(EXPR _two_pow_sf "1 << ${SF}")
math(EXPR _sym_dur_us "${_two_pow_sf} * 1000000 / ${BW}")
if(_sym_dur_us GREATER 16000)
    set(LDRO "true")
else()
    set(LDRO "false")
endif()

# Metadata includes payload_hex for verification
file(WRITE "${META}"
    "{\"sf\":${SF},\"bw\":${BW},\"sample_rate\":${SAMPLE_RATE},\"cr\":${CR},\"payload_len\":${PAYLOAD_LEN},\"has_crc\":true,\"implicit_header\":false,\"ldro\":${LDRO},\"preamble_len\":8,\"sync_word\":18,\"payload_hex\":\"${PAYLOAD_HEX}\"}")

# TX encode with hex payload
execute_process(
    COMMAND "${LORA_TX}"
        --sf "${SF}" --cr "${CR}" --bw "${BW}"
        --sample-rate "${SAMPLE_RATE}"
        --payload-hex "${PAYLOAD_HEX}"
        --output "${TX_IQ}"
    OUTPUT_VARIABLE _tx_out ERROR_VARIABLE _tx_err RESULT_VARIABLE _tx_rc TIMEOUT 30)
message("TX: ${_tx_out}")
if(NOT _tx_rc EQUAL 0)
    message(FATAL_ERROR "TX encode failed:\n${_tx_err}")
endif()

# RX decode (no --payload flag; uses payload_hex from metadata for verification)
execute_process(
    COMMAND "${LORA_REPLAY}"
        --iq "${TX_IQ}"
        --metadata "${META}"
    OUTPUT_VARIABLE _rx_out ERROR_VARIABLE _rx_err RESULT_VARIABLE _rx_rc TIMEOUT 60)
message("RX: ${_rx_out}")

# Check for CRC OK
string(FIND "${_rx_out}" "CRC" _crc_pos)
if(_crc_pos EQUAL -1)
    message(FATAL_ERROR "No CRC output found")
endif()
string(FIND "${_rx_out}" "MISMATCH" _mismatch_pos)
if(NOT _mismatch_pos EQUAL -1)
    message(FATAL_ERROR "CRC MISMATCH detected")
endif()
string(FIND "${_rx_out}" "OK" _ok_pos)
if(_ok_pos EQUAL -1)
    message(FATAL_ERROR "CRC OK not found")
endif()

# Check no payload_hex mismatch
string(FIND "${_rx_out}" "payload_hex mismatch" _hex_mis_pos)
if(NOT _hex_mis_pos EQUAL -1)
    message(FATAL_ERROR "payload_hex verification failed")
endif()

message("TX hex roundtrip PASSED: SF=${SF} CR=${CR} BW=${BW} payload_hex=${PAYLOAD_HEX}")
file(REMOVE "${TX_IQ}")
