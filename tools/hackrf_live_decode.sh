#!/usr/bin/env bash
# hackrf_live_decode.sh — Capture from HackRF and decode LoRa in real-time.
#
# Usage:
#   ./tools/hackrf_live_decode.sh [OPTIONS]
#
# Options:
#   -f FREQ_HZ      Center frequency in Hz         (default: 868100000)
#   -s SAMPLE_RATE   Sample rate in Hz              (default: 2000000)
#   -d DURATION      Capture duration in seconds    (default: 10)
#   -l LNA_GAIN      HackRF LNA gain 0-40 dB       (default: 32)
#   -g VGA_GAIN      HackRF IF/VGA gain 0-62 dB    (default: 32)
#   -m METADATA      Path to metadata JSON          (required)
#   -M               Multi-packet mode
#   -S               Soft-decision decoding
#   -v               Verbose output
#   -h               Show help
#
# Examples:
#   # Basic: capture 10 seconds and decode
#   ./tools/hackrf_live_decode.sh -m metadata.json -d 10
#
#   # Multi-packet with soft decoding
#   ./tools/hackrf_live_decode.sh -m metadata.json -d 30 -M -S
#
#   # Custom frequency and gain
#   ./tools/hackrf_live_decode.sh -f 915000000 -m metadata.json -l 40 -g 40
#
# Requirements:
#   - hackrf_transfer in PATH (from HackRF tools)
#   - lora_replay built at build/host_sim/lora_replay

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
LORA_REPLAY="$REPO_ROOT/build/host_sim/lora_replay"

# Defaults
FREQ_HZ=868100000
SAMPLE_RATE=2000000
DURATION=10
LNA_GAIN=32
VGA_GAIN=32
METADATA=""
MULTI=""
SOFT=""
VERBOSE=""

usage() {
    head -n 30 "$0" | grep '^#' | sed 's/^# \?//'
    exit 0
}

while getopts "f:s:d:l:g:m:MSvh" opt; do
    case $opt in
        f) FREQ_HZ="$OPTARG" ;;
        s) SAMPLE_RATE="$OPTARG" ;;
        d) DURATION="$OPTARG" ;;
        l) LNA_GAIN="$OPTARG" ;;
        g) VGA_GAIN="$OPTARG" ;;
        m) METADATA="$OPTARG" ;;
        M) MULTI="--multi" ;;
        S) SOFT="--soft" ;;
        v) VERBOSE="--verbose" ;;
        h) usage ;;
        *) usage ;;
    esac
done

if [[ -z "$METADATA" ]]; then
    echo "ERROR: --metadata (-m) is required" >&2
    exit 1
fi

if ! command -v hackrf_transfer &>/dev/null; then
    echo "ERROR: hackrf_transfer not found in PATH" >&2
    echo "Install HackRF tools: sudo apt install hackrf" >&2
    exit 1
fi

if [[ ! -x "$LORA_REPLAY" ]]; then
    echo "ERROR: lora_replay not found at $LORA_REPLAY" >&2
    echo "Build with: cd build && ninja" >&2
    exit 1
fi

N_SAMPLES=$((SAMPLE_RATE * DURATION))

echo "=== HackRF Live LoRa Decode ===" >&2
echo "  Frequency:   $FREQ_HZ Hz" >&2
echo "  Sample rate: $SAMPLE_RATE Hz" >&2
echo "  Duration:    $DURATION s ($N_SAMPLES samples)" >&2
echo "  LNA gain:    $LNA_GAIN dB" >&2
echo "  VGA gain:    $VGA_GAIN dB" >&2
echo "  Metadata:    $METADATA" >&2
echo "  Capturing..." >&2

# hackrf_transfer -r /dev/stdout outputs int8 IQ pairs.
# Pipe directly to lora_replay with --format hackrf.
hackrf_transfer \
    -r /dev/stdout \
    -f "$FREQ_HZ" \
    -s "$SAMPLE_RATE" \
    -l "$LNA_GAIN" \
    -g "$VGA_GAIN" \
    -n "$N_SAMPLES" \
    2>/dev/null \
| "$LORA_REPLAY" \
    --iq - \
    --format hackrf \
    --metadata "$METADATA" \
    $MULTI $SOFT $VERBOSE
