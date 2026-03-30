# tx_multi_packet_test.cmake
# Generate two TX packets, concatenate with a gap, decode with --multi.
# Expects: LORA_TX, LORA_REPLAY, SF, CR, BW, PAYLOAD1, PAYLOAD2, WORK_DIR

file(MAKE_DIRECTORY "${WORK_DIR}")

if(NOT DEFINED SAMPLE_RATE OR "${SAMPLE_RATE}" STREQUAL "")
    math(EXPR SAMPLE_RATE "${BW} * 4")
endif()

string(LENGTH "${PAYLOAD1}" PL1_LEN)
string(LENGTH "${PAYLOAD2}" PL2_LEN)

# Compute LDRO
math(EXPR _two_pow_sf "1 << ${SF}")
math(EXPR _sym_dur_us "${_two_pow_sf} * 1000000 / ${BW}")
if(_sym_dur_us GREATER 16000)
    set(LDRO "true")
else()
    set(LDRO "false")
endif()

set(TX_IQ1 "${WORK_DIR}/pkt1.cf32")
set(TX_IQ2 "${WORK_DIR}/pkt2.cf32")
set(MULTI_IQ "${WORK_DIR}/multi.cf32")
set(META "${WORK_DIR}/meta.json")

# Use the longer payload length for metadata (decoder needs max)
if(PL1_LEN GREATER PL2_LEN)
    set(PL_META ${PL1_LEN})
else()
    set(PL_META ${PL2_LEN})
endif()
file(WRITE "${META}"
    "{\"sf\":${SF},\"bw\":${BW},\"sample_rate\":${SAMPLE_RATE},\"cr\":${CR},\"payload_len\":${PL_META},\"has_crc\":true,\"implicit_header\":false,\"ldro\":${LDRO},\"preamble_len\":8,\"sync_word\":18}")

# TX encode packet 1
execute_process(
    COMMAND "${LORA_TX}"
        --sf "${SF}" --cr "${CR}" --bw "${BW}"
        --sample-rate "${SAMPLE_RATE}"
        --payload "${PAYLOAD1}"
        --output "${TX_IQ1}"
    OUTPUT_VARIABLE _tx1_out ERROR_VARIABLE _tx1_err RESULT_VARIABLE _tx1_rc TIMEOUT 30)
if(NOT _tx1_rc EQUAL 0)
    message(FATAL_ERROR "TX encode pkt1 failed:\n${_tx1_err}")
endif()
message("PKT1: ${_tx1_out}")

# TX encode packet 2
execute_process(
    COMMAND "${LORA_TX}"
        --sf "${SF}" --cr "${CR}" --bw "${BW}"
        --sample-rate "${SAMPLE_RATE}"
        --payload "${PAYLOAD2}"
        --output "${TX_IQ2}"
    OUTPUT_VARIABLE _tx2_out ERROR_VARIABLE _tx2_err RESULT_VARIABLE _tx2_rc TIMEOUT 30)
if(NOT _tx2_rc EQUAL 0)
    message(FATAL_ERROR "TX encode pkt2 failed:\n${_tx2_err}")
endif()
message("PKT2: ${_tx2_out}")

# Concatenate: pkt1 + gap + pkt2
find_package(Python3 COMPONENTS Interpreter REQUIRED)
execute_process(
    COMMAND "${Python3_EXECUTABLE}" -c "
import pathlib, sys
p1 = pathlib.Path(sys.argv[1]).read_bytes()
p2 = pathlib.Path(sys.argv[2]).read_bytes()
gap = b'\\x00' * (100000 * 8)
pathlib.Path(sys.argv[3]).write_bytes(p1 + gap + p2)
" "${TX_IQ1}" "${TX_IQ2}" "${MULTI_IQ}"
    RESULT_VARIABLE _gen_rc)
if(NOT _gen_rc EQUAL 0)
    message(FATAL_ERROR "Failed to concatenate IQ files")
endif()

# Decode with --multi
execute_process(
    COMMAND "${LORA_REPLAY}"
        --iq "${MULTI_IQ}"
        --metadata "${META}"
        --multi
    OUTPUT_VARIABLE _rx_out ERROR_VARIABLE _rx_err RESULT_VARIABLE _rx_rc TIMEOUT 120)
message("RX output:\n${_rx_out}")

# Count CRC OK
string(REGEX MATCHALL "CRC[^\n]*OK" _crc_matches "${_rx_out}")
list(LENGTH _crc_matches _crc_count)
if(_crc_count GREATER_EQUAL 2)
    message("TX_MULTI_PACKET_OK: decoded ${_crc_count} packets with CRC OK")
else()
    message(FATAL_ERROR "Multi-packet test failed: expected >=2 CRC OK, got ${_crc_count}\nOutput: ${_rx_out}")
endif()

# Clean up large files
file(REMOVE "${TX_IQ1}" "${TX_IQ2}" "${MULTI_IQ}")
