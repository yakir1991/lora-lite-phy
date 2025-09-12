#include "lora/rx/frame.hpp"
#include "lora/rx/header_decode.hpp"
#include "lora/rx/payload_decode.hpp"
#include <cstdlib>
#include <cstdio>

namespace lora::rx {

std::optional<LocalHeader> decode_header_with_preamble_cfo_sto_os(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    lora::utils::CodeRate cr,
    size_t min_preamble_syms,
    uint8_t expected_sync) {
    if (std::getenv("LORA_DEBUG"))
        printf("DEBUG: decode_header_with_preamble_cfo_sto_os called!\n");

    return decode_header_with_preamble_cfo_sto_os_impl(ws, samples, sf, cr,
                                                       min_preamble_syms,
                                                       expected_sync);
}

std::pair<std::vector<uint8_t>, bool> decode_payload_no_crc_with_preamble_cfo_sto_os(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    lora::utils::CodeRate cr,
    size_t min_preamble_syms,
    uint8_t expected_sync) {
    return decode_payload_no_crc_with_preamble_cfo_sto_os_impl(ws, samples, sf, cr,
                                                               min_preamble_syms, expected_sync);
}

} // namespace lora::rx
