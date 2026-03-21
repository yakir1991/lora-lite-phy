#!/usr/bin/env bash
#
# Helper for CI/nightly jobs to shard the receiver-vs-GNU Radio comparison
# matrix across smaller capture groups. Each invocation runs a deterministic
# subset and writes its JSON results under build/. Optionally the freshly
# generated batch can be merged back into docs/receiver_vs_gnuradio_results.json
# so dashboards have up-to-date data.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

MATRIX="${MATRIX_PATH:-$REPO_ROOT/docs/receiver_vs_gnuradio_matrix.json}"
CAPTURE_ROOT="${CAPTURE_ROOT:-$REPO_ROOT/gr_lora_sdr/data/generated}"
WORK_DIR="${WORK_DIR:-$REPO_ROOT/build/receiver_vs_gnuradio_batches}"
OUTPUT_DIR="${OUTPUT_DIR:-$REPO_ROOT/build}"
MERGE_TARGET="${MERGE_TARGET:-$REPO_ROOT/docs/receiver_vs_gnuradio_results.json}"
GNURADIO_ENV="${GNURADIO_ENV:-gr310}"
LORA_REPLAY_BIN="${LORA_REPLAY_BIN:-$REPO_ROOT/build/host_sim/lora_replay}"
GR_SCRIPT="${GR_SCRIPT:-$REPO_ROOT/tools/gr_decode_capture.py}"
MC_RUNS="${MC_RUNS:-1}"

usage() {
    cat <<EOF
Usage: $(basename "$0") --batch <mid|low|high> [--merge]

Environment overrides:
  MATRIX_PATH          Path to receiver_vs_gnuradio_matrix.json (default: $MATRIX)
  CAPTURE_ROOT         Root directory for CF32 captures (default: $CAPTURE_ROOT)
  WORK_DIR             Working directory for impaired IQ (default: $WORK_DIR)
  OUTPUT_DIR           Where batch JSON files are written (default: $OUTPUT_DIR)
  MERGE_TARGET         Path to consolidated JSON (default: $MERGE_TARGET)
  GNURADIO_ENV         Conda env hosting GNU Radio (default: $GNURADIO_ENV)
  LORA_REPLAY_BIN      Path to host_sim lora_replay binary (default: $LORA_REPLAY_BIN)
  GR_SCRIPT            Path to GNU Radio decode helper (default: $GR_SCRIPT)
  MC_RUNS              Monte Carlo iterations per capture/profile (default: $MC_RUNS)
EOF
}

BATCH_NAME=""
DO_MERGE=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --batch)
            shift
            BATCH_NAME="${1:-}"
            ;;
        --merge)
            DO_MERGE=1
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage
            exit 1
            ;;
    esac
    shift || true
done

if [[ -z "$BATCH_NAME" ]]; then
    echo "Error: --batch is required" >&2
    usage
    exit 1
fi

declare -A CAPTURE_BATCHES
CAPTURE_BATCHES[mid]="sf7_bw125_cr1_short sf9_bw125_cr3_short sf8_bw125_cr4_payload128 sf9_bw125_cr1_implicithdr_nocrc sf8_bw500_cr3_long sf9_bw500_cr2_snrm10 sf7_bw500_cr2_implicithdr_nocrc"
CAPTURE_BATCHES[low]="sf5_bw125_cr1_full sf5_bw125_cr2_full sf6_bw125_cr1_full sf6_bw125_cr2_full sf6_bw62500_cr4_ldro sf7_bw62500_cr3_implicithdr_nocrc sf7_bw125_cr2_full"
CAPTURE_BATCHES[high]="sf10_bw250_cr4_full sf10_bw500_cr4_highsnr sf10_bw62500_cr2_ldro sf11_bw125_cr1_short sf12_bw125_cr1_short sf12_bw500_cr4_payload256 sf7_bw62500_cr1_short"

CAPTURE_LIST="${CAPTURE_BATCHES[$BATCH_NAME]:-}"
if [[ -z "$CAPTURE_LIST" ]]; then
    echo "Unknown batch '$BATCH_NAME'. Expected one of: ${!CAPTURE_BATCHES[*]}" >&2
    exit 1
fi

mkdir -p "$WORK_DIR" "$OUTPUT_DIR"
OUTPUT_JSON="$OUTPUT_DIR/receiver_vs_gnuradio_${BATCH_NAME}.json"

echo "[CI] Running batch '$BATCH_NAME' -> $OUTPUT_JSON"
python "$REPO_ROOT/tools/run_receiver_vs_gnuradio.py" \
    "$MATRIX" \
    --capture-root "$CAPTURE_ROOT" \
    --work-dir "$WORK_DIR/$BATCH_NAME" \
    --captures $CAPTURE_LIST \
    --output-json "$OUTPUT_JSON" \
    --gnuradio-env "$GNURADIO_ENV" \
    --lora-replay "$LORA_REPLAY_BIN" \
    --gr-script "$GR_SCRIPT" \
    --mc-runs "$MC_RUNS"

if [[ $DO_MERGE -eq 1 ]]; then
    echo "[CI] Merging $OUTPUT_JSON into $MERGE_TARGET"
    python - "$OUTPUT_JSON" "$MERGE_TARGET" <<'PY'
import json
import sys
from pathlib import Path

output_path = Path(sys.argv[1])
merge_target = Path(sys.argv[2])

if not output_path.exists():
    raise SystemExit(f"Batch results missing: {output_path}")

batch_entries = json.loads(output_path.read_text())
existing = {}
if merge_target.exists():
    for item in json.loads(merge_target.read_text()):
        key = (
            item.get("capture"),
            item.get("profile"),
            item.get("mc_iteration"),
            item.get("mc_seed"),
        )
        existing[key] = item

for item in batch_entries:
    key = (
        item.get("capture"),
        item.get("profile"),
        item.get("mc_iteration"),
        item.get("mc_seed"),
    )
    existing[key] = item

merged = [existing[key] for key in sorted(existing)]
merge_target.parent.mkdir(parents=True, exist_ok=True)
merge_target.write_text(json.dumps(merged, indent=2) + "\n")
print(f"Merged {len(batch_entries)} entries; merged total {len(merged)} -> {merge_target}")
PY
fi

echo "[CI] Batch '$BATCH_NAME' complete"
