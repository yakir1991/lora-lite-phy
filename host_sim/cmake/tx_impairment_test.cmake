# tx_impairment_test.cmake
# Encode with lora_tx + channel impairments, decode with lora_replay.
# Runs multiple seeds and requires a minimum pass rate.
# Expects: LORA_TX, LORA_REPLAY, SF, CR, BW, PAYLOAD, WORK_DIR
#          SEEDS, MIN_OK_RATIO (e.g. "1.0" for 100%, "0.5" for 50%)
# Optional: SAMPLE_RATE, SNR, CFO, SFO, SOFT

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

if(DEFINED IMPLICIT AND "${IMPLICIT}" STREQUAL "1")
    set(_implicit_json "true")
else()
    set(_implicit_json "false")
endif()

set(META "${WORK_DIR}/meta.json")
file(WRITE "${META}"
    "{\"sf\":${SF},\"bw\":${BW},\"sample_rate\":${SAMPLE_RATE},\"cr\":${CR},\"payload_len\":${PAYLOAD_LEN},\"has_crc\":true,\"implicit_header\":${_implicit_json},\"ldro\":${LDRO},\"preamble_len\":8,\"sync_word\":18}")

# Build impairment description for logging
set(_impair_desc "")
if(DEFINED SNR AND NOT "${SNR}" STREQUAL "")
    string(APPEND _impair_desc " SNR=${SNR}dB")
endif()
if(DEFINED CFO AND NOT "${CFO}" STREQUAL "")
    string(APPEND _impair_desc " CFO=${CFO}Hz")
endif()
if(DEFINED SFO AND NOT "${SFO}" STREQUAL "")
    string(APPEND _impair_desc " SFO=${SFO}ppm")
endif()

set(OK_COUNT 0)
foreach(_seed RANGE 1 ${SEEDS})
    set(TX_IQ "${WORK_DIR}/tx_seed${_seed}.cf32")

    # Build TX command
    set(_tx_cmd "${LORA_TX}"
        --sf "${SF}" --cr "${CR}" --bw "${BW}"
        --sample-rate "${SAMPLE_RATE}"
        --payload "${PAYLOAD}"
        --output "${TX_IQ}")

    if(DEFINED SNR AND NOT "${SNR}" STREQUAL "")
        list(APPEND _tx_cmd --snr "${SNR}" --seed "${_seed}")
    endif()
    if(DEFINED CFO AND NOT "${CFO}" STREQUAL "")
        list(APPEND _tx_cmd --cfo "${CFO}")
    endif()
    if(DEFINED SFO AND NOT "${SFO}" STREQUAL "")
        list(APPEND _tx_cmd --sfo "${SFO}")
    endif()
    if(DEFINED IMPLICIT AND "${IMPLICIT}" STREQUAL "1")
        list(APPEND _tx_cmd --implicit)
    endif()

    execute_process(
        COMMAND ${_tx_cmd}
        OUTPUT_QUIET
        ERROR_VARIABLE _tx_err
        RESULT_VARIABLE _tx_rc
        TIMEOUT 30
    )
    if(NOT _tx_rc EQUAL 0)
        message(FATAL_ERROR "TX encode failed (seed=${_seed}):\n${_tx_err}")
    endif()

    # Decode
    set(_rx_cmd "${LORA_REPLAY}"
        --iq "${TX_IQ}"
        --metadata "${META}"
        --payload "${PAYLOAD}")

    if(DEFINED SOFT AND "${SOFT}" STREQUAL "1")
        list(APPEND _rx_cmd --soft)
    endif()

    execute_process(
        COMMAND ${_rx_cmd}
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

    file(REMOVE "${TX_IQ}")
endforeach()

# Compute minimum required OK count from ratio
# CMake doesn't support floating point math directly, so use integer approximation
# MIN_OK_RATIO is a string like "1.0" or "0.5" or "0.8"
if(NOT DEFINED MIN_OK_RATIO OR "${MIN_OK_RATIO}" STREQUAL "")
    set(MIN_OK_RATIO "1.0")
endif()

if("${MIN_OK_RATIO}" STREQUAL "1.0")
    set(MIN_OK ${SEEDS})
elseif("${MIN_OK_RATIO}" STREQUAL "0.5")
    math(EXPR MIN_OK "${SEEDS} / 2")
elseif("${MIN_OK_RATIO}" STREQUAL "0.8")
    math(EXPR MIN_OK "${SEEDS} * 4 / 5")
else()
    math(EXPR MIN_OK "${SEEDS} / 2")
endif()

if(OK_COUNT LESS MIN_OK)
    message(FATAL_ERROR "Impairment test FAILED: ${OK_COUNT}/${SEEDS} CRC OK (need >= ${MIN_OK}). SF=${SF} CR=${CR}${_impair_desc}")
endif()

message("Impairment test PASSED: ${OK_COUNT}/${SEEDS} CRC OK. SF=${SF} CR=${CR}${_impair_desc}")
