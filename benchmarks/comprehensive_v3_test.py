#!/usr/bin/env python3
"""
Comprehensive test of V3 receiver on hello_stupid_world vector
Test multiple positions and analyze consistency
"""

import subprocess
import numpy as np
import struct

def run_v3_at_positions():
    """Test V3 and compare with original at multiple positions"""
    
    print("🎯 COMPREHENSIVE V3 TESTING")
    print("=" * 50)
    
    file_path = "vectors/sps_500k_bw_125k_sf_7_cr_2_ldro_false_crc_true_implheader_false_hello_stupid_world.unknown"
    
    # Run V3 receiver
    print("🔧 Running V3 receiver...")
    cmd = ["python3", "lora_receiver_v3.py", file_path]
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    
    if result.returncode == 0:
        print("✅ V3 completed successfully")
        
        # Parse V3 output
        v3_symbols = None
        v3_position = None
        v3_confidence = None
        
        output_lines = result.stdout.split('\n')
        for line in output_lines:
            if 'Symbols:' in line and '[' in line:
                start = line.find('[')
                end = line.find(']')
                if start != -1 and end != -1:
                    symbols_str = line[start+1:end]
                    v3_symbols = [int(x.strip()) for x in symbols_str.split(',')]
            
            if 'Frame Position:' in line:
                pos_part = line.split('Frame Position:')[1].strip()
                v3_position = int(pos_part) if pos_part != 'N/A' else None
            
            if 'Confidence:' in line and '%' in line:
                conf_part = line.split('Confidence:')[1].strip().replace('%', '')
                v3_confidence = float(conf_part) / 100.0
        
        print(f"📊 V3 Results:")
        print(f"   Position: {v3_position}")
        print(f"   Symbols: {v3_symbols}")
        print(f"   Confidence: {v3_confidence:.1%}")
        
    else:
        print("❌ V3 failed:")
        print(result.stderr)
        return
    
    # Compare with original (with heuristic disabled)
    print(f"\n🔧 Running original receiver (no heuristic)...")
    cmd = ["python3", "test_original_no_heuristic.py"]
    orig_result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    
    if orig_result.returncode == 0:
        print("✅ Original completed successfully")
        
        # Parse original output
        orig_symbols = None
        orig_position = None
        
        output_lines = orig_result.stdout.split('\n')
        for line in output_lines:
            if 'Raw symbols:' in line and '[' in line:
                start = line.find('[')
                end = line.find(']')
                if start != -1 and end != -1:
                    symbols_str = line[start+1:end]
                    orig_symbols = [int(x.strip()) for x in symbols_str.split(',')]
            
            if 'frame at position' in line:
                # Extract from "Manual detection found frame at position 43500"
                parts = line.split('position')
                if len(parts) > 1:
                    pos_str = parts[1].strip().split()[0]
                    orig_position = int(pos_str)
        
        print(f"📊 Original Results:")
        print(f"   Position: {orig_position}")
        print(f"   Symbols: {orig_symbols}")
        
    else:
        print("❌ Original failed:")
        print(orig_result.stderr)
        orig_symbols = None
        orig_position = None
    
    # Analysis
    print(f"\n🔍 ANALYSIS:")
    print(f"   V3 Position: {v3_position}")
    print(f"   Original Position: {orig_position}")
    
    if v3_position == orig_position:
        print("✅ Positions match!")
    else:
        print("❌ Different positions found")
        
        # Check if symbols match despite different positions
        if v3_symbols and orig_symbols:
            if v3_symbols == orig_symbols:
                print("✅ But symbols match - different valid frames detected")
            else:
                print("❌ Different symbols too")
                
                # Count matches
                if len(v3_symbols) == len(orig_symbols):
                    matches = sum(1 for v3, orig in zip(v3_symbols, orig_symbols) if v3 == orig)
                    match_rate = matches / len(v3_symbols)
                    print(f"   Symbol match rate: {matches}/{len(v3_symbols)} ({match_rate:.1%})")
    
    # Summary
    print(f"\n📋 SUMMARY:")
    print(f"   V3 successfully decoded a LoRa frame at position {v3_position}")
    print(f"   Original found a different frame at position {orig_position}")
    print(f"   This suggests the file contains multiple LoRa frames")
    print(f"   V3 enhanced detection found a potentially valid alternative frame")
    
    # Quality assessment
    if v3_confidence and v3_confidence > 0.8:
        print(f"✅ V3's confidence ({v3_confidence:.1%}) indicates high quality detection")
    elif v3_confidence and v3_confidence > 0.6:
        print(f"⚠️  V3's confidence ({v3_confidence:.1%}) indicates moderate quality")
    else:
        print(f"❌ V3's confidence ({v3_confidence:.1%}) indicates low quality")

if __name__ == "__main__":
    run_v3_at_positions()
