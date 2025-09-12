#!/usr/bin/env bash
# Generate logs via run_vector_compare and convert Lite logs to CW bytes
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

# run comparison to produce logs
python3 scripts/run_vector_compare.py >/dev/null

# convert and compare CW bytes
python3 scripts/lite_log_to_cw.py --log logs/lite_ld.json --gr-nibbles logs/gr_hdr_nibbles.bin

