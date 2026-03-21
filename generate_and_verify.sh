#!/bin/bash
set -e

# Setup environment
export PYTHONPATH=/home/yakirqaq/projects/lora-lite-phy/gr_lora_sdr/install_sys/local/lib/python3.10/dist-packages:$PYTHONPATH
export LD_LIBRARY_PATH=/home/yakirqaq/projects/lora-lite-phy/gr_lora_sdr/install_sys/lib:/home/yakirqaq/projects/lora-lite-phy/gr_lora_sdr/install_sys/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH

# Create payload
echo -n "Hello LoRa!" > payload.txt

# Configuration
export LORA_SF=10
export LORA_BW=250000
export LORA_CR=4
export LORA_PAYLOAD_SOURCE=payload.txt
export LORA_PAYLOAD_LEN=11
export LORA_AUTOSTOP_SECS=2
export LORA_OUTPUT_DIR=output
export LORA_OUTPUT_NAME=test_vector.cf32
export LORA_DUMP_DEBUG=1
export LORA_SNR_DB=100 # High SNR for clean signal

# Run GNU Radio Simulation
echo "Running GNU Radio Simulation..."
/usr/bin/python3 gr_lora_sdr/examples/tx_rx_simulation.py

# Create metadata file
cat <<EOF > temp_metadata.json
{
    "sf": $LORA_SF,
    "bw": $LORA_BW,
    "sample_rate": 500000,
    "cr": $LORA_CR,
    "preamble_len": 8,
    "payload_len": $LORA_PAYLOAD_LEN,
    "has_crc": true,
    "ldro": false,
    "implicit_header": false
}
EOF

# Run Host Sim Replay
echo "Running Host Sim Replay..."
# Note: lora_replay is in build/host_sim/lora_replay
# Run the replay tool
./build/host_sim/lora_replay --iq output/test_vector.cf32 --metadata temp_metadata.json --dump-stages output/host_debug --payload "Hello LoRa!"

echo "Done."
