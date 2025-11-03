# Stage 0 – Preparation & Knowledge Consolidation

Timeline kick-off: `<fill when complete>`

## 1. Key PHY Operations & Equations (source: `docs/rev_eng_lora.md`)
- **Whitening**: LFSR polynomial \(x^8 + x^6 + x^5 + x^4 + 1\); byte-level XOR prior to FEC (Figures 1–2). Nibble ordering: payload low-first, header high-first.
- **Header checksum**: Generator matrix \(G \in \mathbb{F}_2^{5 \times 12}\) defines \(h_c = G \cdot h\). Fixed first interleaver block always coded as rate 4/8 with LDRO rules.
- **Payload CRC**: Polynomial \(x^{16} + x^{12} + x^5 + 1\); final two payload bytes XOR with CRC output instead of being fed into CRC.
- **FEC**: Hamming-based generator matrices \(G_0\) (for \(P=1\)) and \(G_1\) (cropped for \(P \in \{2,3,4\}\)); rate options 4/5…4/8. Decoder parity-check matrix \(H\) and syndrome mapping provided; soft-decision LLR formulation uses modified Bessel function \(I_0(\cdot)\).
- **Interleaver**: Diagonal mapping \(I_{i,j} = D((i-(j+1)) \bmod SF,\, i)\) with optional LDRO reduction to \(SF-2\) rows; append parity + zero bit per block.
- **Gray mapping**: \(s = ((I_i \oplus (I_i \gg 1)) + 1) \bmod 2^{SF}\).
- **Modulation**: CSS upchirp expression \(x_s[n]\) with fold index \(n_{\text{fold}} = (2^{SF} - s)\,f_s/B\); symbol duration \(T_s = 2^{SF}/B\).
- **Preamble & frame length**: Sync word encoding \(S_{w,0/1} = ((S_w \gg 4),(S_w\,\&\,0x0F)) \times 8\). Total symbol count \(N_s\) equation capturing payload, CRC, header, LDRO effects; frame duration \(T_{\text{frame}} = N_s 2^{SF}/B\).
- **Synchronization**:
  - Fractional CFO estimator (Bernier/Xhonneux) and compensation exponential.
  - Fractional STO estimator \( \hat{\lambda}_{\text{STO}} \) using spectrum power sums \(P[k]\); resampling by \(\lceil (f_s/B)\hat{\lambda}_{\text{STO}}\rceil\).
  - Integer CFO/STO from up/downchirp demod outputs via \(\Gamma_N[\cdot]\).
  - SFO mitigation drops/duplicates samples every \(\lfloor B / (2\hat{\Delta f_s}) \rfloor\).
- **Demodulation**: Dechirp + FFT; decision \(\hat{s}_l = \arg\max_k |\tilde{Y}_l[k]|\).

## 1.1. Additional Insights from PDF Review
- **Reverse_Eng_Report.pdf**:
  - Whitening matrices show explicit header whitening offset: in explicit mode the sequence shifts by five codewords, confirming whitening starts immediately after the header; implicit-header payload whitening aligns without offset.
  - Demonstrates implicit header fields (`To`, `From`, `Id`, `Flags`) belong to the higher-layer driver, not the LoRa PHY; confirms PHY treats implicit-header frames identically to payload (no header whitening).
  - Payload CRC discovered to use CCITT-16 with the final two payload bytes acting as the post-processing XOR; whitening must *not* be removed before CRC verification to observe the correct sequence.
  - Provides time-domain symbol derivation (Eq. 2.2) with pre-/post-fold expressions, useful when validating custom chirp generators against theoretical phase evolution.
- **rev eng lora.pdf** (conference paper):
  - Reinforces synchronization estimators and reports <1 dB SNR loss between simulation and SDR experiments—sets performance target for the standalone receiver.
- **lorawebinar-lora-1.pdf**:
  - Supplies Shannon-capacity framing for spread-spectrum motivation and tabulated airtime equations; includes worked SF=11 grey-mapping example for verification.

## 2. Reference Assets Identified
- **Algorithm implementations** (`gr_lora_sdr`):
  - C++ blocks in `lib/`: synchronization, demod, whitening, FEC, CRC (verify: `lib/frame_sync_impl.cc`, `lib/fft_demod_impl.cc`, etc.).
  - Headers in `include/gnuradio/lora_sdr/`: API surface for each block.
  - Python bindings in `python/gnuradio/lora_sdr/`: reference for parameter wiring.
  - GRC definitions under `grc/`: metadata/parameter defaults for blocks.
  - Example flowgraphs and scripts: `examples/`, `apps/`.
  - Test vectors: `data/` (verify contents) and any IQ dumps referenced in docs.
- **Documentation**:
  - `docs/rev_eng_lora.md` (detailed PHY paper) — already ingested.
  - PDFs: `rev eng lora.pdf`, `Reverse_Eng_Report.pdf`, `lorawebinar-lora-1.pdf` — key takeaways captured above; revisit for diagrams if we need illustrative figures later.
- **Environment**:
  - `environment.yml` defines pinned tooling (GNU Radio 3.10, GCC 11+, UHD, VOLK) useful for reproducing raw reference outputs.
  - `build/install` tree currently contains compiled GNU Radio blocks for validation harness.

### 2.1 Block Cross-Reference (`gr_lora_sdr`)

| Block (API header) | Core implementation | GRC definition | Role / notes |
|--------------------|---------------------|----------------|--------------|
| `add_crc.h` | `lib/add_crc_impl.cc` | `grc/lora_sdr_add_crc.block.yml` | Appends optional CCITT-16 CRC to payload bytes prior to FEC. |
| `crc_verif.h` | `lib/crc_verif_impl.cc` | `grc/lora_sdr_crc_verif.block.yml` | Recomputes payload CRC (with whitening-aware handling) and exposes pass/fail flags. |
| `data_source.h` | `lib/data_source_impl.cc` | `grc/lora_sdr_data_source.block.yml` | Generates tagged byte streams from files/test patterns. |
| `deinterleaver.h` | `lib/deinterleaver_impl.cc` | `grc/lora_sdr_deinterleaver.block.yml` | Performs diagonal deinterleaving and parity-bit stripping; supports soft metrics. |
| `dewhitening.h` | `lib/dewhitening_impl.cc` | `grc/lora_sdr_dewhitening.block.yml` | Removes LFSR whitening sequence after header decode. |
| `fft_demod.h` | `lib/fft_demod_impl.cc` | `grc/lora_sdr_fft_demod.block.yml` | Dechirps and FFT-demodulates symbols; outputs hard/soft decisions. |
| `frame_sync.h` | `lib/frame_sync_impl.cc` | `grc/lora_sdr_frame_sync.block.yml` | Detects preamble, estimates/corrects CFO/STO/SFO, aligns frame start. |
| `gray_demap.h` | `lib/gray_demap_impl.cc` | `grc/lora_sdr_gray_demap.block.yml` | Maps interleaved bits into Gray-coded symbol indices for modulation. |
| `gray_mapping.h` | `lib/gray_mapping_impl.cc` | `grc/lora_sdr_gray_mapping.block.yml` | Converts demodulated symbols (hard/soft) back to bit metrics. |
| `hamming_dec.h` | `lib/hamming_dec_impl.cc` | `grc/lora_sdr_hamming_dec.block.yml` | Applies hard/soft Hamming decoding for CR 4/5…4/8. |
| `hamming_enc.h` | `lib/hamming_enc_impl.cc` | `grc/lora_sdr_hamming_enc.block.yml` | Generates parity bits per selected coding rate. |
| `header.h` | `lib/header_impl.cc` | `grc/lora_sdr_header.block.yml` | Builds explicit/implicit PHY header codewords. |
| `header_decoder.h` | `lib/header_decoder_impl.cc` | `grc/lora_sdr_header_decoder.block.yml` | Recovers frame parameters, informs synchronization block via `frame_info`. |
| `interleaver.h` | `lib/interleaver_impl.cc` | `grc/lora_sdr_interleaver.block.yml` | Performs diagonal interleaving and parity-bit append; honours LDRO. |
| `modulate.h` | `lib/modulate_impl.cc` | `grc/lora_sdr_modulate.block.yml` | Generates baseband upchirps/downchirps with sync-word embedding. |
| `payload_id_inc.h` | `lib/payload_id_inc_impl.cc` | `grc/lora_sdr_payload_id_inc.block.yml` | Adds packet counters/tags for debugging or harness use. |
| `RH_RF95_header.h` | `lib/RH_RF95_header_impl.cc` | `grc/lora_sdr_RH_RF95_header.block.yml` | Handles RadioHead-compatible header (for RFM95) cosmetics. |
| `whitening.h` | `lib/whitening_impl.cc` | `grc/lora_sdr_whitening.block.yml` | Applies byte-wise LFSR whitening prior to FEC. |
| `no_sfo_frame_sync.h` | `lib/frame_sync_impl.cc` (mode) | — | Variant of frame sync skipping SFO tracking (flag inside implementation). |
| `utilities.h` | Shared helpers (`lib/*`) | — | Lookup tables, math helpers used across blocks. |

Composite flowgraphs:
- `python/lora_sdr/lora_sdr_lora_tx.py` / `.block.yml`: wraps whitening→modulation chain.
- `python/lora_sdr/lora_sdr_lora_rx.py` / `.block.yml`: wraps sync→decode chain for quick reuse.
- Support glue in `python/lora_sdr/lora.py` and `python/lora_sdr/utils.py`.

## 3. Draft Success Metrics / Constraints (software-first, MCU-aware)
- **Throughput target**: Real-time for max planned bandwidth (define explicit value, e.g., 500 kS/s) and SF up to 12 with minimal latency (<1 frame duration).
- **Resource envelope (placeholder)**:
  - Code footprint ≤ 256 kB flash (assumes Cortex-M7/M55 class).
  - RAM budget ≤ 128 kB for buffering + scratch (to refine once timing model built).
  - Allowance for fixed-point arithmetic (Q15/Q31) with optional float fallback.
- **Power proxy**: Favor algorithms compatible with single-core MCU at ≤ 200 MHz; use MAC count per symbol as optimization metric.
- **Correctness**: Bit-exact agreement with `gr_lora_sdr` for all SF ∈ [5,12], code rates 4/5…4/8, optional CRC, LDRO on/off. Synchronization must acquire within preamble length under ±10 ppm CFO and ±5 ppm SFO.
- **Current hardware stance**: No MCU shortlisted yet; aim for implementation choices that keep CPU, RAM, and flash use modest so porting to a mid-range Cortex-M later remains realistic.

## 4. Open Items & Next Actions
- [x] Deep-dive PDFs to capture any hardware nuance, performance plots, or parameter tables missing from Markdown.
- [x] Catalogue exact filenames for key `gr_lora_sdr` blocks (see Table 2.1).
- [x] Decide preferred host-language stack (Python vs. C++/Rust) for simulation harness prior to Stage 1. **Decision**: C++ simulation harness (aligns with eventual MCU port).
- [x] Refine resource envelope once a representative MCU is shortlisted. *Status*: no candidate selected yet; default to low-footprint design and revisit when hardware is in scope.
- [x] Determine availability of existing IQ captures; if missing, plan to generate with current GNU Radio setup (Section 6).

## 5. Stage Completion Checklist
- Summaries above validated with primary references (Markdown + PDFs).
- Asset inventory reviewed with repo tree owners; no critical gaps.
- Success metrics endorsed (or flagged for revision) before entering Stage 1.

## 6. Golden IQ Capture Plan
- **Generated baseline**: `gr_lora_sdr/examples/tx_rx_simulation.py` now uses repo-relative assets, adds a tagged-stream adapter, and records the channel output via a vector sink. Running  
  `PYTHONPATH=$PWD/gr_lora_sdr/install/lib/python3.12/site-packages LD_LIBRARY_PATH=$PWD/gr_lora_sdr/install/lib LORA_AUTOSTOP_SECS=5 conda run -n gr310 python gr_lora_sdr/examples/tx_rx_simulation.py`  
  produces `gr_lora_sdr/data/generated/tx_rx_simulation.cf32` (~25 M complex samples for SF7/SNR = −5 dB). `.gitignore` excludes `gr_lora_sdr/data/generated/`.
- **GNU Radio verification**: `gr_lora_sdr/tools/verify_cf32.py` replays the capture through the reference blocks and prints recovered payloads. Example:  
  `PYTHONPATH=... LD_LIBRARY_PATH=... conda run -n gr310 python gr_lora_sdr/tools/verify_cf32.py --input gr_lora_sdr/data/generated/tx_rx_simulation.cf32 --max-frames 3`
- **Batch generation**: `gr_lora_sdr/tools/generate_vectors.py` sweeps a handful of SF/BW/CR/SNR combinations using the same flowgraph and writes `.cf32` plus `.json` metadata files to `data/generated/`.
- **Host harness**: `./build/host_sim/lora_replay --iq <file> --metadata <file.json> --dump-symbols symbols.txt` aligns the capture, emits demodulated symbols, and can dump aligned IQ for later diffing.
- **Parameter tweaks**: Adjust `pay_len`, SNR, SF, or `LORA_AUTOSTOP_SECS` in the script to sweep additional conditions; rerun to collect per-configuration IQ captures.
- **Stage 1 harness**: Convert these `.cf32` dumps into golden vectors consumed by the forthcoming C++ simulation harness (encode/decode stage-by-stage comparisons).
- **Hardware captures** (future): `examples/tx_rx_usrp.py` remains the template once SDR hardware is available.
