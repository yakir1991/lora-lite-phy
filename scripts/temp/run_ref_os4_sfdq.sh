#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

out="build/ref_os4_sfdq.bin"
echo "[gen] $out"
./build/gen_frame_vectors --sf 7 --cr 45 \
  --payload vectors/sf7_cr45_payload.bin \
  --out "$out" --os 4 --preamble 8

echo "[decode] $out"
./build/lora_decode --in "$out" --sf 7 --cr 45 --json 1> logs/ld.json 2> logs/ld.err || true
tail -n 200 logs/ld.json || true

