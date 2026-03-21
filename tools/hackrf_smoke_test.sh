#!/bin/bash
# Quick HackRF smoke test for LoRa transceiver validation
# Usage: ./tools/hackrf_smoke_test.sh

set -e

echo "╔════════════════════════════════════════════════════════════╗"
echo "║           HackRF LoRa Quick Smoke Test                     ║"
echo "╚════════════════════════════════════════════════════════════╝"
echo ""

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

WORK_DIR="${1:-build/hackrf_smoke_test}"
FREQ=${FREQ:-868100000}
SAMPLE_RATE=${SAMPLE_RATE:-2000000}
DURATION=${DURATION:-2}

mkdir -p "$WORK_DIR"

# Step 1: Check HackRF connection
echo -e "${YELLOW}[1/5] Checking HackRF connection...${NC}"
if hackrf_info > "$WORK_DIR/hackrf_info.txt" 2>&1; then
    SERIAL=$(grep "Serial number" "$WORK_DIR/hackrf_info.txt" | head -1)
    echo -e "${GREEN}✓ HackRF found: $SERIAL${NC}"
else
    echo -e "${RED}✗ HackRF not found. Is it connected?${NC}"
    cat "$WORK_DIR/hackrf_info.txt"
    exit 1
fi

# Step 2: Short capture
echo -e "${YELLOW}[2/5] Capturing ${DURATION}s @ $(echo "scale=3; $FREQ/1000000" | bc) MHz...${NC}"
RAW_FILE="$WORK_DIR/capture.raw"
hackrf_transfer -r "$RAW_FILE" \
    -f "$FREQ" \
    -s "$SAMPLE_RATE" \
    -l 32 -g 20 \
    -n $((SAMPLE_RATE * DURATION)) 2>&1 | tee "$WORK_DIR/capture_log.txt"

if [ ! -f "$RAW_FILE" ] || [ ! -s "$RAW_FILE" ]; then
    echo -e "${RED}✗ Capture failed${NC}"
    exit 1
fi
echo -e "${GREEN}✓ Captured $(ls -lh "$RAW_FILE" | awk '{print $5}')${NC}"

# Step 3: Convert to CF32
echo -e "${YELLOW}[3/5] Converting to CF32...${NC}"
CF32_FILE="$WORK_DIR/capture.cf32"
python3 << EOF
import numpy as np
raw = np.fromfile("$RAW_FILE", dtype=np.int8)
iq = raw[0::2].astype(np.float32) + 1j * raw[1::2].astype(np.float32)
iq /= 128.0
iq.astype(np.complex64).tofile("$CF32_FILE")
print(f"Converted {len(iq):,} samples")
EOF
echo -e "${GREEN}✓ Converted to $CF32_FILE${NC}"

# Step 4: Quick spectrum analysis
echo -e "${YELLOW}[4/5] Analyzing spectrum...${NC}"
python3 << EOF
import numpy as np
import json

iq = np.fromfile("$CF32_FILE", dtype=np.complex64)
sample_rate = $SAMPLE_RATE

# Power analysis
power_db = 10 * np.log10(np.mean(np.abs(iq)**2) + 1e-10)
peak_db = 10 * np.log10(np.max(np.abs(iq)**2) + 1e-10)
noise_floor = 10 * np.log10(np.percentile(np.abs(iq)**2, 10) + 1e-10)

# FFT for dominant frequency
fft_len = min(65536, len(iq))
fft = np.fft.fftshift(np.fft.fft(iq[:fft_len]))
fft_mag = np.abs(fft)
peak_bin = np.argmax(fft_mag)
freq_offset = (peak_bin - fft_len // 2) * sample_rate / fft_len

print(f"  Samples:       {len(iq):,}")
print(f"  Duration:      {len(iq)/sample_rate:.2f} s")
print(f"  Mean Power:    {power_db:.1f} dB")
print(f"  Peak Power:    {peak_db:.1f} dB")
print(f"  Noise Floor:   {noise_floor:.1f} dB")
print(f"  SNR (est):     {peak_db - noise_floor:.1f} dB")
print(f"  Freq Offset:   {freq_offset:.0f} Hz")

# Save analysis
analysis = {
    "samples": int(len(iq)),
    "duration_s": float(len(iq)/sample_rate),
    "mean_power_db": float(power_db),
    "peak_power_db": float(peak_db),
    "noise_floor_db": float(noise_floor),
    "snr_db": float(peak_db - noise_floor),
    "freq_offset_hz": float(freq_offset),
}
with open("$WORK_DIR/spectrum_analysis.json", "w") as f:
    json.dump(analysis, f, indent=2)
EOF
echo -e "${GREEN}✓ Spectrum analysis saved${NC}"

# Step 5: Check if host decoder exists
echo -e "${YELLOW}[5/5] Checking decoder availability...${NC}"
HOST_BIN="build/host_sim/lora_replay"
if [ -f "$HOST_BIN" ]; then
    echo -e "${GREEN}✓ Host decoder found: $HOST_BIN${NC}"
    
    # Create minimal metadata
    cat > "$WORK_DIR/metadata.json" << 'METADATA'
{
    "sf": 7,
    "bw": 125000,
    "cr": 1,
    "crc_enabled": true,
    "implicit_header": false,
    "sample_rate": 2000000
}
METADATA
    
    echo "  Attempting decode..."
    if timeout 30 "$HOST_BIN" \
        --iq "$CF32_FILE" \
        --metadata "$WORK_DIR/metadata.json" \
        --summary-json "$WORK_DIR/decode_summary.json" \
        --ignore-ref-mismatch 2>&1 | tee "$WORK_DIR/decode_log.txt"; then
        
        if [ -f "$WORK_DIR/decode_summary.json" ]; then
            echo -e "${GREEN}✓ Decode complete - check $WORK_DIR/decode_summary.json${NC}"
        else
            echo -e "${YELLOW}⚠ Decode ran but no packets found (expected if no LoRa TX active)${NC}"
        fi
    else
        echo -e "${YELLOW}⚠ Decode failed or timed out${NC}"
    fi
else
    echo -e "${YELLOW}⚠ Host decoder not built. Run: cmake --build build${NC}"
fi

echo ""
echo "════════════════════════════════════════════════════════════"
echo -e "${GREEN}Smoke test complete!${NC}"
echo "Results in: $WORK_DIR/"
echo ""
echo "Next steps:"
echo "  1. Connect your LoRa TX with attenuators"
echo "  2. Run: python tools/hackrf_quick_test.py --iq $CF32_FILE --sf 7 --compare-gnuradio"
echo "════════════════════════════════════════════════════════════"
