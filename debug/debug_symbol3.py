#!/usr/bin/env python3
"""
Debug Symbol 3 extraction to match original exactly
"""

import numpy as np
import struct

def load_cf32_file(filepath):
    with open(filepath, 'rb') as f:
        raw_data = f.read()
    return np.frombuffer(raw_data, dtype=np.complex64)

def debug_symbol_3():
    """Debug specific symbol 3 extraction"""
    
    print("🔍 Debugging Symbol 3 extraction...")
    
    # Load samples
    samples = load_cf32_file('temp/hello_world.cf32')
    print(f"Loaded {len(samples)} samples")
    
    # Parameters from working receiver
    position = 10972
    sps = 512  # samples per symbol
    sf = 7
    N = 2**sf  # 128
    
    # Position offsets from original
    position_offsets = [-20, 0, 6, -4, 8, 4, 2, 2]
    
    # Extract Symbol 3 (should be 0)
    i = 3
    symbol_pos = position + i * sps + position_offsets[i]
    symbol_data = samples[symbol_pos:symbol_pos + sps][::4]  # Downsample by 4
    
    print(f"Symbol {i}:")
    print(f"  Base position: {position}")
    print(f"  Symbol offset: {i} * {sps} = {i * sps}")
    print(f"  Position offset: {position_offsets[i]}")
    print(f"  Final position: {symbol_pos}")
    print(f"  Symbol data length: {len(symbol_data)}")
    print(f"  Expected method: fft_128")
    
    # Try FFT method (what V3 uses)
    fft_data = symbol_data[:128] if len(symbol_data) >= 128 else np.pad(symbol_data, (0, 128 - len(symbol_data)))
    fft_result = np.fft.fft(fft_data)
    fft_detected = np.argmax(np.abs(fft_result))
    print(f"  FFT_128 result: {fft_detected}")
    
    # Let's also try without downsampling 
    symbol_data_full = samples[symbol_pos:symbol_pos + sps]  # No downsampling
    fft_data_full = symbol_data_full[:128] if len(symbol_data_full) >= 128 else np.pad(symbol_data_full, (0, 128 - len(symbol_data_full)))
    fft_result_full = np.fft.fft(fft_data_full)
    fft_detected_full = np.argmax(np.abs(fft_result_full))
    print(f"  FFT_128 (no downsample): {fft_detected_full}")
    
    # Try different position offsets around the original
    print("\n🔍 Testing different offsets for Symbol 3:")
    for test_offset in [-8, -6, -4, -2, 0, 2, 4]:
        test_pos = position + i * sps + test_offset
        if test_pos + sps < len(samples):
            test_data = samples[test_pos:test_pos + sps][::4]
            test_fft_data = test_data[:128] if len(test_data) >= 128 else np.pad(test_data, (0, 128 - len(test_data)))
            test_fft_result = np.fft.fft(test_fft_data)
            test_detected = np.argmax(np.abs(test_fft_result))
            marker = "✅" if test_detected == 0 else "❌"
            print(f"    Offset {test_offset:2d}: {test_detected:3d} {marker}")
    
    # Let's also check what the original C++ implementation might be doing differently
    # Try with different N values
    print("\n🔍 Testing different FFT sizes:")
    for N_test in [64, 96, 128, 256]:
        test_data = symbol_data[:N_test] if len(symbol_data) >= N_test else np.pad(symbol_data, (0, N_test - len(symbol_data)))
        test_fft_result = np.fft.fft(test_data)
        test_detected = np.argmax(np.abs(test_fft_result))
        marker = "✅" if test_detected == 0 else "❌"
        print(f"    FFT N={N_test:3d}: {test_detected:3d} {marker}")


if __name__ == "__main__":
    debug_symbol_3()
