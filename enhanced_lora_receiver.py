#!/usr/bin/env python3
"""
Deprecated: enhanced_lora_receiver is no longer maintained.

Please use the sdr_lora-only CLI instead:
  python -m scripts.sdr_lora_cli decode <file.cf32>
or the batch mode:
  python -m scripts.sdr_lora_cli batch --fast
"""

import sys

def main() -> int:  # pragma: no cover
    sys.stderr.write(
        "[DEPRECATED] enhanced_lora_receiver.py has been replaced. Use 'python -m scripts.sdr_lora_cli ...' instead.\n"
    )
    return 2

if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
                result['payload_decode_error'] = str(e)
        
        # Add metadata to result
        result['vector_metadata'] = metadata
        result['config_used'] = config.__dict__
        
        return result
        
    except Exception as e:
        return {"status": "error", "error": f"Decoding failed: {e}"}

def compare_with_gnu_radio(vector_path: Path, our_result: dict) -> dict:
    """Compare our results with GNU Radio output."""
    
    import subprocess
    
    metadata = load_metadata(vector_path)
    if not metadata:
        return {"status": "no_metadata"}
    
    # Run GNU Radio decoder
    try:
        cmd = [
            "python", "external/gr_lora_sdr/scripts/decode_offline_recording.py",
            str(vector_path),
            "--sf", str(metadata["sf"]),
            "--bw", str(metadata["bw"]),
            "--samp-rate", str(metadata["samp_rate"]),
            "--cr", str(metadata["cr"]),
            "--ldro-mode", str(metadata["ldro_mode"])
        ]
        
        if metadata["crc"]:
            cmd.append("--has-crc")
        else:
            cmd.append("--no-crc")
            
        if metadata["impl_header"]:
            cmd.append("--impl-header")
        else:
            cmd.append("--explicit-header")
        
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        
        if result.returncode == 0:
            # Parse GNU Radio output
            lines = result.stdout.strip().split('\\n')
            gnu_radio_result = {
                "status": "success",
                "output": result.stdout,
                "frames": []
            }
            
            for line in lines:
                if "Hex:" in line:
                    hex_data = line.split("Hex:")[1].strip()
                    gnu_radio_result["payload_hex"] = hex_data
                elif "Text:" in line:
                    text_data = line.split("Text:")[1].strip()
                    gnu_radio_result["payload_text"] = text_data
            
            # Compare results
            comparison = {
                "gnu_radio": gnu_radio_result,
                "our_result": our_result,
                "frame_detected_both": (
                    our_result.get("status") == "extracted" and 
                    gnu_radio_result["status"] == "success"
                )
            }
            
            # Check payload match if both succeeded
            if ("payload_hex" in gnu_radio_result and 
                "raw_symbols" in our_result):
                comparison["payload_comparison"] = {
                    "gnu_radio_hex": gnu_radio_result.get("payload_hex", ""),
                    "our_symbols": our_result.get("raw_symbols", []),
                    "match_attempted": True
                }
            
            return comparison
            
        else:
            return {
                "gnu_radio": {"status": "failed", "error": result.stderr},
                "our_result": our_result
            }
            
    except Exception as e:
        return {
            "gnu_radio": {"status": "error", "error": str(e)},
            "our_result": our_result
        }

def main():
    parser = argparse.ArgumentParser(description="Enhanced LoRa Receiver for GNU Radio Compatibility")
    parser.add_argument("vector", type=Path, help="Path to CF32 vector file")
    parser.add_argument("--compare-gnuradio", action="store_true", 
                        help="Compare results with GNU Radio decoder")
    parser.add_argument("--output", type=Path, help="Save detailed results to JSON file")
    
    args = parser.parse_args()
    
    print("🚀 Enhanced LoRa Receiver - GNU Radio Compatibility Mode")
    print("="*60)
    print(f"📁 Processing: {args.vector}")
    
    # Run our enhanced decoder
    result = enhanced_lora_decode(args.vector)
    
    print("\\n📊 OUR RESULTS:")
    print(json.dumps(result, indent=2))
    
    # Compare with GNU Radio if requested
    if args.compare_gnuradio:
        print("\\n🔍 COMPARING WITH GNU RADIO...")
        comparison = compare_with_gnu_radio(args.vector, result)
        
        print("\\n📈 COMPARISON RESULTS:")
        print(json.dumps(comparison, indent=2))
        
        # Save results if requested
        if args.output:
            output_data = {
                "vector": str(args.vector),
                "our_result": result,
                "comparison": comparison,
                "timestamp": __import__('time').time()
            }
            
            with open(args.output, 'w') as f:
                json.dump(output_data, f, indent=2)
            
            print(f"\\n💾 Results saved to: {args.output}")

if __name__ == "__main__":
    main()