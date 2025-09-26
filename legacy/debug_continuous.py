#!/usr/bin/env python3
"""
Check if our extracted symbols match any subsequence of GNU Radio symbols
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

def check_subsequence_match():
    """Check if our symbols match any subsequence of GNU Radio symbols"""
    # GNU Radio complete symbol sequence
    gr_symbols = [9, 1, 1, 0, 27, 4, 26, 12, 53, 124, 32, 70, 107, 50, 74, 62, 120, 83, 82, 23]
    
    # Our best extracted symbols
    samples = load_cf32_file('temp/hello_world.cf32')
    our_symbols = extract_symbols_simple(samples, 10976, 20)  # Extract 20 symbols
    
    print(f"GNU Radio symbols: {gr_symbols}")
    print(f"Our symbols:       {our_symbols}")
    print()
    
    # Check all possible alignments
    best_score = 0
    best_alignment = 0
    
    for offset in range(len(our_symbols)):
        for start in range(len(gr_symbols) - 8 + 1):
            gr_subseq = gr_symbols[start:start+8]
            our_subseq = our_symbols[offset:offset+8] if offset+8 <= len(our_symbols) else []
            
            if len(our_subseq) < 8:
                continue
                
            matches = sum(1 for i in range(8) if our_subseq[i] == gr_subseq[i])
            
            if matches > best_score:
                best_score = matches
                best_alignment = (offset, start)
                print(f"NEW BEST: our_offset={offset}, gr_start={start}, matches={matches}/8")
                print(f"  Our subseq: {our_subseq}")
                print(f"  GR subseq:  {gr_subseq}")
    
    print(f"\nBest alignment: our_offset={best_alignment[0]}, gr_start={best_alignment[1]}, matches={best_score}/8")
    
    # Also check if there's a consistent offset pattern
    print(f"\nChecking for offset patterns...")
    our_8 = our_symbols[:8]
    gr_8 = gr_symbols[:8]
    
    # Check if our symbols are shifted versions
    for shift in range(1, 128):
        shifted_ours = [(s + shift) % 128 for s in our_8]
        matches = sum(1 for i in range(8) if shifted_ours[i] == gr_8[i])
        if matches >= 3:
            print(f"Shift by +{shift}: {matches}/8 matches, shifted={shifted_ours}")
            
    for shift in range(1, 128):
        shifted_ours = [(s - shift) % 128 for s in our_8]
        matches = sum(1 for i in range(8) if shifted_ours[i] == gr_8[i])
        if matches >= 3:
            print(f"Shift by -{shift}: {matches}/8 matches, shifted={shifted_ours}")

def extract_symbols_simple(samples, start_offset, num_symbols):
    """Simple symbol extraction"""
    N = 128
    samples_per_symbol = 512
    symbols = []
    
    for i in range(num_symbols):
        sym_start = start_offset + i * samples_per_symbol
        if sym_start + samples_per_symbol > len(samples):
            break
            
        sym_samples = samples[sym_start:sym_start + samples_per_symbol]
        
        # Simple decimation
        decimated = sym_samples[::4][:N]
        fft_result = np.fft.fft(decimated)
        peak = np.argmax(np.abs(fft_result))
        
        symbols.append(peak)
    
    return symbols

def test_different_positions():
    """Test extracting symbols from different positions to see if we can find the start"""
    samples = load_cf32_file('temp/hello_world.cf32')
    gr_symbols = [9, 1, 1, 0, 27, 4, 26, 12]
    
    print("Testing different positions to find the start of symbols...")
    
    # Test a wider range of positions
    positions_to_test = []
    
    # Around our current best
    positions_to_test.extend(range(10000, 12000, 128))
    
    # Earlier positions (maybe frame starts earlier)
    positions_to_test.extend(range(2000, 6000, 256))
    
    # Later positions (maybe frame starts later)
    positions_to_test.extend(range(12000, 16000, 256))
    
    best_score = 0
    best_pos = 0
    
    for pos in positions_to_test:
        if pos + 8*512 > len(samples):
            continue
            
        symbols = extract_symbols_simple(samples, pos, 8)
        matches = sum(1 for i in range(8) if symbols[i] == gr_symbols[i])
        
        if matches > best_score:
            best_score = matches
            best_pos = pos
            print(f"NEW BEST: pos={pos}, matches={matches}/8, symbols={symbols}")
    
    print(f"\nBest position found: {best_pos} with {best_score}/8 matches")

if __name__ == "__main__":
    check_subsequence_match()
    print("\n" + "="*60)
    test_different_positions()
