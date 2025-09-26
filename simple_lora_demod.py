#!/usr/bin/env python3
"""
Simple FFT-based LoRa demodulation to match GNU Radio approach
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

def simple_lora_demod(samples, start_offset, num_symbols=20, sf=7):
    """Simple LoRa demodulation similar to GNU Radio approach"""
    
    # LoRa parameters
    N = 2**sf  # 128 for SF7
    oversample = 4  # GNU Radio typically uses 4x oversampling
    samples_per_symbol = N * oversample  # 512 samples per symbol
    
    print(f"Using SF={sf}, N={N}, samples_per_symbol={samples_per_symbol}")
    print(f"Starting from offset {start_offset}")
    
    symbols = []
    
    for i in range(num_symbols):
        symbol_start = start_offset + i * samples_per_symbol
        
        if symbol_start + samples_per_symbol > len(samples):
            print(f"Symbol {i}: Not enough samples")
            break
            
        # Extract oversampled symbol
        symbol_samples = samples[symbol_start:symbol_start + samples_per_symbol]
        
        # Downsample to base rate (take every oversample-th sample)
        downsampled = symbol_samples[::oversample]
        
        # Ensure we have exactly N samples
        if len(downsampled) > N:
            downsampled = downsampled[:N]
        elif len(downsampled) < N:
            # Pad with zeros
            downsampled = np.pad(downsampled, (0, N - len(downsampled)), 'constant')
        
        # Create ideal downchirp for dechirping
        # LoRa downchirp: exp(-j * π * k^2 / N) for k = 0, 1, ..., N-1
        k = np.arange(N)
        downchirp = np.exp(-1j * np.pi * k * k / N)
        
        # Dechirp (multiply by conjugate of upchirp)
        dechirped = downsampled * downchirp
        
        # FFT to get frequency domain
        fft_result = np.fft.fft(dechirped, N)
        
        # Find peak (symbol value)
        symbol_value = np.argmax(np.abs(fft_result))
        peak_magnitude = np.abs(fft_result[symbol_value])
        
        symbols.append(symbol_value)
        print(f"Symbol {i:2d}: {symbol_value:3d} (mag: {peak_magnitude:.1f})")
    
    return symbols

def find_frame_start():
    """Find the start of the LoRa frame by looking for preamble pattern"""
    samples = load_cf32_file('temp/hello_world.cf32')
    
    # We know GNU Radio found the frame and got these symbols
    target_symbols = [9, 1, 1, 0, 27, 4, 26, 12]
    
    print(f"Loaded {len(samples)} samples")
    print(f"Looking for target symbols: {target_symbols}")
    
    best_score = 0
    best_offset = 0
    best_symbols = []
    
    # Search in the region where we expect the frame
    # Based on our frame sync debug, it should be around sample 4000-6000
    search_start = 3000
    search_end = 8000
    search_step = 128  # Search every 128 samples
    
    for offset in range(search_start, search_end, search_step):
        if offset + 8 * 512 > len(samples):  # Need at least 8 symbols worth of samples
            break
            
        # Extract first 8 symbols (header)
        try:
            symbols = simple_lora_demod(samples, offset, num_symbols=8)
        except:
            continue
            
        if len(symbols) < 8:
            continue
            
        # Score against target
        score = sum(1 for i in range(8) if symbols[i] == target_symbols[i])
        
        if score > best_score:
            best_score = score
            best_offset = offset
            best_symbols = symbols
            print(f"New best at offset {offset}: score {score}/8, symbols {symbols}")
    
    print(f"\nBest match:")
    print(f"  Offset: {best_offset}")
    print(f"  Score: {best_score}/8")
    print(f"  Symbols: {best_symbols}")
    print(f"  Target:  {target_symbols}")
    
    return best_offset, best_symbols

def fine_search_around_best(best_offset):
    """Fine search around the best offset with smaller steps"""
    samples = load_cf32_file('temp/hello_world.cf32')
    target_symbols = [9, 1, 1, 0, 27, 4, 26, 12]
    
    print(f"\n=== Fine search around offset {best_offset} ===")
    
    best_score = 0
    best_fine_offset = best_offset
    best_symbols = []
    
    # Search with smaller steps around the best position
    for delta in range(-256, 257, 32):  # Search ±256 samples with 32-sample steps
        fine_offset = best_offset + delta
        
        if fine_offset < 0 or fine_offset + 8 * 512 > len(samples):
            continue
            
        try:
            symbols = simple_lora_demod(samples, fine_offset, num_symbols=8)
        except:
            continue
            
        if len(symbols) < 8:
            continue
            
        # Score against target
        score = sum(1 for i in range(8) if symbols[i] == target_symbols[i])
        
        if score > best_score:
            best_score = score
            best_fine_offset = fine_offset
            best_symbols = symbols
            print(f"  Better match at offset {fine_offset} (delta {delta:+3d}): score {score}/8")
            print(f"    Symbols: {symbols}")
    
    return best_fine_offset, best_symbols, best_score

def main():
    print("=== Simple LoRa Demodulation Test ===")
    
    # First find the frame start
    best_offset, header_symbols = find_frame_start()
    
    if best_offset > 0:
        print(f"\nDoing fine search around best offset...")
        fine_offset, fine_symbols, fine_score = fine_search_around_best(best_offset)
        
        if fine_score > 1:  # If we found a better position
            best_offset = fine_offset
            print(f"\nUsing refined offset: {fine_offset}")
        
        print(f"\nExtracting more symbols from position {best_offset}...")
        samples = load_cf32_file('temp/hello_world.cf32')
        
        # Extract first 20 symbols to see header + some payload
        all_symbols = simple_lora_demod(samples, best_offset, num_symbols=20)
        
        # Compare with GNU Radio results
        gr_symbols = [9, 1, 1, 0, 27, 4, 26, 12, 53, 124, 32, 70, 107, 50, 74, 62, 120, 83, 82, 23]
        
        print(f"\nComparison with GNU Radio:")
        matches = 0
        for i, (our, gr) in enumerate(zip(all_symbols, gr_symbols)):
            match = "✓" if our == gr else "✗"
            if our == gr:
                matches += 1
            print(f"  Symbol {i:2d}: Our={our:3d}, GR={gr:3d} {match}")
        
        print(f"\nTotal matches: {matches}/{len(all_symbols)}")
        
        if matches >= len(all_symbols) * 0.7:  # 70% match
            print("🎉 SUCCESS! Symbol extraction is working!")
        else:
            print("🔧 Still needs improvement, but getting closer...")

if __name__ == "__main__":
    main()
