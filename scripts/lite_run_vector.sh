#!/usr/bin/env bash
set -euo pipefail

VEC="vectors/bw_125k_sf_7_cr_1_ldro_false_crc_true_implheader_false.unknown"
SF=7
CR=45
SYNC=0x12
LOGS=logs
mkdir -p "$LOGS"

echo "[LoRa Lite] Decoding $VEC (sf=$SF cr=$CR sync=$SYNC)"
./build/lora_decode \
  --in "$VEC" \
  --sf "$SF" --cr "$CR" --sync "$SYNC" --json \
  1> "$LOGS/lite_ld.json" 2> "$LOGS/lite_ld.err" || true

tail -n 40 "$LOGS/lite_ld.json" || true
echo "[LoRa Lite] JSON written to $LOGS/lite_ld.json"

