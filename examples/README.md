# Examples

Minimal programs demonstrating the lora-lite-phy library API.

## Build

```bash
cmake -B build -G Ninja
cmake --build build
```

## Programs

### `example_tx` — Encode a LoRa packet

Generates a CF32 IQ file containing a simple LoRa packet.

```bash
./build/examples/example_tx
# Output: examples_output.cf32
```

### `example_rx` — Decode a CF32 capture

Decodes a CF32 IQ file and prints the payload.

```bash
./build/examples/example_rx capture.cf32 metadata.json
```

## Pipeline Example

Encode, then decode:

```bash
# Generate a packet with the full TX encoder
./build/host_sim/lora_tx --sf 7 --cr 1 --bw 125000 \
    --payload "Hello" --output /tmp/test.cf32

# Decode it
./build/host_sim/lora_replay --iq /tmp/test.cf32 \
    --payload "Hello"
```

## Real-time HackRF decode

```bash
hackrf_transfer -r /dev/stdout -f 868100000 -s 2000000 -n 4000000 \
    | ./build/host_sim/lora_replay --iq - --format hackrf \
      --metadata metadata.json --multi
```
