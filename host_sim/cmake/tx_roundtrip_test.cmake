# tx_roundtrip_test.cmake
# Encode a payload with lora_tx and decode with lora_replay, verifying CRC OK.
# Expects: LORA_TX, LORA_REPLAY, SF, CR, BW, PAYLOAD, WORK_DIR
# Optional: SAMPLE_RATE (defaults to BW*4)

file(MAKE_DIRECTORY "${WORK_DIR}")

if(NOT DEFINED SAMPLE_RATE OR "${SAMPLE_RATE}" STREQUAL "")
    set(SAMPLE_RATE_MULT 4)
    math(EXPR SAMPLE_RATE "${BW} * ${SAMPLE_RATE_MULT}")
endif()
set(TX_IQ "${WORK_DIR}/tx.cf32")
set(META  "${WORK_DIR}/meta.json")

string(LENGTH "${PAYLOAD}" PAYLOAD_LEN)

# Generate metadata JSON
# Compute LDRO: symbol_duration = 2^SF / BW * 1000 ms; LDRO if > 16ms
math(EXPR _two_pow_sf "1 << ${SF}")
math(EXPR _sym_dur_us "${_two_pow_sf} * 1000000 / ${BW}")
if(_sym_dur_us GREATER 16000)
    set(LDRO "true")
else()
    set(LDRO "false")
endif()
file(WRITE "${META}"
    "{\"sf\":${SF},\"bw\":${BW},\"sample_rate\":${SAMPLE_RATE},\"cr\":${CR},\"payload_len\":${PAYLOAD_LEN},\"has_crc\":true,\"implicit_header\":false,\"ldro\":${LDRO},\"preamble_len\":8,\"sync_word\":18}")

# TX encode
execute_process(
    COMMAND "${LORA_TX}"
        --sf "${SF}" --cr "${CR}" --bw "${BW}"
        --sample-rate "${SAMPLE_RATE}"
        --payload "${PAYLOAD}"
        --output "${TX_IQ}"
    OUTPUT_VARIABLE _tx_out
    ERROR_VARIABLE  _tx_err
    RESULT_VARIABLE _tx_rc
    TIMEOUT 30
)
message("TX: ${_tx_out}")
if(NOT _tx_rc EQUAL 0)
    message(FATAL_ERROR "TX encode failed:\n${_tx_err}")
endif()

# RX decode
execute_process(
    COMMAND "${LORA_REPLAY}"
        --iq "${TX_IQ}"
        --metadata "${META}"
        --payload "${PAYLOAD}"
    OUTPUT_VARIABLE _rx_out
    ERROR_VARIABLE  _rx_err
    RESULT_VARIABLE _rx_rc
    TIMEOUT 60
)
message("RX: ${_rx_out}")

# Check for CRC OK
string(FIND "${_rx_out}" "CRC" _crc_pos)
if(_crc_pos EQUAL -1)
    message(FATAL_ERROR "No CRC output found in decoder output")
endif()

string(FIND "${_rx_out}" "MISMATCH" _mismatch_pos)
if(NOT _mismatch_pos EQUAL -1)
    message(FATAL_ERROR "CRC MISMATCH detected in TX round-trip test")
endif()

string(FIND "${_rx_out}" "OK" _ok_pos)
if(_ok_pos EQUAL -1)
    message(FATAL_ERROR "CRC OK not found in decoder output")
endif()

message("TX round-trip test PASSED: SF=${SF} CR=${CR} BW=${BW} payload=\"${PAYLOAD}\"")
