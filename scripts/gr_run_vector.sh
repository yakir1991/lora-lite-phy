#!/usr/bin/env bash
set -euo pipefail

VEC="vectors/bw_125k_sf_7_cr_1_ldro_false_crc_true_implheader_false.unknown"
SF=7
CR=45
BW=125000
SR=250000
SYNC=0x12
LOGS=logs
mkdir -p "$LOGS"

if [ -f "$HOME/miniconda3/etc/profile.d/conda.sh" ]; then
  # shellcheck disable=SC1090
  source "$HOME/miniconda3/etc/profile.d/conda.sh"
  conda activate gnuradio-lora
fi

echo "[GR] Running RX-only on $VEC (sf=$SF cr=$CR bw=$BW sr=$SR sync=$SYNC)"
python3 scripts/gr_original_rx_only.py \
  --in-iq "$VEC" \
  --sf "$SF" --cr "$CR" --bw "$BW" --samp-rate "$SR" --pay-len 255 --sync "$SYNC" \
  --out-rx-payload "$LOGS/gr_rx_payload.bin" \
  --out-predew "$LOGS/gr_predew.bin" \
  --out-postdew "$LOGS/gr_postdew.bin" \
  --out-hdr-gray "$LOGS/gr_hdr_gray.bin" \
  --out-hdr-nibbles "$LOGS/gr_hdr_nibbles.bin" \
  | tee "$LOGS/gr_rx_only.json"

echo "[GR] Artifacts in $LOGS: gr_hdr_gray.bin, gr_hdr_nibbles.bin, gr_predew.bin, gr_postdew.bin, gr_rx_payload.bin"

