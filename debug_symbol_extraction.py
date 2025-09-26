#!/usr/bin/env python3
"""
Test to understand the difference between our symbols and GNU Radio's
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

def gray_decode(gray_val):
    """Convert Gray code to binary"""
    binary_val = gray_val
    while gray_val:
        gray_val >>= 1
        binary_val ^= gray_val
    return binary_val

def gray_encode(binary_val):
    """Convert binary to Gray code"""
    return binary_val ^ (binary_val >> 1)

def test_different_approaches():
    """Test different demodulation approaches to find what matches GNU Radio"""
    samples = load_cf32_file('temp/hello_world.cf32')
    
    # GNU Radio results
    gr_fft_symbols = [9, 1, 1, 0, 27, 4, 26, 12, 53, 124, 32, 70, 107, 50, 74, 62, 120, 83, 82, 23]
    gr_gray_symbols = [13, 1, 1, 0, 22, 6, 23, 10, 47, 66, 48, 101, 94, 43, 111, 33, 68, 122, 123, 28]
    
    print("GNU Radio FFT symbols (first 20):", gr_fft_symbols)
    print("GNU Radio Gray symbols (first 20):", gr_gray_symbols)
    print()
    
    # Test our best offset
    offset = 6096
    N = 128
    samples_per_symbol = 512
    
    print(f"Testing offset {offset}...")
    
    # Test different demodulation approaches
    approaches = [
        ("Basic FFT", basic_fft_demod),
        ("Conjugate chirp", conjugate_chirp_demod),
        ("GNU Radio style", gnuradio_style_demod_simple),
    ]
    
    for name, demod_func in approaches:
        print(f"\n--- {name} ---")
        try:
            our_symbols = demod_func(samples, offset)
            
            # Compare with GNU Radio FFT symbols
            fft_matches = sum(1 for i in range(min(len(our_symbols), 20)) 
                             if our_symbols[i] == gr_fft_symbols[i])
            
            # Convert our symbols to gray and compare
            our_gray = [gray_encode(s) for s in our_symbols[:20]]
            gray_matches = sum(1 for i in range(min(len(our_gray), 20)) 
                              if our_gray[i] == gr_gray_symbols[i])
            
            print(f"FFT matches: {fft_matches}/20")
            print(f"Gray matches: {gray_matches}/20")
            print(f"Our symbols: {our_symbols[:8]}")
            print(f"Our gray:    {our_gray[:8]}")
            
            if fft_matches > 5 or gray_matches > 5:
                print("*** GOOD MATCH! ***")
                
        except Exception as e:
            print(f"Error: {e}")

def basic_fft_demod(samples, offset, num_symbols=20):
    """Basic FFT demodulation"""
    symbols = []
    N = 128
    samples_per_symbol = 512
    
    for i in range(num_symbols):
        start = offset + i * samples_per_symbol
        if start + samples_per_symbol > len(samples):
            break
        
        symbol_samples = samples[start:start + samples_per_symbol]
        
        # Simple decimation to N samples
        decimated = symbol_samples[::4][:N]  # Take every 4th sample
        
        # FFT
        fft_result = np.fft.fft(decimated)
        peak_idx = np.argmax(np.abs(fft_result))
        symbols.append(peak_idx)
    
    return symbols

def conjugate_chirp_demod(samples, offset, num_symbols=20):
    """Use conjugate chirp for demodulation"""
    symbols = []
    N = 128
    samples_per_symbol = 512
    bw = 125000
    fs = 500000
    
    # Create reference downchirp
    t = np.arange(samples_per_symbol) / fs
    T_symbol = N / bw
    freq = bw/2 - bw * (t / T_symbol)
    ref_chirp = np.exp(-1j * 2 * np.pi * np.cumsum(freq) / fs)
    
    for i in range(num_symbols):
        start = offset + i * samples_per_symbol
        if start + samples_per_symbol > len(samples):
            break
        
        symbol_samples = samples[start:start + samples_per_symbol]
        
        # Dechirp with conjugate
        dechirped = symbol_samples * np.conj(ref_chirp)
        
        # Decimate
        decimated = dechirped[::4][:N]
        
        # FFT
        fft_result = np.fft.fft(decimated)
        peak_idx = np.argmax(np.abs(fft_result))
        symbols.append(peak_idx)
    
    return symbols

def gnuradio_style_demod_simple(samples, offset, num_symbols=20):
    """Simple GNU Radio style"""
    symbols = []
    N = 128
    samples_per_symbol = 512
    
    for i in range(num_symbols):
        start = offset + i * samples_per_symbol
        if start + samples_per_symbol > len(samples):
            break
        
        symbol_samples = samples[start:start + samples_per_symbol]
        
        # Create ideal LoRa downchirp in discrete time
        k = np.arange(samples_per_symbol)
        # LoRa chirp: exp(-j * π * k * (k-N) / (N*oversample))
        oversample = 4
        downchirp = np.exp(-1j * np.pi * k * k / (N * oversample))
        
        # Dechirp
        dechirped = symbol_samples * downchirp
        
        # Sum every 4 samples (integrate over oversampling)
        integrated = np.sum(dechirped.reshape(-1, 4), axis=1)
        
        # FFT
        fft_result = np.fft.fft(integrated, N)
        peak_idx = np.argmax(np.abs(fft_result))
        symbols.append(peak_idx)
    
    return symbols

if __name__ == "__main__":
    test_different_approaches()
