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

  # Invoke reference GNU Radio flowgraph
  python3 "$ROOT/external/gr_lora_sdr/apps/simulation/flowgraph/tx_rx_simulation.py" \
    "$payload_file" /tmp/rx_payload.bin /tmp/rx_crc.bin \
    --sf "$sf" --cr $((cr-40)) --pay-len "$(stat -c%s "$payload_file")" --SNRdB 30

  # Flowgraph dumps TX IQ to tmp1.bin when probes are enabled
  if [ -f tmp1.bin ]; then
    mv tmp1.bin "$iq_file"
  fi

done
