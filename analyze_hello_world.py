#!/usr/bin/env python3

# Simple script to find where the frames are in the hello world vector
# by looking for characteristic LoRa patterns

import struct
import numpy as np

def load_unknown_file(filename):
    """Load .unknown format (complex float32 little-endian)"""
    with open(filename, 'rb') as f:
        data = f.read()
    samples = struct.unpack('<{}f'.format(len(data)//4), data)
    complex_samples = np.array([samples[i] + 1j*samples[i+1] for i in range(0, len(samples), 2)])
    return complex_samples

def find_strong_signals(samples, window_size=512):
    """Find regions with strong signals (potential preambles)"""
    magnitudes = np.abs(samples)
    
    # Calculate sliding window energy
    energies = []
    for i in range(0, len(magnitudes) - window_size, window_size//4):
        energy = np.mean(magnitudes[i:i+window_size])
        energies.append((i, energy))
    
    # Sort by energy
    energies.sort(key=lambda x: x[1], reverse=True)
    return energies

def analyze_frequency_content(samples, sample_rate=500000):
    """Analyze frequency content to look for chirp patterns"""
    # Take FFT
    fft = np.fft.fft(samples)
    freqs = np.fft.fftfreq(len(samples), 1/sample_rate)
    
    # Find peak frequency
    peak_idx = np.argmax(np.abs(fft))
    peak_freq = freqs[peak_idx]
    
    return peak_freq, np.abs(fft[peak_idx])

def main():
    filename = 'vectors/sps_500k_bw_125k_sf_7_cr_2_ldro_false_crc_true_implheader_false_hello_stupid_world.unknown'
    print(f"Loading {filename}...")
    
    samples = load_unknown_file(filename)
    print(f"Loaded {len(samples)} complex samples")
    
    # Find strong signal regions
    energies = find_strong_signals(samples)
    print(f"\nTop 10 high-energy regions:")
    for i, (pos, energy) in enumerate(energies[:10]):
        percent = 100 * pos / len(samples)
        print(f"  {i+1:2d}: Sample {pos:6d} ({percent:4.1f}%), energy {energy:.6f}")
    
    # Analyze the top energy regions
    print(f"\nAnalyzing top 3 regions:")
    for i, (pos, energy) in enumerate(energies[:3]):
        if pos + 1024 > len(samples):
            continue
            
        chunk = samples[pos:pos+1024]
        peak_freq, peak_mag = analyze_frequency_content(chunk)
        
        print(f"  Region {i+1} (sample {pos}):")
        print(f"    Energy: {energy:.6f}")
        print(f"    Peak freq: {peak_freq/1000:.1f} kHz")
        print(f"    Peak mag: {peak_mag:.1f}")
        
        # Look for patterns in magnitude
        mags = np.abs(chunk)
        print(f"    Magnitude range: {np.min(mags):.3f} - {np.max(mags):.3f}")
        print(f"    Std dev: {np.std(mags):.3f}")

if __name__ == "__main__":
    main()
