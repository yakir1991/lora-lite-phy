#!/usr/bin/env bash
set -euo pipefail

# This helper regenerates golden IQ/payload vectors by invoking the
# reference GNU Radio flowgraph shipped with `gr_lora_sdr`.  The produced
# files live under `vectors/` and are later consumed by unit tests to
# cross‑validate the local TX/RX implementation.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="$ROOT/vectors"
mkdir -p "$OUT_DIR"

# Spreading factor / code rate pairs to export. Code rate uses LoRa
# notation (45 = 4/5, ...).  Additional pairs can be supplied by setting
# the PAIRS environment variable before calling this script.
PAIRS=(
  "7 45"
  "8 48"
  ${PAIRS_EXTRA:-}
)

for entry in "${PAIRS[@]}"; do
  sf=$(echo "$entry" | awk '{print $1}')
  cr=$(echo "$entry" | awk '{print $2}')
  payload_file="$OUT_DIR/sf${sf}_cr${cr}_payload.bin"
  iq_file="$OUT_DIR/sf${sf}_cr${cr}_iq.bin"

  # Create deterministic 4-byte payload if none exists
  if [ ! -f "$payload_file" ]; then
    python3 - <<'PY'
import os,sys
payload=bytes(range(1,5))
with open(sys.argv[1], 'wb') as f:
    f.write(payload)
PY
 "$payload_file"
  fi

  # Ensure GNU Radio reference model is available
  python3 - <<'PY'
try:
    import gnuradio
    import gnuradio.lora_sdr
except Exception as e:
    raise SystemExit("ERROR: GNU Radio or gnuradio.lora_sdr is not available in this environment.\n"
                     "Install via conda (recommended) and re-run export.")
PY

  # Generate IQ using the GNU Radio TX-only flowgraph (no preamble)
  python3 "$ROOT/scripts/gr_generate_vectors.py" \
    --sf "$sf" --cr "$cr" --payload "$payload_file" --out "$iq_file" \
    --bw 125000 --samp-rate 125000 --preamble-len 8

done
