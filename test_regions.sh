#!/bin/bash

# Extract specific regions from the hello world vector for testing
echo "Extracting high-energy regions from hello world vector..."

cd /home/yakirqaq/projects/lora-lite-phy

# Create temp directory
mkdir -p temp

python3 << 'EOF'
import struct
import numpy as np

# Load the full file
with open('vectors/sps_500k_bw_125k_sf_7_cr_2_ldro_false_crc_true_implheader_false_hello_stupid_world.unknown', 'rb') as f:
    data = f.read()

samples = struct.unpack('<{}f'.format(len(data)//4), data)
complex_samples = np.array([samples[i] + 1j*samples[i+1] for i in range(0, len(samples), 2)])

print(f"Total samples: {len(complex_samples)}")

# Extract regions around high-energy areas with some context
regions = [
    (15000, 3000, "region1_start15k"),    # Around sample 16000-16384
    (54000, 3000, "region2_start54k"),    # Around sample 55000-55424  
]

for start, length, name in regions:
    if start + length > len(complex_samples):
        length = len(complex_samples) - start
    
    region = complex_samples[start:start+length]
    
    # Save as cf32 format (interleaved real/imag float32)
    with open(f'temp/{name}.cf32', 'wb') as f:
        for sample in region:
            f.write(struct.pack('<f', sample.real))
            f.write(struct.pack('<f', sample.imag))
    
    print(f"Saved {name}: {len(region)} samples from {start} to {start+length-1}")

EOF

echo "Testing region extracts with debug_frame_step..."
for file in temp/*.cf32; do
    if [ -f "$file" ]; then
        echo "=== Testing $file ==="
        ./build_standalone/debug_frame_step "$file" 7 | head -20
        echo
    fi
done
