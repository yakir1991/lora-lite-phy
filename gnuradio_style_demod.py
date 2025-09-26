#!/usr/bin/env python3
"""
GNU Radio style LoRa demodulation with CFO correction
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

def gnuradio_lora_demod(samples, start_offset, num_symbols=20, sf=7, bw=125000, fs=500000, 
                        cfo_int=9, cfo_frac=0.00164447):
    """
    GNU Radio style LoRa demodulation with CFO correction
    Based on the gr-lora implementation
    """
    N = 2**sf  # 128 for SF7
    samples_per_symbol = int(fs * N / bw)  # 512 for SF7, BW=125kHz, fs=500kHz
    
    print(f"GNU Radio style demod: SF={sf}, N={N}, samples_per_symbol={samples_per_symbol}")
    print(f"CFO correction: int={cfo_int}, frac={cfo_frac}")
    print(f"Starting from offset {start_offset}")
    
    # Apply CFO correction (both integer and fractional)
    total_cfo = cfo_int + cfo_frac
    print(f"Total CFO: {total_cfo:.6f} bins ({total_cfo * bw / N:.1f} Hz)")
    
    symbols = []
    
    for i in range(num_symbols):
        symbol_start = start_offset + i * samples_per_symbol
        symbol_end = symbol_start + samples_per_symbol
        
        if symbol_end > len(samples):
            print(f"Warning: Not enough samples for symbol {i}")
            break
            
        # Extract symbol samples
        symbol_samples = samples[symbol_start:symbol_end]
        
        # Apply fractional CFO correction in time domain
        if cfo_frac != 0:
            t = np.arange(len(symbol_samples)) / fs
            frac_correction = np.exp(-1j * 2 * np.pi * cfo_frac * bw * t / N)
            symbol_samples = symbol_samples * frac_correction
        
        # Generate reference chirp (downchirp for demodulation)
        # GNU Radio style: frequency goes from +BW/2 to -BW/2
        t_symbol = np.arange(samples_per_symbol) / fs
        T = N / bw  # Symbol duration
        
        # Downchirp: frequency decreases from BW/2 to -BW/2
        instantaneous_freq = (bw/2) - (bw * t_symbol / T)
        ref_chirp = np.exp(1j * 2 * np.pi * np.cumsum(instantaneous_freq) / fs)
        
        # Dechirp
        dechirped = symbol_samples * np.conj(ref_chirp)
        
        # Decimate to N samples (from samples_per_symbol)
        decimation_factor = samples_per_symbol // N
        if decimation_factor > 1:
            dechirped_decimated = dechirped[::decimation_factor][:N]
        else:
            dechirped_decimated = dechirped[:N]
        
        # FFT
        fft_result = np.fft.fft(dechirped_decimated, N)
        
        # Apply integer CFO correction in frequency domain
        if cfo_int != 0:
            fft_result = np.roll(fft_result, -cfo_int)
        
        # Find peak
        peak_index = np.argmax(np.abs(fft_result))
        peak_magnitude = np.abs(fft_result[peak_index])
        
        symbols.append(peak_index)
        print(f"Symbol {i:2d}: {peak_index:3d} (mag: {peak_magnitude:.1f})")
    
    return symbols

def test_cfo_variations():
    """Test different CFO corrections to find the best match"""
    samples = load_cf32_file('temp/hello_world.cf32')
    target_symbols = [9, 1, 1, 0, 27, 4, 26, 12, 53, 124, 32, 70, 107, 50, 74, 62, 120, 83, 82, 23]
    
    print(f"Testing CFO variations around detected values...")
    
    # Test different integer CFO values around 9
    cfo_int_range = range(7, 12)
    cfo_frac_range = [0.0, 0.00164447, -0.00164447, 0.003, -0.003]
    
    best_score = 0
    best_params = None
    
    base_offset = 6200  # Our best known offset
    
    for cfo_int in cfo_int_range:
        for cfo_frac in cfo_frac_range:
            print(f"\n--- Testing CFO: int={cfo_int}, frac={cfo_frac:.6f} ---")
            
            try:
                symbols = gnuradio_lora_demod(samples, base_offset, num_symbols=20, 
                                            cfo_int=cfo_int, cfo_frac=cfo_frac)
                
                # Score against target (first 8 symbols for header)
                score = sum(1 for i in range(min(8, len(symbols))) if symbols[i] == target_symbols[i])
                
                print(f"Header score: {score}/8")
                
                if score > best_score:
                    best_score = score
                    best_params = (cfo_int, cfo_frac)
                    print(f"*** NEW BEST: {score}/8 ***")
                    
                    # Show detailed comparison
                    print("Comparison with GNU Radio (first 20 symbols):")
                    for j in range(min(20, len(symbols))):
                        match = "✓" if symbols[j] == target_symbols[j] else "✗"
                        print(f"  Symbol {j:2d}: Our={symbols[j]:3d}, GR={target_symbols[j]:3d} {match}")
                    
            except Exception as e:
                print(f"Error with CFO int={cfo_int}, frac={cfo_frac}: {e}")
    
    print(f"\n=== FINAL RESULTS ===")
    print(f"Best CFO parameters: int={best_params[0] if best_params else 'None'}, frac={best_params[1] if best_params else 'None'}")
    print(f"Best score: {best_score}/8")

def test_offset_variations():
    """Test different offsets around 6200 with fixed CFO"""
    samples = load_cf32_file('temp/hello_world.cf32')
    target_symbols = [9, 1, 1, 0, 27, 4, 26, 12]
    
    print(f"Testing offset variations around 6200...")
    
    best_score = 0
    best_offset = 6200
    
    # Test offsets around 6200 with smaller steps
    for offset in range(6000, 6400, 16):  # 16-sample steps
        try:
            symbols = gnuradio_lora_demod(samples, offset, num_symbols=8, 
                                        cfo_int=9, cfo_frac=0.00164447)
            
            score = sum(1 for i in range(min(8, len(symbols))) if symbols[i] == target_symbols[i])
            
            if score > best_score:
                best_score = score
                best_offset = offset
                print(f"New best at offset {offset}: score {score}/8, symbols {symbols}")
                
        except Exception as e:
            continue
    
    print(f"\nBest offset: {best_offset} with score {best_score}/8")
    return best_offset

if __name__ == "__main__":
    print("GNU Radio Style LoRa Demodulation Test")
    print("=" * 50)
    
    # First test CFO variations
    test_cfo_variations()
    
    print("\n" + "=" * 50)
    
    # Then test offset variations
    test_offset_variations()
