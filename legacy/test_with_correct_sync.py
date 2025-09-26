#!/usr/bin/env python3
"""
Analyze the promising position found at 10976
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

def fine_tune_around_position(base_pos=10976):
    """Fine tune around the promising position"""
    samples = load_cf32_file('temp/hello_world.cf32')
    target = [9, 1, 1, 0, 27, 4, 26, 12]
    
    print(f"Fine tuning around position {base_pos}")
    print(f"Target: {target}")
    
    best_score = 0
    best_pos = base_pos
    best_symbols = []
    
    # Test positions around the base with finer steps
    for offset in range(-128, 129, 8):  # ±128 samples in steps of 8
        test_pos = base_pos + offset
        
        if test_pos < 0 or test_pos + 8*512 > len(samples):
            continue
            
        symbols = extract_symbols_detailed(samples, test_pos)
        matches = sum(1 for i in range(8) if symbols[i] == target[i])
        
        if matches > best_score:
            best_score = matches
            best_pos = test_pos
            best_symbols = symbols
            print(f"NEW BEST: pos={test_pos} (offset={offset}), matches={matches}/8")
            print(f"  Our symbols: {symbols}")
            print(f"  Target:      {target}")
            print(f"  Matches:     {['✓' if symbols[i] == target[i] else '✗' for i in range(8)]}")
    
    print(f"\nBest position: {best_pos} with {best_score}/8 matches")
    
    # Test this position with different demodulation methods
    print(f"\nTesting different demodulation methods at position {best_pos}:")
    
    methods = [
        ("Simple decimation", method_simple_decimation),
        ("Chirp correlation", method_chirp_correlation),
        ("GNU Radio style", method_gnuradio_style),
    ]
    
    for name, method in methods:
        symbols = method(samples, best_pos)
        matches = sum(1 for i in range(8) if symbols[i] == target[i])
        print(f"{name:20}: {symbols}, matches={matches}/8")

def extract_symbols_detailed(samples, start_offset):
    """Extract symbols with detailed logging"""
    N = 128
    samples_per_symbol = 512
    symbols = []
    
    for i in range(8):
        sym_start = start_offset + i * samples_per_symbol
        sym_samples = samples[sym_start:sym_start + samples_per_symbol]
        
        # Simple decimation
        decimated = sym_samples[::4][:N]
        fft_result = np.fft.fft(decimated)
        peak = np.argmax(np.abs(fft_result))
        
        symbols.append(peak)
    
    return symbols

def method_simple_decimation(samples, start_offset):
    """Simple decimation method"""
    return extract_symbols_detailed(samples, start_offset)

def method_chirp_correlation(samples, start_offset):
    """Chirp correlation method"""
    N = 128
    samples_per_symbol = 512
    symbols = []
    bw = 125000
    fs = 500000
    
    # Create reference downchirp
    t = np.arange(samples_per_symbol) / fs
    T_symbol = N / bw
    
    # Downchirp: frequency decreases from BW/2 to -BW/2
    freq = bw/2 - bw * (t / T_symbol)
    ref_chirp = np.exp(1j * 2 * np.pi * np.cumsum(freq) / fs)
    
    for i in range(8):
        sym_start = start_offset + i * samples_per_symbol
        sym_samples = samples[sym_start:sym_start + samples_per_symbol]
        
        # Correlate with downchirp
        correlated = sym_samples * np.conj(ref_chirp)
        
        # Decimate
        decimated = correlated[::4][:N]
        
        # FFT
        fft_result = np.fft.fft(decimated)
        peak = np.argmax(np.abs(fft_result))
        
        symbols.append(peak)
    
    return symbols

def method_gnuradio_style(samples, start_offset):
    """GNU Radio style with integrate and dump"""
    N = 128
    samples_per_symbol = 512
    symbols = []
    
    for i in range(8):
        sym_start = start_offset + i * samples_per_symbol
        sym_samples = samples[sym_start:sym_start + samples_per_symbol]
        
        # Create discrete chirp
        k = np.arange(samples_per_symbol)
        oversample = 4
        # Discrete LoRa downchirp
        downchirp = np.exp(-1j * np.pi * k * k / (N * oversample))
        
        # Dechirp
        dechirped = sym_samples * downchirp
        
        # Integrate (sum every 4 samples)
        integrated = np.sum(dechirped.reshape(-1, 4), axis=1)
        
        # FFT
        fft_result = np.fft.fft(integrated, N)
        peak = np.argmax(np.abs(fft_result))
        
        symbols.append(peak)
    
    return symbols

def analyze_fft_spectrum(pos=10976):
    """Analyze the FFT spectrum of symbols at the best position"""
    samples = load_cf32_file('temp/hello_world.cf32')
    N = 128
    samples_per_symbol = 512
    
    print(f"\nAnalyzing FFT spectrum at position {pos}")
    
    for i in range(4):  # Analyze first 4 symbols
        sym_start = pos + i * samples_per_symbol
        sym_samples = samples[sym_start:sym_start + samples_per_symbol]
        
        # Simple method
        decimated = sym_samples[::4][:N]
        fft_result = np.fft.fft(decimated)
        fft_mag = np.abs(fft_result)
        
        peak_idx = np.argmax(fft_mag)
        peak_mag = fft_mag[peak_idx]
        
        # Find second highest peak
        fft_mag_copy = fft_mag.copy()
        fft_mag_copy[peak_idx] = 0
        second_idx = np.argmax(fft_mag_copy)
        second_mag = fft_mag_copy[second_idx]
        
        print(f"Symbol {i}: peak at {peak_idx} (mag={peak_mag:.1f}), second at {second_idx} (mag={second_mag:.1f}), ratio={peak_mag/second_mag:.2f}")
        
        # Show top 5 peaks
        sorted_indices = np.argsort(fft_mag)[::-1][:5]
        peaks_info = [(idx, fft_mag[idx]) for idx in sorted_indices]
        print(f"  Top 5: {peaks_info}")

if __name__ == "__main__":
    fine_tune_around_position()
    analyze_fft_spectrum()
