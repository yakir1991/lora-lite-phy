#!/bin/bash
# Capture LoRa packets from RFM95W TX
# Usage: ./tools/hackrf_capture_lora.sh [duration_seconds]

set -e

DURATION=${1:-30}
FREQ=868100000
SAMPLE_RATE=2000000
OUTPUT_DIR="build/lora_capture_$(date +%Y%m%d_%H%M%S)"

mkdir -p "$OUTPUT_DIR"

echo "╔════════════════════════════════════════════════════════════╗"
echo "║         HackRF LoRa Capture from RFM95W                    ║"
echo "╚════════════════════════════════════════════════════════════╝"
echo ""
echo "Duration: ${DURATION}s"
echo "Frequency: 868.1 MHz"
echo "Output: $OUTPUT_DIR"
echo ""
echo "Make sure Arduino is transmitting LoRa packets!"
echo ""

# Capture
RAW_FILE="$OUTPUT_DIR/capture.raw"
echo "[1/4] Capturing ${DURATION} seconds..."
hackrf_transfer -r "$RAW_FILE" \
    -f $FREQ \
    -s $SAMPLE_RATE \
    -l 32 -g 32 \
    -n $((SAMPLE_RATE * DURATION))

# Convert to CF32
echo "[2/4] Converting to CF32..."
CF32_FILE="$OUTPUT_DIR/capture.cf32"
python3 << EOF
import numpy as np
raw = np.fromfile("$RAW_FILE", dtype=np.int8)
iq = raw[0::2].astype(np.float32) + 1j * raw[1::2].astype(np.float32)
iq /= 128.0
iq.astype(np.complex64).tofile("$CF32_FILE")
print(f"   Converted {len(iq):,} samples")
EOF

# Create metadata
echo "[3/4] Creating metadata..."
cat > "$OUTPUT_DIR/metadata.json" << 'METADATA'
{
    "sf": 7,
    "bw": 125000,
    "cr": 1,
    "crc_enabled": true,
    "implicit_header": false,
    "sample_rate": 2000000,
    "source": "rfm95w_arduino"
}
METADATA

# Analyze and try to decode
echo "[4/4] Analyzing capture..."
python3 << EOF
import numpy as np
import subprocess
import json

iq = np.fromfile("$CF32_FILE", dtype=np.complex64)
sample_rate = 2000000

# Power analysis
chunk_size = 2048
num_chunks = len(iq) // chunk_size
power_db = np.zeros(num_chunks)

for i in range(num_chunks):
    chunk = iq[i*chunk_size:(i+1)*chunk_size]
    power_db[i] = 10 * np.log10(np.mean(np.abs(chunk)**2) + 1e-10)

noise_floor = np.percentile(power_db, 20)
peak_power = np.max(power_db)

print(f"\n📊 Signal Analysis:")
print(f"   Samples: {len(iq):,}")
print(f"   Duration: {len(iq)/sample_rate:.1f}s")
print(f"   Noise floor: {noise_floor:.1f} dB")
print(f"   Peak power: {peak_power:.1f} dB")
print(f"   Dynamic range: {peak_power - noise_floor:.1f} dB")

# Find bursts
threshold = noise_floor + 6
bursts = power_db > threshold
burst_starts = np.where(np.diff(bursts.astype(int)) == 1)[0]
burst_ends = np.where(np.diff(bursts.astype(int)) == -1)[0]

if len(burst_ends) < len(burst_starts):
    burst_ends = np.append(burst_ends, num_chunks - 1)

lora_bursts = []
for start, end in zip(burst_starts, burst_ends):
    duration_ms = (end - start) * chunk_size / sample_rate * 1000
    if duration_ms > 10:
        lora_bursts.append({
            "start_sample": int(start * chunk_size),
            "end_sample": int(end * chunk_size),
            "duration_ms": float(duration_ms),
            "peak_power_db": float(power_db[start:end].max())
        })

print(f"\n🔍 Burst Detection:")
print(f"   Total bursts: {len(burst_starts)}")
print(f"   LoRa-like (>10ms): {len(lora_bursts)}")

if lora_bursts:
    print(f"\n📦 Detected LoRa packets:")
    for i, burst in enumerate(lora_bursts[:10]):
        t = burst["start_sample"] / sample_rate
        print(f"   [{i+1}] t={t:.2f}s, {burst['duration_ms']:.0f}ms, {burst['peak_power_db']:.1f}dB")
    
    # Save burst info
    with open("$OUTPUT_DIR/bursts.json", "w") as f:
        json.dump(lora_bursts, f, indent=2)
else:
    print("\n⚠️ No LoRa packets detected")
EOF

echo ""
echo "════════════════════════════════════════════════════════════"
echo "Done! Results in: $OUTPUT_DIR"
echo ""
echo "To decode, run:"
echo "  ./build/host_sim/lora_replay --iq $CF32_FILE --metadata $OUTPUT_DIR/metadata.json"
echo "════════════════════════════════════════════════════════════"
