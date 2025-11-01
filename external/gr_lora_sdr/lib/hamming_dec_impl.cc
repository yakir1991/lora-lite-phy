
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gnuradio/io_signature.h>
#include <gnuradio/lora_sdr/utilities.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

#include "hamming_dec_impl.h"

#ifdef GRLORA_DEBUG
#include <algorithm>  // find in LUT
#include <bitset>     // debug bit
#endif

namespace gr {
    namespace lora_sdr {

namespace {

using gr::lora_sdr::LLR;

std::ofstream &hamming_trace_stream() {
    static std::ofstream stream;
    static std::string path_value;

    const char *env_path = std::getenv("GR_LORA_TRACE_HAMMING_DEC");
    std::string requested_path = (env_path && env_path[0] != '\0') ? std::string(env_path) : std::string();

    if (!requested_path.empty() && requested_path != path_value) {
        if (stream.is_open()) {
            stream.close();
        }
        path_value = requested_path;
    }

    if (!path_value.empty() && !stream.is_open()) {
        stream.open(path_value, std::ios::out | std::ios::app);
        stream << std::setprecision(6) << std::fixed;
    }

    return stream;
}

std::mutex &hamming_trace_mutex() {
    static std::mutex mtx;
    return mtx;
}

uint64_t next_hamming_trace_id() {
    static uint64_t counter = 0;
    return counter++;
}

template <typename Vector>
std::string vector_to_string(const Vector &vec) {
    std::ostringstream oss;
    oss << '[';
    for (std::size_t i = 0; i < vec.size(); ++i) {
        if (i != 0) {
            oss << ',';
        }
        oss << vec[i];
    }
    oss << ']';
    return oss.str();
}

template <typename BoolVector>
std::string bool_vector_to_string(const BoolVector &vec) {
    std::ostringstream oss;
    oss << '[';
    for (std::size_t i = 0; i < vec.size(); ++i) {
        if (i != 0) {
            oss << ',';
        }
        oss << (vec[i] ? 1 : 0);
    }
    oss << ']';
    return oss.str();
}

void log_hamming_soft(bool is_header,
                      int cr_app,
                      int cw_len,
                      const std::vector<LLR> &codeword,
                      int selected_idx,
                      uint8_t decoded_nibble) {
    auto &stream = hamming_trace_stream();
    if (!stream.is_open()) {
        return;
    }
    std::lock_guard<std::mutex> lock(hamming_trace_mutex());
    const uint64_t id = next_hamming_trace_id();
    stream << "{\"id\":" << id << ",\"stage\":\"soft\",\"is_header\":" << (is_header ? 1 : 0)
           << ",\"cr_app\":" << cr_app << ",\"cw_len\":" << cw_len
           << ",\"llr\":" << vector_to_string(codeword) << ",\"selected\":" << selected_idx
           << ",\"decoded\":" << static_cast<int>(decoded_nibble) << "}\n";
}

void log_hamming_hard(bool is_header,
                      int cr_app,
                      int cw_len,
                      const std::vector<bool> &codeword,
                      int syndrome,
                      uint8_t decoded_nibble) {
    auto &stream = hamming_trace_stream();
    if (!stream.is_open()) {
        return;
    }
    std::lock_guard<std::mutex> lock(hamming_trace_mutex());
    const uint64_t id = next_hamming_trace_id();
    stream << "{\"id\":" << id << ",\"stage\":\"hard\",\"is_header\":" << (is_header ? 1 : 0)
           << ",\"cr_app\":" << cr_app << ",\"cw_len\":" << cw_len
           << ",\"bits\":" << bool_vector_to_string(codeword)
           << ",\"syndrome\":" << syndrome << ",\"decoded\":" << static_cast<int>(decoded_nibble) << "}\n";
}

} // namespace

        hamming_dec::sptr
        hamming_dec::make(bool soft_decoding) {
            return gnuradio::get_initial_sptr(new hamming_dec_impl(soft_decoding));
        }

        /*
         * The private constructor
         */
        hamming_dec_impl::hamming_dec_impl(bool soft_decoding)
            : gr::sync_block("hamming_dec",
                             gr::io_signature::make(1, 1, soft_decoding ? 8 * sizeof(LLR) : sizeof(uint8_t)),  // In reality: cw_len = cr_app + 4  < 8
                             gr::io_signature::make(1, 1, sizeof(uint8_t))),
              m_soft_decoding(soft_decoding) {
            set_tag_propagation_policy(TPP_ONE_TO_ONE);
        }
        /*
         * Our virtual destructor.
         */
        hamming_dec_impl::~hamming_dec_impl() {
        }

        int hamming_dec_impl::work(int noutput_items,
                                   gr_vector_const_void_star &input_items,
                                   gr_vector_void_star &output_items) {
            const uint8_t *in = (const uint8_t *)input_items[0];
            const LLR *in2 = (const LLR *)input_items[0];
            uint8_t *out = (uint8_t *)output_items[0];
            int nitems_to_process = noutput_items;

            std::vector<tag_t> tags;
            get_tags_in_window(tags, 0, 0, noutput_items, pmt::string_to_symbol("frame_info"));
            if (tags.size()) {
                if (tags[0].offset != nitems_read(0))
                    nitems_to_process = tags[0].offset - nitems_read(0);  // only decode codewords until the next frame begin

                else {
                    if (tags.size() >= 2)
                        nitems_to_process = tags[1].offset - tags[0].offset;

                    pmt::pmt_t err = pmt::string_to_symbol("error");
                    is_header = pmt::to_bool(pmt::dict_ref(tags[0].value, pmt::string_to_symbol("is_header"), err));

                    if (!is_header) {
                        m_cr = pmt::to_long(pmt::dict_ref(tags[0].value, pmt::string_to_symbol("cr"), err));
                        // std::cout<<"\nhamming_cr "<<tags[0].offset<<" - cr: "<<(int)m_cr<<"\n";
                    }
                }
            }

            cr_app = is_header ? 4 : m_cr;
            uint8_t cw_len = cr_app + 4;

            for (int i = 0; i < nitems_to_process; i++) {
                if (m_soft_decoding) {
                    std::vector<LLR> codeword_LLR(cw_len, 0);

                    memcpy(codeword_LLR.data(), in2 + i * 8, cw_len * sizeof(LLR));

#ifdef GRLORA_DEBUG
                    // convert LLR to binary for debug
                    uint8_t x(0);
                    for (int i(0); i < cw_len; i++) x += (codeword_LLR[i] > 0) << (7 - i);
                    std::bitset<8> X(x);
                    std::cout << "Hamming in-symbol: " << +x << " " << X << std::endl;
#endif

                    /*  Hamming Look-up Table generation, parity bits formula with data [d0 d1 d2 d3]:
                     *      p0 = d0 ^ d1 ^ d2;     ^ = xor
                     *      p1 = d1 ^ d2 ^ d3;
                     *      p2 = d0 ^ d1 ^ d3;
                     *      p3 = d0 ^ d2 ^ d3;
                     *
                     *      p = d0 ^ d1 ^ d2 ^ d3;  for CR=4/5
                     * 
                     *      For LUT, store the decimal value instead of bit matrix, same LUT for CR 4/6, 4/7 and 4/8 (just crop)
                     *      e.g.    139 = [ 1 0 0 0 | 1 0 1 1 ] = [ d0 d1 d2 d3 | p0 p1 p2 p3]
                     */
                    const uint8_t cw_nbr = 16;  // In LoRa, always "only" 16 possible codewords => compare with all and take argmax
                    uint8_t cw_LUT[cw_nbr] = {0, 23, 45, 58, 78, 89, 99, 116, 139, 156, 166, 177, 197, 210, 232, 255};
                    uint8_t cw_LUT_cr5[cw_nbr] = {0, 24, 40, 48, 72, 80, 96, 120, 136, 144, 160, 184, 192, 216, 232, 240};  // Different for cr = 4/5

                    LLR cw_proba[cw_nbr] = {0};

                    for (int n = 0; n < cw_nbr; n++) {      // for all possible codeword
                        for (int j = 0; j < cw_len; j++) {  // for all codeword bits
                            // Select correct bit            from correct LUT          crop table (cr)    bit position mask
                            bool bit = (((cr_app != 1) ? cw_LUT[n] : cw_LUT_cr5[n]) >> (8 - cw_len)) & (1u << (cw_len - 1 - j));
                            // if LLR > 0 --> 1     if LLR < 0 --> 0
                            if ((bit and codeword_LLR[j] > 0) or (!bit and codeword_LLR[j] < 0)) {  // if correct bit 1-->1 or 0-->0
                                cw_proba[n] += abs(codeword_LLR[j]);
                            } else {                                  // if incorrect bit 0-->1 or 1-->0
                                cw_proba[n] -= abs(codeword_LLR[j]);  // penalty
                            }  // can be optimized in 1 line: ... + ((cond)? 1 : -1) * abs(codeword_LLR[j]); but less readable
                        }
                    }
                    // Select the codeword with the maximum probability (ML)
                    uint8_t idx_max = std::max_element(cw_proba, cw_proba + cw_nbr) - cw_proba;
                    // convert LLR to binary => Hard decision
                    uint8_t data_nibble_soft = cw_LUT[idx_max] >> 4;  // Take data bits of the correct codeword (=> discard hamming code part)

#ifdef GRLORA_DEBUG
                    // for (int n = 0; n < cw_nbr; n++) std::cout << cw_proba[n] << std::endl;
                    std::cout << "correct cw " << unsigned(correct_cw) << " with proba " << cw_proba[idx_max] << " idxm " << unsigned(idx_max) << std::endl;

                    /*if ( std::find(cw_LUT.begin(), cw_LUT.end(), x) != cw_LUT.end() )
                        std::cout << "LUT " << unsigned(x) << std::endl;
                    else
                        std::cout << "NOT in LUT " << unsigned(x) << std::endl;*/
#endif

                    // Output the most probable data nibble
                    // and reversed bit order MSB<=>LSB
                    out[i] = ((bool)(data_nibble_soft & 0b0001) << 3) + ((bool)(data_nibble_soft & 0b0010) << 2) + ((bool)(data_nibble_soft & 0b0100) << 1) + (bool)(data_nibble_soft & 0b1000);

                    log_hamming_soft(is_header, cr_app, cw_len, codeword_LLR, idx_max, out[i]);


                } 
                else {// Hard decoding
                    std::vector<bool> data_nibble(4, 0);
                    bool s0, s1, s2 = 0;
                    int syndrom = 0;
                    std::vector<bool> codeword;

                    codeword = int2bool(in[i], cr_app + 4);
                    data_nibble = {codeword[3], codeword[2], codeword[1], codeword[0]};  // reorganized msb-first

                    switch (cr_app) {
                        case 4:
                            if (!(count(codeword.begin(), codeword.end(), true) % 2))  // Don't correct if even number of errors
                                break;
                        case 3:
                            // get syndrom
                            s0 = codeword[0] ^ codeword[1] ^ codeword[2] ^ codeword[4];
                            s1 = codeword[1] ^ codeword[2] ^ codeword[3] ^ codeword[5];
                            s2 = codeword[0] ^ codeword[1] ^ codeword[3] ^ codeword[6];

                            syndrom = s0 + (s1 << 1) + (s2 << 2);

                            switch (syndrom) {
                                case 5:
                                    data_nibble[3].flip();
                                    break;
                                case 7:
                                    data_nibble[2].flip();
                                    break;
                                case 3:
                                    data_nibble[1].flip();
                                    break;
                                case 6:
                                    data_nibble[0].flip();
                                    break;
                                default:  // either parity bit wrong or no error
                                    break;
                            }
                            break;
                        case 2:
                            s0 = codeword[0] ^ codeword[1] ^ codeword[2] ^ codeword[4];
                            s1 = codeword[1] ^ codeword[2] ^ codeword[3] ^ codeword[5];

                            if (s0 | s1) {
                            }
                            break;
                        case 1:
                            if (!(count(codeword.begin(), codeword.end(), true) % 2)) {
                            }
                            break;
                    }

                    out[i] = bool2int(data_nibble);

                    log_hamming_hard(is_header, cr_app, cw_len, codeword, syndrom, out[i]);
                }
            }
            return nitems_to_process;
        }
    }  // namespace lora_sdr
} /* namespace gr */
