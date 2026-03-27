# multi_packet_test.cmake
# Concatenate a capture with a noise gap and itself, then run --multi decode.
# Expects: LORA_REPLAY, SRC_IQ, MULTI_IQ, META

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Build the concatenated IQ file: capture + 500k zero-samples gap + capture
execute_process(
    COMMAND "${Python3_EXECUTABLE}" -c "
import struct, pathlib, sys
src = pathlib.Path(sys.argv[1]).read_bytes()
gap = b'\\x00' * (500000 * 8)   # 500k complex-float samples (8 bytes each)
pathlib.Path(sys.argv[2]).write_bytes(src + gap + src)
" "${SRC_IQ}" "${MULTI_IQ}"
    RESULT_VARIABLE _gen_rc
)
if(NOT _gen_rc EQUAL 0)
    message(FATAL_ERROR "Failed to generate multi-packet IQ file")
endif()

# Run the multi-packet decoder
execute_process(
    COMMAND "${LORA_REPLAY}"
        --iq "${MULTI_IQ}"
        --metadata "${META}"
        --multi
    OUTPUT_VARIABLE _output
    ERROR_VARIABLE  _stderr
    RESULT_VARIABLE _rc
    TIMEOUT 120
)

message("${_output}")
message("${_stderr}")

# Count CRC OK occurrences
string(REGEX MATCHALL "CRC[^\n]*OK" _crc_matches "${_output}")
list(LENGTH _crc_matches _crc_count)

if(_crc_count GREATER_EQUAL 2)
    message("MULTI_PACKET_OK: decoded ${_crc_count} packets with CRC OK")
else()
    message(FATAL_ERROR "Multi-packet test failed: expected >=2 CRC OK, got ${_crc_count}")
endif()
