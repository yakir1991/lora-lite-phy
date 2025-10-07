# Reference case: SF10 BW500k CR4 high-SNR header fail

Vector: golden_vectors/new_batch/tx_sf10_bw500000_cr4_crc1_impl0_ldro0_pay10_snr+9.8dB.cf32
Expected payload (GNU Radio): 58f77c8829437f0be905

C++ streaming receiver attempts and status:
- With multi-offset attempts (0, ±sps/8, ±sps/4, ±sps/2), header still fails at all capture sizes.
- p_ofs_est ~ -50 samples; trying offsets around it did not yield a header decode.

Next debugging ideas:
- Add CFO estimation or derotate preamble window before header demod.
- Try a small integer-symbol offset search (±1 symbol) in addition to fractional-sample tweaks.
- Compare header demod IQ slice vs GNU Radio to check header start alignment (export a short IQ snippet of preamble->header and plot).
