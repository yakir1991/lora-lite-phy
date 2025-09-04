#!/usr/bin/env bash
set -euo pipefail

# Regenerate golden IQ/payload vectors using the GNU Radio reference (gr_lora_sdr).
# Outputs go to vectors/ and are used by the unit tests.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="$ROOT/vectors"
mkdir -p "$OUT_DIR"

# Default SF/CR pairs; you can append via:
#   PAIRS="9 45 10 48" or PAIRS_EXTRA="9 45" (both supported)
DEFAULT_PAIRS=("7 45" "8 48")
read -r -a ENV_PAIRS <<< "${PAIRS:-}"
read -r -a ENV_PAIRS_EXTRA <<< "${PAIRS_EXTRA:-}"
PAIRS=( "${DEFAULT_PAIRS[@]}" "${ENV_PAIRS[@]}" "${ENV_PAIRS_EXTRA[@]}" )

# Ensure GNU Radio reference is available in this conda env
python3 - <<'PY'
import sys
try:
    import gnuradio, gnuradio.lora_sdr
except Exception as e:
    sys.exit(f"ERROR: GNU Radio or gnuradio.lora_sdr is missing: {e}")
PY

for entry in "${PAIRS[@]}"; do
  sf="${entry%% *}"
  cr="${entry##* }"
  payload_file="$OUT_DIR/sf${sf}_cr${cr}_payload.bin"
  iq_file="$OUT_DIR/sf${sf}_cr${cr}_iq.bin"

  # Create deterministic 16-byte payload if none exists (0x01..0x10)
  if [[ ! -f "$payload_file" ]]; then
    python3 -c 'import sys; open(sys.argv[1],"wb").write(bytes(range(1,17)))' "$payload_file"
  fi

  echo "[*] Generating IQ: SF=$sf CR=$cr -> $iq_file"
  if ! timeout 120s python3 "$ROOT/scripts/gr_generate_vectors.py" \
      --sf "$sf" --cr "$cr" \
      --payload "$payload_file" \
      --out "$iq_file" \
      --bw 125000 --samp-rate 125000 --preamble-len 8; then
    echo "Primary generator failed or timed out. Falling back to GRC path..." >&2
    bash "$ROOT/scripts/export_vectors_grc.sh"
    break
  fi
done

ls -lh "$OUT_DIR"/*.bin
echo "Done."
