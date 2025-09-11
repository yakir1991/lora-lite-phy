#!/usr/bin/env bash
set -euo pipefail

# Detailed GR LoRa SDR run for a vector with OS=2 (SR=250k at BW=125k)
# Mirrors the lite_run_vector.sh experience with clear steps and artifact dumps.

VEC="${1:-vectors/bw_125k_sf_7_cr_1_ldro_false_crc_true_implheader_false.unknown}"
SF=${SF:-7}
CR=${CR:-45}
BW=${BW:-125000}
SR=${SR:-250000}
SYNC=${SYNC:-0x12}
LOGS=${LOGS:-logs}

mkdir -p "$LOGS"

echo "[Step 1/5] Activating conda (if available)"
if [ -f "$HOME/miniconda3/etc/profile.d/conda.sh" ]; then
  # shellcheck disable=SC1090
  source "$HOME/miniconda3/etc/profile.d/conda.sh"
  conda activate gnuradio-lora || true
else
  echo "  miniconda3 not found; using system python"
fi

echo "[Step 2/5] Running GNU Radio LoRa SDR RX-only"
echo "  VEC=$VEC SF=$SF CR=$CR BW=$BW SR=$SR SYNC=$SYNC"
python3 scripts/gr_original_rx_only.py \
  --in-iq "$VEC" \
  --sf "$SF" --cr "$CR" --bw "$BW" --samp-rate "$SR" --pay-len 255 --sync "$SYNC" \
  --out-rx-payload "$LOGS/gr_rx_payload.bin" \
  --out-predew "$LOGS/gr_predew.bin" \
  --out-postdew "$LOGS/gr_postdew.bin" \
  --out-hdr-gray "$LOGS/gr_hdr_gray.bin" \
  --out-hdr-nibbles "$LOGS/gr_hdr_nibbles.bin" \
  --out-raw-bins "$LOGS/gr_raw_bins.bin" \
  --out-deint-bits "$LOGS/gr_deint_bits.bin" \
  | tee "$LOGS/gr_rx_only.json"

echo "[Step 3/5] Artifact sizes"
ls -lh "$LOGS/gr_hdr_gray.bin" "$LOGS/gr_hdr_nibbles.bin" \
       "$LOGS/gr_predew.bin" "$LOGS/gr_postdew.bin" "$LOGS/gr_rx_payload.bin"

echo "[Step 4/5] Quick hex previews"
echo "  Header nibbles (first 32 bytes):"
xxd -g 1 "$LOGS/gr_hdr_nibbles.bin" | head -n 4 || true
echo "  Header gray (first 16 shorts):"
od -An -tu2 -N 32 "$LOGS/gr_hdr_gray.bin" | head -n 2 || true
echo "  Raw bins (first 16 shorts):"
od -An -tu2 -N 32 "$LOGS/gr_raw_bins.bin" | head -n 2 || true
echo "  Pre-dewhitening (first 32 bytes):"
xxd -g 1 "$LOGS/gr_predew.bin" | head -n 4 || true
echo "  Post-dewhitening (first 32 bytes):"
xxd -g 1 "$LOGS/gr_postdew.bin" | head -n 4 || true
echo "  RX payload (first 32 bytes):"
xxd -g 1 "$LOGS/gr_rx_payload.bin" | head -n 4 || true

echo "[Step 5/5] Done. JSON + binaries in $LOGS/"
