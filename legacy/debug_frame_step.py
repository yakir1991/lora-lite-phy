#!/usr/bin/env python3
"""
Final attempt: Use exact CFO values from C++ frame sync
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

def precise_lora_demod(samples, start_offset, cfo_int=9, cfo_frac=0.00164447, sf=7, bw=125000, fs=500000):
    """
    Use the exact CFO parameters from the C++ frame sync
    """
    N = 2**sf  # 128
    samples_per_symbol = int(fs * N / bw)  # 512
    
    print(f"Precise LoRa demod:")
    print(f"  CFO: int={cfo_int}, frac={cfo_frac:.8f}")
    print(f"  Starting at sample {start_offset}")
    print(f"  SF={sf}, N={N}, samples_per_symbol={samples_per_symbol}")
    
    # Apply CFO correction
    total_cfo_bins = cfo_int + cfo_frac
    cfo_hz = total_cfo_bins * bw / N  # Convert to Hz
    print(f"  Total CFO: {total_cfo_bins:.6f} bins = {cfo_hz:.1f} Hz")
    
    symbols = []
    
    for i in range(20):  # Extract 20 symbols
        sym_start = start_offset + i * samples_per_symbol
        sym_end = sym_start + samples_per_symbol
        
        if sym_end > len(samples):
            print(f"Symbol {i}: Not enough samples")
            break
            
        # Extract symbol samples
        symbol_samples = samples[sym_start:sym_end]
        
        # Apply CFO correction in time domain
        if cfo_hz != 0:
            t = np.arange(len(symbol_samples)) / fs
            cfo_correction = np.exp(-1j * 2 * np.pi * cfo_hz * t)
            symbol_samples = symbol_samples * cfo_correction
        
        # GNU Radio style: decimate to N samples (from 512 to 128)
        decimated = symbol_samples[::4][:N]  # Take every 4th sample
        
        # FFT
        fft_result = np.fft.fft(decimated, N)
        
        # Apply integer CFO correction in frequency domain
        if cfo_int != 0:
            fft_result = np.roll(fft_result, -cfo_int)
        
        # Find peak
        peak_idx = np.argmax(np.abs(fft_result))
        peak_mag = np.abs(fft_result[peak_idx])
        
        symbols.append(peak_idx)
        print(f"  Symbol {i:2d}: {peak_idx:3d} (mag: {peak_mag:.1f})")
    
    return symbols

def test_precise_positions():
    """Test precise positions based on C++ frame sync output"""
    samples = load_cf32_file('temp/hello_world.cf32')
    target = [9, 1, 1, 0, 27, 4, 26, 12, 53, 124, 32, 70, 107, 50, 74, 62, 120, 83, 82, 23]
    
    print(f"Total samples: {len(samples)}")
    print(f"Target symbols: {target[:8]}")
    print()
    
    # The C++ frame sync consumed 8136 samples total at iteration 8
    # Each iteration consumes roughly 512 samples (except iteration 3: 292, iteration 8: 676)
    # Let's calculate where the data symbols should start
    
    # Iterations: 0-7 consumed samples, iteration 8 detected frame
    consumed_before_detection = 512*7 + 292  # 3876 samples
    consumed_at_detection = 676
    
    print(f"Samples consumed before detection: {consumed_before_detection}")
    print(f"Samples consumed at detection: {consumed_at_detection}")
    print(f"Total at detection: {consumed_before_detection + consumed_at_detection}")
    
    # Frame sync outputs symbols starting from the next iteration (9)
    # So data symbols should start around sample 3876 + 676 = 4552
    
    candidate_positions = [
        4552,                    # Right after frame detection
        4552 + 512,             # One symbol later
        4552 - 512,             # One symbol earlier
        consumed_before_detection,  # 3876
        8136 - 7*512,           # 4552 (from total - 7 symbols)
    ]
    
    best_score = 0
    best_pos = 0
    
    for pos in candidate_positions:
        if pos < 0 or pos + 20*512 > len(samples):
            print(f"Position {pos}: Outside valid range")
            continue
            
        print(f"\n--- Testing position {pos} ---")
        symbols = precise_lora_demod(samples, pos)
        
        # Compare first 8 symbols (header)
        matches = sum(1 for i in range(min(8, len(symbols))) if symbols[i] == target[i])
        print(f"Header matches: {matches}/8")
        
        # Compare all 20 symbols
        total_matches = sum(1 for i in range(min(20, len(symbols))) if symbols[i] == target[i])
        print(f"Total matches: {total_matches}/20")
        
        if matches > best_score:
            best_score = matches
            best_pos = pos
            print("*** NEW BEST ***")
    
    print(f"\nBest position: {best_pos} with {best_score}/8 header matches")

if __name__ == "__main__":
    test_precise_positions()
