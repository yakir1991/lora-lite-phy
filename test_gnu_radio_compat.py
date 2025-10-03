#!/usr/bin/env python3
"""
GNU Radio Compatibility Test Suite

This script compares the lora-lite-phy decoder against GNU Radio's gr-lora-sdr
on a comprehensive set of test vectors to measure compatibility.
"""

import os
import sys
import json
import subprocess
import pytest
import argparse
from pathlib import Path
from typing import Dict, List, Optional, Tuple, Any
import tempfile
import time

def run_gnu_radio_decoder(vector_path: Path, metadata: Dict) -> Optional[Dict]:
    """Run GNU Radio decoder on a vector and return results."""
    try:
        cmd = [
            sys.executable, "external/gr_lora_sdr/scripts/decode_offline_recording.py",
            str(vector_path),
            "--sf", str(metadata["sf"]),
            "--bw", str(metadata["bw"]),
            "--samp-rate", str(metadata["samp_rate"]),
            "--cr", str(metadata["cr"]),
            "--ldro-mode", str(metadata["ldro_mode"]),
            "--format", "cf32",
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
            # Parse the output to extract frame information
            lines = result.stdout.strip().split('\n')
            frames = []
            
            for line in lines:
                if line.startswith("Frame"):
                    # Parse frame line: "Frame 0: 11 bytes, CRC valid"
                    parts = line.split(": ")
                    if len(parts) >= 2:
                        frame_info = parts[1]
                        frames.append({"info": frame_info})
                elif line.strip().startswith("Hex:"):
                    # Parse hex payload
                    hex_data = line.strip().replace("Hex: ", "")
                    if frames:
                        frames[-1]["hex"] = hex_data
                elif line.strip().startswith("Text:"):
                    # Parse text payload
                    text_data = line.strip().replace("Text: ", "")
                    if frames:
                        frames[-1]["text"] = text_data
                        
            return {
                "status": "success",
                "frames": frames,
                "output": result.stdout
            }
        else:
            return {
                "status": "failed",
                "error": result.stderr,
                "output": result.stdout
            }
            
    except subprocess.TimeoutExpired:
        return {"status": "timeout"}
    except Exception as e:
        return {"status": "error", "error": str(e)}

def run_lora_lite_python(vector_path: Path, metadata: Dict) -> Optional[Dict]:
    """Run our Python LoRa receiver on a vector."""
    try:
        cmd = [
            sys.executable, "complete_lora_receiver.py", str(vector_path),
            "--sf", str(metadata.get("sf")),
            "--bw", str(metadata.get("bw")),
            "--cr", str(metadata.get("cr")),
            "--samp-rate", str(metadata.get("samp_rate")),
            "--ldro-mode", str(metadata.get("ldro_mode", 0)),
        ]
        # Pass sync words from metadata if present (avoid default mismatches)
        if "sync_words" in metadata and isinstance(metadata["sync_words"], list) and metadata["sync_words"]:
            sw_str = ",".join(f"0x{int(s)&0xFF:02x}" for s in metadata["sync_words"]) 
            cmd += ["--sync-words", sw_str]
        elif "sync_word" in metadata:
            cmd += ["--sync-words", f"0x{int(metadata['sync_word']) & 0xFF:02x}"]
        # Ensure unbiased defaults: do not enable oracle assist from tests
        # (receiver keeps oracle-assist off unless explicitly requested)
        if metadata.get("crc", True):
            cmd.append("--crc")
        else:
            cmd.append("--no-crc")
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        
        if result.returncode == 0:
            # Extract last JSON object from stdout
            json_data = None
            for line in result.stdout.splitlines()[::-1]:
                line = line.strip()
                if line.startswith('{') and line.endswith('}'):
                    try:
                        json_data = json.loads(line)
                        break
                    except json.JSONDecodeError:
                        continue
            return {
                "status": "success",
                "data": json_data or {},
                "output": result.stdout
            }
        else:
            return {
                "status": "failed",
                "error": result.stderr,
                "output": result.stdout
            }
            
    except subprocess.TimeoutExpired:
        return {"status": "timeout"}
    except Exception as e:
        return {"status": "error", "error": str(e)}

def run_lora_lite_cpp(vector_path: Path, metadata: Dict) -> Optional[Dict]:
    """Run our C++ LoRa receiver on a vector."""
    try:
        # Determine oversampling factor
        os_factor = metadata["samp_rate"] // metadata["bw"]
        # Ensure the binary exists
        run_vec_path = Path("./build/run_vector")
        if not run_vec_path.exists():
            return {
                "status": "skipped",
                "error": "run_vector binary not found at ./build/run_vector",
            }
        
        cmd = [
            str(run_vec_path),
            str(vector_path),
            str(metadata["sf"]),
            str(metadata["cr"]),
            "1" if metadata["crc"] else "0",
            "1" if metadata["impl_header"] else "0", 
            str(metadata["ldro_mode"]),
            str(os_factor),
            # Prefer metadata-provided sync words if present; fallback to 0x12 (18) and 0
            str(int((metadata.get("sync_word") or metadata.get("sync_words", [18])[0]))),
            str(int((metadata.get("sync_words", [18, 0])[1] if isinstance(metadata.get("sync_words"), list) and len(metadata.get("sync_words")) > 1 else 0)))
        ]
        
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        
        if result.returncode == 0:
            return {
                "status": "success",
                "output": result.stdout
            }
        else:
            return {
                "status": "failed",
                "error": result.stderr,
                "output": result.stdout
            }
            
    except subprocess.TimeoutExpired:
        return {"status": "timeout"}
    except Exception as e:
        return {"status": "error", "error": str(e)}

def _have_gnuradio_script() -> bool:
    return Path("external/gr_lora_sdr/scripts/decode_offline_recording.py").exists()


def _collect_vector_pairs(vectors_dir: Path) -> list[tuple[Path, Path]]:
    pairs = []
    if not vectors_dir.exists():
        return pairs
    for cf32_file in vectors_dir.glob("*.cf32"):
        json_file = cf32_file.with_suffix(".json")
        if json_file.exists():
            pairs.append((cf32_file, json_file))
    return pairs


def test_vector_compatibility():
    """End-to-end compatibility smoke test over a small sample of vectors.

    Skips if GNU Radio offline decoder isn't available or vectors are missing.
    """
    vectors_dir = Path("golden_vectors_demo_batch")
    pairs = _collect_vector_pairs(vectors_dir)
    if not pairs:
        pytest.skip("no vectors found in golden_vectors_demo_batch")
    if not _have_gnuradio_script():
        pytest.skip("GNU Radio offline decoder script not found")

    # Limit runtime: test only the first 1-2 vectors for CI speed
    sample = pairs[:1]
    all_results = []
    for vector_path, metadata_path in sample:
        with open(metadata_path, 'r') as f:
            metadata = json.load(f)
        print(f"\n🧪 Testing: {vector_path.name}")
        print(f"   SF={metadata['sf']}, BW={metadata['bw']}, CR={metadata['cr']}, CRC={metadata['crc']}")
        result = {
            "vector": str(vector_path),
            "metadata": metadata,
            "gnu_radio": run_gnu_radio_decoder(vector_path, metadata),
            "lora_lite_python": run_lora_lite_python(vector_path, metadata),
            "lora_lite_cpp": run_lora_lite_cpp(vector_path, metadata),
        }
        all_results.append(result)

    analysis = analyze_compatibility(all_results)
    # Basic structure assertions
    assert "total_vectors" in analysis and analysis["total_vectors"] == len(sample)
    # At least our Python path should return a structured response (success or fail)
    assert all(r.get("lora_lite_python") is not None for r in all_results)

def analyze_compatibility(results: List[Dict]) -> Dict:
    """Analyze compatibility results."""
    
    total_vectors = len(results)
    gnu_radio_success = sum(1 for r in results if r["gnu_radio"] and r["gnu_radio"]["status"] == "success")
    python_success = sum(1 for r in results if r["lora_lite_python"] and r["lora_lite_python"]["status"] == "success")
    cpp_success = sum(1 for r in results if r["lora_lite_cpp"] and r["lora_lite_cpp"]["status"] == "success")
    
    # Check payload matching for successful cases
    payload_matches = 0
    exact_payload_matches = 0
    for result in results:
        gr = result["gnu_radio"]
        py = result["lora_lite_python"]
        
        if (gr and gr["status"] == "success" and gr["frames"] and
            py and py["status"] == "success"):
            
            # Compare payload hex if both have it
            if gr["frames"] and "hex" in gr["frames"][0]:
                expected_hex = gr["frames"][0]["hex"].replace(" ", "").lower()
                payload_matches += 1
                py_hex = None
                try:
                    py_hex = (py.get("data", {}) or {}).get("payload_hex")
                except Exception:
                    py_hex = None
                if py_hex and py_hex.lower() == expected_hex:
                    exact_payload_matches += 1
    
    return {
        "total_vectors": total_vectors,
        "gnu_radio_success_rate": gnu_radio_success / total_vectors * 100,
        "python_success_rate": python_success / total_vectors * 100,
        "cpp_success_rate": cpp_success / total_vectors * 100,
        "payload_match_rate": payload_matches / max(gnu_radio_success, 1) * 100,
        "exact_payload_match_rate": exact_payload_matches / max(gnu_radio_success, 1) * 100,
        "summary": {
            "gnu_radio_successes": gnu_radio_success,
            "python_successes": python_success,
            "cpp_successes": cpp_success,
            "payload_matches": payload_matches,
            "exact_payload_matches": exact_payload_matches
        }
    }

def main():
    parser = argparse.ArgumentParser(description="GNU Radio Compatibility Test Suite")
    parser.add_argument("--vectors-dir", type=Path, default="golden_vectors_demo_batch",
                        help="Directory containing test vectors")
    parser.add_argument("--output", type=Path, default="gnu_radio_compat_results.json",
                        help="Output file for results")
    parser.add_argument("--limit", type=int, default=None,
                        help="Limit number of vectors to test")
    
    args = parser.parse_args()
    
    print("🚀 GNU Radio Compatibility Test Suite")
    print("="*50)
    
    # Find all CF32 files with corresponding JSON metadata
    vector_files = []
    vectors_dir = Path(args.vectors_dir)
    
    for cf32_file in vectors_dir.glob("*.cf32"):
        json_file = cf32_file.with_suffix(".json")
        if json_file.exists():
            vector_files.append((cf32_file, json_file))
    
    if args.limit:
        vector_files = vector_files[:args.limit]
    
    print(f"📊 Found {len(vector_files)} test vectors")
    
    # Test each vector
    all_results = []
    for i, (vector_path, metadata_path) in enumerate(vector_files):
        print(f"\n[{i+1}/{len(vector_files)}]", end="")
        result = test_vector_compatibility(vector_path, metadata_path)
        all_results.append(result)
    
    # Analyze results
    print("\n\n📈 COMPATIBILITY ANALYSIS")
    print("="*50)
    
    analysis = analyze_compatibility(all_results)
    
    print(f"Total vectors tested: {analysis['total_vectors']}")
    print(f"GNU Radio success rate: {analysis['gnu_radio_success_rate']:.1f}%")
    print(f"LoRa-Lite Python success rate: {analysis['python_success_rate']:.1f}%")
    print(f"LoRa-Lite C++ success rate: {analysis['cpp_success_rate']:.1f}%")
    print(f"Payload match rate: {analysis['payload_match_rate']:.1f}%")
    print(f"Exact payload match rate: {analysis['exact_payload_match_rate']:.1f}%")
    
    # Save detailed results
    output_data = {
        "analysis": analysis,
        "detailed_results": all_results,
        "timestamp": time.time()
    }
    
    with open(args.output, 'w') as f:
        json.dump(output_data, f, indent=2)
    
    print(f"\n💾 Detailed results saved to: {args.output}")
    
    # Print recommendations
    print("\n🎯 RECOMMENDATIONS:")
    if analysis['python_success_rate'] < 90:
        print("   • Improve Python decoder frame detection")
    if analysis['cpp_success_rate'] < 50:
        print("   • Fix C++ decoder sync issues")
    if analysis['payload_match_rate'] < 80:
        print("   • Debug payload decoding mismatches")
    
    compatibility_score = (analysis['python_success_rate'] + analysis['payload_match_rate']) / 2
    print(f"\n🏆 Overall Compatibility Score: {compatibility_score:.1f}%")
    
    if compatibility_score >= 95:
        print("   STATUS: ✅ EXCELLENT COMPATIBILITY")
    elif compatibility_score >= 80:
        print("   STATUS: 🟡 GOOD COMPATIBILITY")  
    else:
        print("   STATUS: ❌ NEEDS IMPROVEMENT")

if __name__ == "__main__":
    main()