#!/usr/bin/env python3
"""
Debug V3 Symbol extraction vs Original
"""

import numpy as np
import struct

def load_cf32_file(filepath):
    with open(filepath, 'rb') as f:
        raw_data = f.read()
    return np.frombuffer(raw_data, dtype=np.complex64)

def extract_symbol_original_method(samples, position, symbol_idx, sps=512):
    """Extract symbol using exact original method"""
    
    position_offsets = [-20, 0, 6, -4, 8, 4, 2, 2]
    symbol_methods = {
        0: 'phase', 1: 'fft_64', 2: 'fft_128', 3: 'fft_128',
        4: 'fft_128', 5: 'fft_128', 6: 'fft_128', 7: 'phase'
    }
    
    # Calculate position exactly like original
    symbol_pos = position + symbol_idx * sps + position_offsets[symbol_idx]
    symbol_data = samples[symbol_pos:symbol_pos + sps][::4]  # Downsample by 4
    method = symbol_methods[symbol_idx]
    
    # Apply method exactly like original
    if method == 'phase':
        N = 128
        if len(symbol_data) >= N:
            data = symbol_data[:N]
        else:
            data = np.pad(symbol_data, (0, N - len(symbol_data)))
        
        # Remove DC component
        data = data - np.mean(data)
        
        # Phase unwrapping method
        phases = np.unwrap(np.angle(data))
        if len(phases) > 2:
            slope = np.polyfit(np.arange(len(phases)), phases, 1)[0]
            detected = int((slope * N / (2 * np.pi)) % 128)
            return max(0, min(127, detected))
        else:
            return 0
    
    elif method == 'fft_64':
        N = 64
        if len(symbol_data) >= N:
            data = symbol_data[:N]
        else:
            data = np.pad(symbol_data, (0, N - len(symbol_data)))
        fft_result = np.fft.fft(data)
        return np.argmax(np.abs(fft_result))
    
    elif method == 'fft_128':
        N = 128
        if len(symbol_data) >= N:
            data = symbol_data[:N]
        else:
            data = np.pad(symbol_data, (0, N - len(symbol_data)))
        fft_result = np.fft.fft(data)
        return np.argmax(np.abs(fft_result))
    
    return 0

def debug_all_symbols():
    """Debug all 8 symbols with original method"""
    
    print("🔍 Debugging all symbols with original method...")
    
    # Load samples
    samples = load_cf32_file('temp/hello_world.cf32')
    position = 10972
    sps = 512
    
    expected_symbols = [9, 1, 53, 0, 20, 4, 72, 12]
    
    print("Symbol comparison:")
    print("Idx | Original | Expected | Match")
    print("----|----------|----------|------")
    
    for i in range(8):
        detected = extract_symbol_original_method(samples, position, i, sps)
        expected = expected_symbols[i]
        match = "✅" if detected == expected else "❌"
        print(f" {i}  |   {detected:3d}    |   {expected:3d}    | {match}")
    
    # Focus on Symbol 3 - debug step by step  
    print(f"\n🔍 Deep dive into Symbol 3:")
    i = 3
    position_offsets = [-20, 0, 6, -4, 8, 4, 2, 2]
    symbol_pos = position + i * sps + position_offsets[i]
    
    print(f"Position calculation:")
    print(f"  Base: {position}")
    print(f"  + Symbol offset: {i} * {sps} = {i * sps}")
    print(f"  + Position offset: {position_offsets[i]}")
    print(f"  = Final position: {symbol_pos}")
    
    # Extract full data
    full_symbol_data = samples[symbol_pos:symbol_pos + sps]
    downsampled_data = full_symbol_data[::4]
    
    print(f"Data extraction:")
    print(f"  Full data length: {len(full_symbol_data)}")
    print(f"  Downsampled length: {len(downsampled_data)}")
    print(f"  Power (full): {np.mean(np.abs(full_symbol_data)**2):.6f}")
    print(f"  Power (down): {np.mean(np.abs(downsampled_data)**2):.6f}")
    
    # FFT analysis
    N = 128
    if len(downsampled_data) >= N:
        data = downsampled_data[:N]
    else:
        data = np.pad(downsampled_data, (0, N - len(downsampled_data)))
    
    fft_result = np.fft.fft(data)
    magnitude = np.abs(fft_result)
    
    # Find peaks
    peak_idx = np.argmax(magnitude)
    sorted_indices = np.argsort(magnitude)[::-1]
    
    print(f"FFT analysis (N=128):")
    print(f"  Peak at index: {peak_idx}")
    print(f"  Peak magnitude: {magnitude[peak_idx]:.6f}")
    print(f"  Top 5 peaks:")
    for j in range(5):
        idx = sorted_indices[j]
        print(f"    Index {idx:3d}: {magnitude[idx]:.6f}")

if __name__ == "__main__":
    debug_all_symbols()
