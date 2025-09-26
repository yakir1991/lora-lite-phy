#!/usr/bin/env python3
"""
Calculate the exact position where frame sync found the frame
"""

import numpy as np
import struct

def load_cf32_file(filename):
    """Load CF32 format file"""
    with open(filename, 'rb') as f:
        data = f.read()
    
    samples = struct.unpack('<{}f'.format(len(data)//4), data)
    complex_samples = np.array([samples[i] + 1j*samples[i+1] for i in range(0, len(samples), 2)])
    return complex_samples

def calculate_frame_sync_position():
    """Calculate where frame sync found the frame"""
    # From our C++ debug output:
    # Frame sync found frame at iteration 8
    # CFO values: cfo_int=9, cfo_frac=0.00164447
    # Sync words: 93, 112
    
    # Frame sync typically advances by some number of samples per iteration
    # Let's assume it advances by ~100-200 samples per iteration (common in GNU Radio)
    
    samples = load_cf32_file('temp/hello_world.cf32')
    print(f"Total samples: {len(samples)}")
    
    # Try different step sizes for frame sync iteration
    step_sizes = [64, 128, 256, 512]
    iteration = 8
    
    print(f"Frame sync found frame at iteration {iteration}")
    print("Possible frame start positions:")
    
    for step_size in step_sizes:
        position = iteration * step_size
        print(f"  Step size {step_size}: position {position}")
    
    # Also consider that the frame might start some samples after detection
    # Frame sync detects the preamble, but data symbols start after preamble + sync word
    preamble_length = 8 * 512  # 8 preamble symbols * 512 samples/symbol
    sync_word_length = 2.25 * 512  # 2.25 sync word symbols
    
    print(f"\nConsidering preamble offset:")
    print(f"Preamble length: {preamble_length} samples")
    print(f"Sync word length: {sync_word_length} samples")
    
    for step_size in step_sizes:
        detection_pos = iteration * step_size
        data_start = detection_pos + int(preamble_length + sync_word_length)
        print(f"  Step {step_size}: detection at {detection_pos}, data at {data_start}")

def test_calculated_positions():
    """Test symbol extraction at calculated positions"""
    samples = load_cf32_file('temp/hello_world.cf32')
    gr_fft_symbols = [9, 1, 1, 0, 27, 4, 26, 12]
    
    # Test positions based on frame sync iteration
    test_positions = [
        8 * 128,    # 1024
        8 * 256,    # 2048
        8 * 512,    # 4096
        1024 + 4608,  # Detection + preamble/sync
        2048 + 4608,
        4096 + 4608,
    ]
    
    print("\n=== Testing calculated positions ===")
    
    for pos in test_positions:
        if pos + 8 * 512 > len(samples):
            print(f"Position {pos}: Beyond file length")
            continue
            
        print(f"\nTesting position {pos}:")
        symbols = simple_demod(samples, pos, 8)
        matches = sum(1 for i in range(8) if symbols[i] == gr_fft_symbols[i])
        print(f"  Symbols: {symbols}")
        print(f"  Matches: {matches}/8")
        
        if matches > 2:
            print("  *** PROMISING! ***")

def simple_demod(samples, offset, num_symbols):
    """Simple demodulation for testing"""
    symbols = []
    N = 128
    samples_per_symbol = 512
    
    for i in range(num_symbols):
        start = offset + i * samples_per_symbol
        if start + samples_per_symbol > len(samples):
            break
        
        # Extract symbol
        symbol_samples = samples[start:start + samples_per_symbol]
        
        # Decimate to N samples
        decimated = symbol_samples[::4][:N]
        
        # Simple FFT peak detection
        fft_result = np.fft.fft(decimated)
        peak_idx = np.argmax(np.abs(fft_result))
        symbols.append(peak_idx)
    
    return symbols

if __name__ == "__main__":
    calculate_frame_sync_position()
    test_calculated_positions()
