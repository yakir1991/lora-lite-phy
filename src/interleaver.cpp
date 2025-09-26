#include "interleaver.hpp"

namespace lora_lite {

void interleave_bits(const uint8_t* in, uint8_t* out, int sf_app, int cw_len) {
    // in layout: rows = sf_app, cols = cw_len (row-major)
    // out layout: columns serialized with diagonal shift inverse of deinterleaver
    for (int col=0; col<cw_len; ++col) {
        for (int row=0; row<sf_app; ++row) {
            int dest_row = (col - row - 1);
            dest_row %= sf_app;
            if (dest_row < 0) dest_row += sf_app;
            out[col*sf_app + row] = in[dest_row*cw_len + col];
        }
    }
}

void deinterleave_bits(const uint8_t* in, uint8_t* out, int sf_app, int cw_len) {
    // inverse of above
    for (int col=0; col<cw_len; ++col) {
        for (int row=0; row<sf_app; ++row) {
            int dest_row = (col - row - 1);
            dest_row %= sf_app;
            if (dest_row < 0) dest_row += sf_app;
            out[dest_row*cw_len + col] = in[col*sf_app + row];
        }
    }
}

} // namespace lora_lite
