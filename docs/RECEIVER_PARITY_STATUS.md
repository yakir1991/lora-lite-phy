# Receiver Parity Status

This document summarises parity between the C++ receiver, the Python reference, and the GNU Radio offline decoder. Data produced on "results/receiver_comparison.json".

## Summary Counts

- **all_match**: 25
- **cpp_gnur_only**: 8
- **cpp_only**: 8
- **cpp_python_only**: 20
- **cpp_vs_python_mismatch**: 12
- **cpp_vs_gnur_mismatch**: 4
- **python_only**: 4
- **python_vs_gnur_mismatch**: 4

## Detailed Table

| Vector | SF | CR | LDRO | Implicit | C++ Payload | Python Payload | GNU Radio Payload | Category |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| golden_vectors/custom/tx_sf7_bw125000_cr3_crc1_impl0_ldro0_criminal.cf32 | 7 | 3 | 0 | no | 01012a15…93f3f | 01012a15…93f3f | 01012a15…93f3f | all_match |
| golden_vectors/extended_batch/tx_sf10_bw125000_cr3_crc1_impl0_ldro0_pay19.cf32 | 10 | 3 | 0 | no | 53545245…f4144 | 53545245…f4144 | 53545245…f4144 | all_match |
| golden_vectors/extended_batch/tx_sf10_bw125000_cr3_crc1_impl0_ldro1_pay19.cf32 | 10 | 3 | 1 | no | 53545245…f4144 |  | 53545245…f4144 | cpp_gnur_only |
| golden_vectors/extended_batch/tx_sf10_bw125000_cr3_crc1_impl0_ldro2_pay19.cf32 | 10 | 3 | 2 | no | 53545245…f4144 | 53545245…f4144 | 53545245…f4144 | all_match |
| golden_vectors/extended_batch/tx_sf10_bw125000_cr4_crc1_impl0_ldro0_pay19.cf32 | 10 | 4 | 0 | no | 53545245…f4144 | 53545245…f4144 | 53545245…f4144 | all_match |
| golden_vectors/extended_batch/tx_sf10_bw125000_cr4_crc1_impl0_ldro1_pay19.cf32 | 10 | 4 | 1 | no | 53545245…f4144 |  | 53545245…f4144 | cpp_gnur_only |
| golden_vectors/extended_batch/tx_sf10_bw125000_cr4_crc1_impl0_ldro2_pay19.cf32 | 10 | 4 | 2 | no | 53545245…f4144 | 53545245…f4144 | 53545245…f4144 | all_match |
| golden_vectors/extended_batch/tx_sf11_bw125000_cr3_crc1_impl0_ldro0_pay19.cf32 | 11 | 3 | 0 | no | 53545245…f4144 |  |  | cpp_only |
| golden_vectors/extended_batch/tx_sf11_bw125000_cr3_crc1_impl0_ldro1_pay19.cf32 | 11 | 3 | 1 | no | 53545245…f4144 | 53545245…f4144 |  | cpp_python_only |
| golden_vectors/extended_batch/tx_sf11_bw125000_cr3_crc1_impl0_ldro2_pay19.cf32 | 11 | 3 | 2 | no | 53545245…f4144 | 53545245…f4144 |  | cpp_python_only |
| golden_vectors/extended_batch/tx_sf11_bw125000_cr4_crc1_impl0_ldro0_pay19.cf32 | 11 | 4 | 0 | no | 53545245…f4144 |  |  | cpp_only |
| golden_vectors/extended_batch/tx_sf11_bw125000_cr4_crc1_impl0_ldro1_pay19.cf32 | 11 | 4 | 1 | no | 53545245…f4144 | 53545245…f4144 |  | cpp_python_only |
| golden_vectors/extended_batch/tx_sf11_bw125000_cr4_crc1_impl0_ldro2_pay19.cf32 | 11 | 4 | 2 | no | 53545245…f4144 | 53545245…f4144 |  | cpp_python_only |
| golden_vectors/extended_batch/tx_sf12_bw125000_cr3_crc1_impl0_ldro0_pay19.cf32 | 12 | 3 | 0 | no | 53545245…f4144 |  |  | cpp_only |
| golden_vectors/extended_batch/tx_sf12_bw125000_cr3_crc1_impl0_ldro1_pay19.cf32 | 12 | 3 | 1 | no | 53545245…f4144 | 53545245…f4144 |  | cpp_python_only |
| golden_vectors/extended_batch/tx_sf12_bw125000_cr3_crc1_impl0_ldro2_pay19.cf32 | 12 | 3 | 2 | no | 53545245…f4144 | 53545245…f4144 |  | cpp_python_only |
| golden_vectors/extended_batch/tx_sf12_bw125000_cr4_crc1_impl0_ldro0_pay19.cf32 | 12 | 4 | 0 | no | 53545245…f4144 |  |  | cpp_only |
| golden_vectors/extended_batch/tx_sf12_bw125000_cr4_crc1_impl0_ldro1_pay19.cf32 | 12 | 4 | 1 | no | 53545245…f4144 | 53545245…f4144 |  | cpp_python_only |
| golden_vectors/extended_batch/tx_sf12_bw125000_cr4_crc1_impl0_ldro2_pay19.cf32 | 12 | 4 | 2 | no | 53545245…f4144 | 53545245…f4144 |  | cpp_python_only |
| golden_vectors/extended_batch/tx_sf9_bw125000_cr3_crc1_impl0_ldro0_pay19.cf32 | 9 | 3 | 0 | no | 53545245…f4144 | 53545245…f4144 | 53545245…f4144 | all_match |
| golden_vectors/extended_batch/tx_sf9_bw125000_cr3_crc1_impl0_ldro1_pay19.cf32 | 9 | 3 | 1 | no | 53545245…f4144 |  | 53545245…f4144 | cpp_gnur_only |
| golden_vectors/extended_batch/tx_sf9_bw125000_cr3_crc1_impl0_ldro2_pay19.cf32 | 9 | 3 | 2 | no | 53545245…f4144 | 53545245…f4144 | 53545245…f4144 | all_match |
| golden_vectors/extended_batch/tx_sf9_bw125000_cr4_crc1_impl0_ldro0_pay19.cf32 | 9 | 4 | 0 | no | 53545245…f4144 | 53545245…f4144 | 53545245…f4144 | all_match |
| golden_vectors/extended_batch/tx_sf9_bw125000_cr4_crc1_impl0_ldro1_pay19.cf32 | 9 | 4 | 1 | no | 53545245…f4144 |  | 53545245…f4144 | cpp_gnur_only |
| golden_vectors/extended_batch/tx_sf9_bw125000_cr4_crc1_impl0_ldro2_pay19.cf32 | 9 | 4 | 2 | no | 53545245…f4144 | 53545245…f4144 | 53545245…f4144 | all_match |
| golden_vectors/extended_batch_crc_off/tx_sf10_bw125000_cr3_crc0_impl0_ldro0_pay19.cf32 | 10 | 3 | 0 | no | 53545245…f4144 | 53545245…f4144 | 53545245…f4144 | all_match |
| golden_vectors/extended_batch_crc_off/tx_sf10_bw125000_cr3_crc0_impl0_ldro1_pay19.cf32 | 10 | 3 | 1 | no | 53545245…f4144 | 535451da…f14ba | 53545245…f4144 | cpp_vs_python_mismatch |
| golden_vectors/extended_batch_crc_off/tx_sf10_bw125000_cr3_crc0_impl0_ldro2_pay19.cf32 | 10 | 3 | 2 | no | 53545245…f4144 | 53545245…f4144 | 53545245…f4144 | all_match |
| golden_vectors/extended_batch_crc_off/tx_sf10_bw125000_cr4_crc0_impl0_ldro0_pay19.cf32 | 10 | 4 | 0 | no | 53545245…f4144 | 53545245…f4144 | 53545245…f4144 | all_match |
| golden_vectors/extended_batch_crc_off/tx_sf10_bw125000_cr4_crc0_impl0_ldro1_pay19.cf32 | 10 | 4 | 1 | no | 53545245…f4144 | 535451da…f14ba | 53545245…f4144 | cpp_vs_python_mismatch |
| golden_vectors/extended_batch_crc_off/tx_sf10_bw125000_cr4_crc0_impl0_ldro2_pay19.cf32 | 10 | 4 | 2 | no | 53545245…f4144 | 53545245…f4144 | 53545245…f4144 | all_match |
| golden_vectors/extended_batch_crc_off/tx_sf11_bw125000_cr3_crc0_impl0_ldro0_pay19.cf32 | 11 | 3 | 0 | no | 53545245…f4144 | 53545353…28d1a |  | cpp_vs_python_mismatch |
| golden_vectors/extended_batch_crc_off/tx_sf11_bw125000_cr3_crc0_impl0_ldro1_pay19.cf32 | 11 | 3 | 1 | no | 53545245…f4144 | 53545245…f4144 |  | cpp_python_only |
| golden_vectors/extended_batch_crc_off/tx_sf11_bw125000_cr3_crc0_impl0_ldro2_pay19.cf32 | 11 | 3 | 2 | no | 53545245…f4144 | 53545245…f4144 |  | cpp_python_only |
| golden_vectors/extended_batch_crc_off/tx_sf11_bw125000_cr4_crc0_impl0_ldro0_pay19.cf32 | 11 | 4 | 0 | no | 53545245…f4144 | 53545353…28d1a |  | cpp_vs_python_mismatch |
| golden_vectors/extended_batch_crc_off/tx_sf11_bw125000_cr4_crc0_impl0_ldro1_pay19.cf32 | 11 | 4 | 1 | no | 53545245…f4144 | 53545245…f4144 |  | cpp_python_only |
| golden_vectors/extended_batch_crc_off/tx_sf11_bw125000_cr4_crc0_impl0_ldro2_pay19.cf32 | 11 | 4 | 2 | no | 53545245…f4144 | 53545245…f4144 |  | cpp_python_only |
| golden_vectors/extended_batch_crc_off/tx_sf12_bw125000_cr3_crc0_impl0_ldro0_pay19.cf32 | 12 | 3 | 0 | no | 53545245…f4144 |  |  | cpp_only |
| golden_vectors/extended_batch_crc_off/tx_sf12_bw125000_cr3_crc0_impl0_ldro1_pay19.cf32 | 12 | 3 | 1 | no | 53545245…f4144 | 53545245…f4144 |  | cpp_python_only |
| golden_vectors/extended_batch_crc_off/tx_sf12_bw125000_cr3_crc0_impl0_ldro2_pay19.cf32 | 12 | 3 | 2 | no | 53545245…f4144 | 53545245…f4144 |  | cpp_python_only |
| golden_vectors/extended_batch_crc_off/tx_sf12_bw125000_cr4_crc0_impl0_ldro0_pay19.cf32 | 12 | 4 | 0 | no | 53545245…f4144 |  |  | cpp_only |
| golden_vectors/extended_batch_crc_off/tx_sf12_bw125000_cr4_crc0_impl0_ldro1_pay19.cf32 | 12 | 4 | 1 | no | 53545245…f4144 | 53545245…f4144 |  | cpp_python_only |
| golden_vectors/extended_batch_crc_off/tx_sf12_bw125000_cr4_crc0_impl0_ldro2_pay19.cf32 | 12 | 4 | 2 | no | 53545245…f4144 | 53545245…f4144 |  | cpp_python_only |
| golden_vectors/extended_batch_crc_off/tx_sf9_bw125000_cr3_crc0_impl0_ldro0_pay19.cf32 | 9 | 3 | 0 | no | 53545245…f4144 | 53545245…f4144 | 53545245…f4144 | all_match |
| golden_vectors/extended_batch_crc_off/tx_sf9_bw125000_cr3_crc0_impl0_ldro1_pay19.cf32 | 9 | 3 | 1 | no | 53545245…f4144 | 5364d45e…6b4b2 | 53545245…f4144 | cpp_vs_python_mismatch |
| golden_vectors/extended_batch_crc_off/tx_sf9_bw125000_cr3_crc0_impl0_ldro2_pay19.cf32 | 9 | 3 | 2 | no | 53545245…f4144 | 53545245…f4144 | 53545245…f4144 | all_match |
| golden_vectors/extended_batch_crc_off/tx_sf9_bw125000_cr4_crc0_impl0_ldro0_pay19.cf32 | 9 | 4 | 0 | no | 53545245…f4144 | 53545245…f4144 | 53545245…f4144 | all_match |
| golden_vectors/extended_batch_crc_off/tx_sf9_bw125000_cr4_crc0_impl0_ldro1_pay19.cf32 | 9 | 4 | 1 | no | 53545245…f4144 | 5364d45e…6b4b2 | 53545245…f4144 | cpp_vs_python_mismatch |
| golden_vectors/extended_batch_crc_off/tx_sf9_bw125000_cr4_crc0_impl0_ldro2_pay19.cf32 | 9 | 4 | 2 | no | 53545245…f4144 | 53545245…f4144 | 53545245…f4144 | all_match |
| golden_vectors/extended_batch_impl/tx_sf10_bw125000_cr3_crc0_impl1_ldro0_pay19.cf32 | 10 | 3 | 0 | yes | 73965e56…2a99b | 73965e56…2a99b | 53545245…f4144 | cpp_vs_gnur_mismatch |
| golden_vectors/extended_batch_impl/tx_sf10_bw125000_cr3_crc0_impl1_ldro1_pay19.cf32 | 10 | 3 | 1 | yes | 53545245…70cd2 | 73965e56…da11c | 53545245…f4144 | cpp_vs_python_mismatch |
| golden_vectors/extended_batch_impl/tx_sf10_bw125000_cr3_crc0_impl1_ldro2_pay19.cf32 | 10 | 3 | 2 | yes | 73965e56…2a99b | 73965e56…2a99b | 53545245…f4144 | cpp_vs_gnur_mismatch |
| golden_vectors/extended_batch_impl/tx_sf10_bw125000_cr4_crc0_impl1_ldro0_pay19.cf32 | 10 | 4 | 0 | yes | 73965e56…904d6 | 73965e56…904d6 | 53545245…f4144 | cpp_vs_gnur_mismatch |
| golden_vectors/extended_batch_impl/tx_sf10_bw125000_cr4_crc0_impl1_ldro1_pay19.cf32 | 10 | 4 | 1 | yes | 53545245…f4144 | 73965e56…cb4b2 | 53545245…f4144 | cpp_vs_python_mismatch |
| golden_vectors/extended_batch_impl/tx_sf10_bw125000_cr4_crc0_impl1_ldro2_pay19.cf32 | 10 | 4 | 2 | yes | 73965e56…904d6 | 73965e56…904d6 | 53545245…f4144 | cpp_vs_gnur_mismatch |
| golden_vectors/extended_batch_impl/tx_sf11_bw125000_cr3_crc0_impl1_ldro0_pay19.cf32 | 11 | 3 | 0 | yes | 63945e56…3b98a |  |  | cpp_only |
| golden_vectors/extended_batch_impl/tx_sf11_bw125000_cr3_crc0_impl1_ldro1_pay19.cf32 | 11 | 3 | 1 | yes |  | 53545245…30ccb |  | python_only |
| golden_vectors/extended_batch_impl/tx_sf11_bw125000_cr3_crc0_impl1_ldro2_pay19.cf32 | 11 | 3 | 2 | yes |  | 53545245…30ccb |  | python_only |
| golden_vectors/extended_batch_impl/tx_sf11_bw125000_cr4_crc0_impl1_ldro0_pay19.cf32 | 11 | 4 | 0 | yes | 63945e56…904d6 |  |  | cpp_only |
| golden_vectors/extended_batch_impl/tx_sf11_bw125000_cr4_crc0_impl1_ldro1_pay19.cf32 | 11 | 4 | 1 | yes |  | 53545245…f4144 |  | python_only |
| golden_vectors/extended_batch_impl/tx_sf11_bw125000_cr4_crc0_impl1_ldro2_pay19.cf32 | 11 | 4 | 2 | yes |  | 53545245…f4144 |  | python_only |
| golden_vectors/extended_batch_impl/tx_sf12_bw125000_cr3_crc0_impl1_ldro0_pay19.cf32 | 12 | 3 | 0 | yes | 63965656…2a89b | 53545245…88c1b |  | cpp_vs_python_mismatch |
| golden_vectors/extended_batch_impl/tx_sf12_bw125000_cr3_crc0_impl1_ldro1_pay19.cf32 | 12 | 3 | 1 | yes | 53545245…30cd3 | 53545245…30cd3 |  | cpp_python_only |
| golden_vectors/extended_batch_impl/tx_sf12_bw125000_cr3_crc0_impl1_ldro2_pay19.cf32 | 12 | 3 | 2 | yes | 53545245…30cd3 | 53545245…30cd3 |  | cpp_python_only |
| golden_vectors/extended_batch_impl/tx_sf12_bw125000_cr4_crc0_impl1_ldro0_pay19.cf32 | 12 | 4 | 0 | yes | 63965656…904d6 | 53545245…f8d1a |  | cpp_vs_python_mismatch |
| golden_vectors/extended_batch_impl/tx_sf12_bw125000_cr4_crc0_impl1_ldro1_pay19.cf32 | 12 | 4 | 1 | yes | 53545245…f4144 | 53545245…f4144 |  | cpp_python_only |
| golden_vectors/extended_batch_impl/tx_sf12_bw125000_cr4_crc0_impl1_ldro2_pay19.cf32 | 12 | 4 | 2 | yes | 53545245…f4144 | 53545245…f4144 |  | cpp_python_only |
| golden_vectors/extended_batch_impl/tx_sf9_bw125000_cr3_crc0_impl1_ldro0_pay19.cf32 | 9 | 3 | 0 | yes |  | 63945656…2a89a | 53545245…f4144 | python_vs_gnur_mismatch |
| golden_vectors/extended_batch_impl/tx_sf9_bw125000_cr3_crc0_impl1_ldro1_pay19.cf32 | 9 | 3 | 1 | yes | 535452d5…39dd3 | 63945656…6a112 | 53545245…f4144 | cpp_vs_python_mismatch |
| golden_vectors/extended_batch_impl/tx_sf9_bw125000_cr3_crc0_impl1_ldro2_pay19.cf32 | 9 | 3 | 2 | yes |  | 63945656…2a89a | 53545245…f4144 | python_vs_gnur_mismatch |
| golden_vectors/extended_batch_impl/tx_sf9_bw125000_cr4_crc0_impl1_ldro0_pay19.cf32 | 9 | 4 | 0 | yes |  | 63945656…904d6 | 53545245…f4144 | python_vs_gnur_mismatch |
| golden_vectors/extended_batch_impl/tx_sf9_bw125000_cr4_crc0_impl1_ldro1_pay19.cf32 | 9 | 4 | 1 | yes | 53545245…f4144 | 63945656…ab492 | 53545245…f4144 | cpp_vs_python_mismatch |
| golden_vectors/extended_batch_impl/tx_sf9_bw125000_cr4_crc0_impl1_ldro2_pay19.cf32 | 9 | 4 | 2 | yes |  | 63945656…904d6 | 53545245…f4144 | python_vs_gnur_mismatch |
| golden_vectors/new_batch/tx_sf7_bw125000_cr1_crc1_impl0_ldro0_pay18.cf32 | 7 | 1 | 0 | no | 48454c4c…24c44 | 48454c4c…24c44 | 48454c4c…24c44 | all_match |
| golden_vectors/new_batch/tx_sf7_bw125000_cr1_crc1_impl0_ldro1_pay18.cf32 | 7 | 1 | 1 | no | 48454c4c…24c44 |  | 48454c4c…24c44 | cpp_gnur_only |
| golden_vectors/new_batch/tx_sf7_bw125000_cr1_crc1_impl0_ldro2_pay18.cf32 | 7 | 1 | 2 | no | 48454c4c…24c44 | 48454c4c…24c44 | 48454c4c…24c44 | all_match |
| golden_vectors/new_batch/tx_sf7_bw125000_cr2_crc1_impl0_ldro0_pay18.cf32 | 7 | 2 | 0 | no | 48454c4c…24c44 | 48454c4c…24c44 | 48454c4c…24c44 | all_match |
| golden_vectors/new_batch/tx_sf7_bw125000_cr2_crc1_impl0_ldro1_pay18.cf32 | 7 | 2 | 1 | no | 48454c4c…24c44 |  | 48454c4c…24c44 | cpp_gnur_only |
| golden_vectors/new_batch/tx_sf7_bw125000_cr2_crc1_impl0_ldro2_pay18.cf32 | 7 | 2 | 2 | no | 48454c4c…24c44 | 48454c4c…24c44 | 48454c4c…24c44 | all_match |
| golden_vectors/new_batch/tx_sf8_bw125000_cr1_crc1_impl0_ldro0_pay18.cf32 | 8 | 1 | 0 | no | 48454c4c…24c44 | 48454c4c…24c44 | 48454c4c…24c44 | all_match |
| golden_vectors/new_batch/tx_sf8_bw125000_cr1_crc1_impl0_ldro1_pay18.cf32 | 8 | 1 | 1 | no | 48454c4c…24c44 |  | 48454c4c…24c44 | cpp_gnur_only |
| golden_vectors/new_batch/tx_sf8_bw125000_cr1_crc1_impl0_ldro2_pay18.cf32 | 8 | 1 | 2 | no | 48454c4c…24c44 | 48454c4c…24c44 | 48454c4c…24c44 | all_match |
| golden_vectors/new_batch/tx_sf8_bw125000_cr2_crc1_impl0_ldro0_pay18.cf32 | 8 | 2 | 0 | no | 48454c4c…24c44 | 48454c4c…24c44 | 48454c4c…24c44 | all_match |
| golden_vectors/new_batch/tx_sf8_bw125000_cr2_crc1_impl0_ldro1_pay18.cf32 | 8 | 2 | 1 | no | 48454c4c…24c44 |  | 48454c4c…24c44 | cpp_gnur_only |
| golden_vectors/new_batch/tx_sf8_bw125000_cr2_crc1_impl0_ldro2_pay18.cf32 | 8 | 2 | 2 | no | 48454c4c…24c44 | 48454c4c…24c44 | 48454c4c…24c44 | all_match |
