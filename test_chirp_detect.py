#!/usr/bin/env python3
"""
Try different timing offsets within symbols and different CFO corrections
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

def test_micro_timing_offsets():
    """Test tiny timing offsets within the symbol"""
    samples = load_cf32_file('temp/hello_world.cf32')
    target = [9, 1, 1, 0, 27, 4, 26, 12]
    base_pos = 10976
    
    print("Testing micro timing offsets...")
    
    best_score = 2  # We already have 2 matches
    best_offset = 0
    
    # Test offsets within one symbol period (±64 samples in steps of 4)
    for offset in range(-64, 65, 4):
        test_pos = base_pos + offset
        
        if test_pos < 0 or test_pos + 8*512 > len(samples):
            continue
            
        symbols = extract_symbols_simple(samples, test_pos)
        matches = sum(1 for i in range(8) if symbols[i] == target[i])
        
        if matches > best_score:
            best_score = matches
            best_offset = offset
            print(f"NEW BEST: offset={offset}, matches={matches}/8, symbols={symbols}")
    
    print(f"Best micro offset: {best_offset} samples, matches: {best_score}/8")
    return base_pos + best_offset

def test_cfo_corrections(base_pos):
    """Test different CFO corrections"""
    samples = load_cf32_file('temp/hello_world.cf32')
    target = [9, 1, 1, 0, 27, 4, 26, 12]
    
    print(f"\nTesting CFO corrections at position {base_pos}...")
    
    # Different CFO values to test (in Hz)
    cfo_values = [0, 1000, 2000, 5000, 8000, -1000, -2000, -5000, -8000]
    fs = 500000
    
    best_score = 0
    best_cfo = 0
    
    for cfo_hz in cfo_values:
        # Apply CFO correction
        corrected_samples = apply_cfo_correction(samples, cfo_hz, fs)
        
        # Extract symbols
        symbols = extract_symbols_simple(corrected_samples, base_pos)
        matches = sum(1 for i in range(8) if symbols[i] == target[i])
        
        print(f"CFO {cfo_hz:5d} Hz: matches={matches}/8, symbols={symbols[:4]}...")
        
        if matches > best_score:
            best_score = matches
            best_cfo = cfo_hz
    
    print(f"Best CFO: {best_cfo} Hz, matches: {best_score}/8")
    return best_cfo

def apply_cfo_correction(samples, cfo_hz, fs):
    """Apply CFO correction"""
    t = np.arange(len(samples)) / fs
    correction = np.exp(-1j * 2 * np.pi * cfo_hz * t)
    return samples * correction

def extract_symbols_simple(samples, start_offset):
    """Simple symbol extraction"""
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

def test_phase_corrections(base_pos):
    """Test different phase corrections"""
    samples = load_cf32_file('temp/hello_world.cf32')
    target = [9, 1, 1, 0, 27, 4, 26, 12]
    
    print(f"\nTesting phase corrections at position {base_pos}...")
    
    best_score = 0
    best_phase = 0
    
    # Test different phase offsets
    for phase_deg in range(0, 360, 15):
        phase_rad = np.deg2rad(phase_deg)
        
        # Apply phase correction
        corrected_samples = samples * np.exp(-1j * phase_rad)
        
        # Extract symbols
        symbols = extract_symbols_simple(corrected_samples, base_pos)
        matches = sum(1 for i in range(8) if symbols[i] == target[i])
        
        if matches > best_score:
            best_score = matches
            best_phase = phase_deg
            print(f"NEW BEST: phase={phase_deg}°, matches={matches}/8, symbols={symbols}")
    
    print(f"Best phase: {best_phase}°, matches: {best_score}/8")

def test_symbol_boundary_shifts(base_pos):
    """Test shifting the symbol boundaries"""
    samples = load_cf32_file('temp/hello_world.cf32')
    target = [9, 1, 1, 0, 27, 4, 26, 12]
    
    print(f"\nTesting symbol boundary shifts at position {base_pos}...")
    
    best_score = 0
    best_shift = 0
    
    # Test different symbol period lengths around 512
    for samples_per_symbol in range(480, 545, 4):
        symbols = extract_symbols_variable_period(samples, base_pos, samples_per_symbol)
        matches = sum(1 for i in range(8) if symbols[i] == target[i])
        
        if matches > best_score:
            best_score = matches
            best_shift = samples_per_symbol - 512
            print(f"NEW BEST: symbol_period={samples_per_symbol} (shift={best_shift}), matches={matches}/8, symbols={symbols}")
    
    print(f"Best symbol period shift: {best_shift} samples, matches: {best_score}/8")

def extract_symbols_variable_period(samples, start_offset, samples_per_symbol):
    """Extract symbols with variable symbol period"""
    N = 128
    symbols = []
    
    for i in range(8):
        sym_start = start_offset + i * samples_per_symbol
        if sym_start + samples_per_symbol > len(samples):
            break
            
        sym_samples = samples[sym_start:sym_start + samples_per_symbol]
        
        # Decimate to N samples
        step = len(sym_samples) // N
        if step > 0:
            decimated = sym_samples[::step][:N]
        else:
            decimated = np.pad(sym_samples, (0, N - len(sym_samples)), 'constant')[:N]
        
        fft_result = np.fft.fft(decimated)
        peak = np.argmax(np.abs(fft_result))
        
        symbols.append(peak)
    
    return symbols

if __name__ == "__main__":
    # Start with our best known position
    best_pos = test_micro_timing_offsets()
    
    # Test CFO corrections
    best_cfo = test_cfo_corrections(best_pos)
    
    # Test phase corrections
    test_phase_corrections(best_pos)
    
    # Test symbol boundary shifts
    test_symbol_boundary_shifts(best_pos)
