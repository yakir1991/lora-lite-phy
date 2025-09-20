#!/usr/bin/env python3
"""
Decoder for the new C++ LoRa pipeline.
This script uses the new C++ pipeline to decode LoRa baseband captures.
"""

import argparse
import json
import sys
from pathlib import Path

import numpy as np

# Add the project root to the Python path
project_root = Path(__file__).parent.parent
sys.path.insert(0, str(project_root))

try:
    import lora_pipeline
except ImportError as e:
    print(f"Error importing lora_pipeline: {e}")
    print("Make sure you have built the project with 'ninja -C build'")
    sys.exit(1)


def load_iq_samples(file_path):
    """Load IQ samples from file."""
    file_path = Path(file_path)
    
    if not file_path.exists():
        raise FileNotFoundError(f"File not found: {file_path}")
    
    # Try to determine file format
    if file_path.suffix.lower() == '.npy':
        # NumPy array file
        samples = np.load(file_path)
    elif file_path.suffix.lower() == '.unknown':
        # Raw binary file (assume complex64)
        samples = np.fromfile(file_path, dtype=np.complex64)
    else:
        # Try to load as raw binary complex64
        samples = np.fromfile(file_path, dtype=np.complex64)
    
    print(f"Loaded {len(samples)} samples from {file_path}")
    return samples


def main():
    parser = argparse.ArgumentParser(description="Decode LoRa baseband capture using new C++ pipeline")
    parser.add_argument("input_file", help="Input IQ samples file")
    parser.add_argument("--sf", type=int, default=7, help="Spreading factor (default: 7)")
    parser.add_argument("--bw", type=float, default=125000, help="Bandwidth (default: 125000)")
    parser.add_argument("--samp-rate", type=float, default=500000, help="Sample rate (default: 500000)")
    parser.add_argument("--cr", type=int, default=1, help="Coding rate (default: 1)")
    parser.add_argument("--sync-word", type=int, default=0x34, help="Sync word (default: 0x34)")
    parser.add_argument("--output", "-o", help="Output JSON file")
    parser.add_argument("--verbose", "-v", action="store_true", help="Verbose output")
    
    args = parser.parse_args()
    
    # Load IQ samples
    try:
        iq_samples = load_iq_samples(args.input_file)
    except Exception as e:
        print(f"Error loading samples: {e}")
        return 1
    
    # Configure the pipeline
    config = lora_pipeline.Config(
        sf=args.sf,
        bw=args.bw,
        samp_rate=args.samp_rate,
        cr=args.cr,
        sync_word=args.sync_word,
        decode_payload=True,
        expect_payload_crc=True
    )
    
    print(f"Pipeline configuration:")
    print(f"  Spreading factor: {config.sf}")
    print(f"  Bandwidth: {config.bw}")
    print(f"  Sample rate: {config.samp_rate}")
    print(f"  Coding rate: {config.cr}")
    print(f"  Sync word: 0x{config.sync_word:02x}")
    
    # Create pipeline
    pipeline = lora_pipeline.GnuRadioLikePipeline(config)
    
    # Run pipeline
    print(f"\nRunning pipeline on {len(iq_samples)} samples...")
    result = pipeline.run(iq_samples)
    
    if not result.success:
        print(f"Pipeline failed: {result.failure_reason}")
        return 1
    
    print(f"\nPipeline completed successfully!")
    print(f"  Frame count: {result.frame_count}")
    print(f"  Individual frame payloads: {len(result.individual_frame_payloads)}")
    print(f"  Individual frame CRC status: {len(result.individual_frame_crc_ok)}")
    
    # Print frame results
    for i, (payload, crc_ok) in enumerate(zip(result.individual_frame_payloads, result.individual_frame_crc_ok)):
        print(f"\nFrame {i}:")
        print(f"  Length: {len(payload)} bytes")
        print(f"  CRC valid: {crc_ok}")
        if len(payload) > 0:
            try:
                text = payload.decode('latin-1', errors='replace')
                print(f"  Text: '{text}'")
                print(f"  Hex: {' '.join(f'{b:02x}' for b in payload)}")
            except Exception as e:
                print(f"  Hex: {' '.join(f'{b:02x}' for b in payload)}")
                print(f"  Error decoding text: {e}")
    
    # Save results to JSON if requested
    if args.output:
        output_data = {
            "config": {
                "sf": config.sf,
                "bw": config.bw,
                "samp_rate": config.samp_rate,
                "cr": config.cr,
                "sync_word": config.sync_word,
            },
            "result": {
                "success": result.success,
                "failure_reason": result.failure_reason,
                "frame_count": result.frame_count,
                "frames": [
                    {
                        "index": i,
                        "payload": payload.hex(),
                        "payload_text": payload.decode('latin-1', errors='replace'),
                        "crc_valid": crc_ok,
                        "length": len(payload)
                    }
                    for i, (payload, crc_ok) in enumerate(zip(result.individual_frame_payloads, result.individual_frame_crc_ok))
                ]
            }
        }
        
        output_path = Path(args.output)
        with output_path.open("w", encoding="utf-8") as f:
            json.dump(output_data, f, indent=2, ensure_ascii=False)
        
        print(f"\nResults saved to: {output_path}")
    
    return 0


if __name__ == "__main__":
    sys.exit(main())
