#!/usr/bin/env python3
"""
Test V3 at specific position to validate it can extract the same symbols as original
"""

import numpy as np
import struct

def load_cf32_file(filepath):
    with open(filepath, 'rb') as f:
        raw_data = f.read()
    return np.frombuffer(raw_data, dtype=np.complex64)

def extract_symbols_at_position(samples, position):
    """Extract symbols using V3 method at specific position"""
    
    # V3 exact parameters
    sps = 512
    position_offsets = [-20, 0, 6, -4, 8, 4, 2, 2]
    symbol_methods = {
        0: 'phase', 1: 'fft_64', 2: 'fft_128', 3: 'fft_128',
        4: 'fft_128', 5: 'fft_128', 6: 'fft_128', 7: 'phase'
    }
    
    symbols = []
    print(f"🔧 Extracting symbols from position {position} with V3 method...")
    
    for i in range(8):
        symbol_pos = position + i * sps + position_offsets[i]
        
        if symbol_pos + sps > len(samples):
            print(f"❌ Not enough samples for symbol {i}")
            symbols.append(0)
            continue
        
        # Extract with downsampling by 4
        symbol_data = samples[symbol_pos:symbol_pos + sps][::4]
        method = symbol_methods[i]
        
        # Apply method
        if method == 'phase':
            N = 128
            if len(symbol_data) >= N:
                data = symbol_data[:N]
            else:
                data = np.pad(symbol_data, (0, N - len(symbol_data)))
            
            data = data - np.mean(data)
            phases = np.unwrap(np.angle(data))
            if len(phases) > 2:
                slope = np.polyfit(np.arange(len(phases)), phases, 1)[0]
                detected = int((slope * N / (2 * np.pi)) % 128)
                detected = max(0, min(127, detected))
            else:
                detected = 0
                
        elif method == 'fft_64':
            N = 64
            if len(symbol_data) >= N:
                data = symbol_data[:N]
            else:
                data = np.pad(symbol_data, (0, N - len(symbol_data)))
            fft_result = np.fft.fft(data)
            detected = np.argmax(np.abs(fft_result))
            detected = max(0, min(127, detected))
            
        elif method == 'fft_128':
            N = 128
            if len(symbol_data) >= N:
                data = symbol_data[:N]
            else:
                data = np.pad(symbol_data, (0, N - len(symbol_data)))
            fft_result = np.fft.fft(data)
            detected = np.argmax(np.abs(fft_result))
            detected = max(0, min(127, detected))
        
        symbols.append(detected)
        print(f"   Symbol {i}: {detected:3d} (method: {method}, offset: {position_offsets[i]})")
    
    return symbols

def main():
    print("🔍 TESTING V3 AT ORIGINAL'S FOUND POSITION")
    print("=" * 50)
    
    # Load samples
    samples = load_cf32_file('vectors/sps_500k_bw_125k_sf_7_cr_2_ldro_false_crc_true_implheader_false_hello_stupid_world.unknown')
    print(f"📊 Loaded {len(samples)} samples")
    
    # Test at the position found by original receiver
    original_position = 43500
    original_symbols = [122, 25, 74, 109, 77, 40, 71, 18]
    
    print(f"\n🎯 Testing at original's position: {original_position}")
    print(f"   Expected symbols (from original): {original_symbols}")
    
    v3_symbols = extract_symbols_at_position(samples, original_position)
    
    print(f"\n📊 COMPARISON:")
    print(f"   Original: {original_symbols}")
    print(f"   V3 at same position: {v3_symbols}")
    
    # Check if they match
    if v3_symbols == original_symbols:
        print("✅ PERFECT MATCH! V3 extracts same symbols as original")
    else:
        print("❌ Different symbols - let's analyze differences:")
        for i, (orig, v3_val) in enumerate(zip(original_symbols, v3_symbols)):
            match = "✅" if orig == v3_val else "❌"
            print(f"     Symbol {i}: {orig:3d} vs {v3_val:3d} {match}")
    
    # Also test V3's found position for reference
    print(f"\n🔍 For reference, V3's original detection:")
    v3_position = 7804
    v3_original_symbols = [123, 49, 74, 52, 74, 0, 39, 11]
    v3_at_v3_pos = extract_symbols_at_position(samples, v3_position)
    
    print(f"   V3's found position: {v3_position}")
    print(f"   V3's extracted symbols: {v3_original_symbols}")
    print(f"   Re-extraction at same pos: {v3_at_v3_pos}")
    
    if v3_original_symbols == v3_at_v3_pos:
        print("✅ V3 is consistent with itself")
    else:
        print("❌ V3 inconsistency detected")

if __name__ == "__main__":
    main()
