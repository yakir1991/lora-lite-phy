#!/usr/bin/env python3
"""
Step-by-step implementation to match GNU Radio's processing
"""

import struct
import numpy as np

def load_unknown_file(filename):
    """Load .unknown format (complex float32 little-endian)"""
    with open(filename, 'rb') as f:
        data = f.read()
    samples = struct.unpack('<{}f'.format(len(data)//4), data)
    complex_samples = np.array([samples[i] + 1j*samples[i+1] for i in range(0, len(samples), 2)])
    return complex_samples

def gray_mapping_decode_table():
    """Create Gray code decode table for LoRa SF7 (7-bit symbols)"""
    # Gray to binary decoding for 7-bit values
    decode_table = {}
    for i in range(128):  # 2^7 = 128
        # Convert binary to Gray
        gray = i ^ (i >> 1)
        decode_table[gray] = i
    return decode_table

def test_gray_mapping():
    """Test our Gray mapping against GNU Radio results"""
    print("=== Gray Mapping Test ===")
    
    # GNU Radio results
    fft_symbols = [9, 1, 1, 0, 27, 4, 26, 12, 53, 124, 32, 70, 107, 50, 74, 62, 120, 83, 82, 23]
    gr_gray_symbols = [13, 1, 1, 0, 22, 6, 23, 10, 47, 66, 48, 101, 94, 43, 111, 33, 68, 122, 123, 28]
    
    # Our Gray decode table
    gray_decode = gray_mapping_decode_table()
    
    print("FFT -> Gray mapping comparison:")
    matches = 0
    for i in range(len(fft_symbols)):
        fft_val = fft_symbols[i]
        gr_gray = gr_gray_symbols[i]
        
        # GNU Radio seems to do: symbol -> Gray decode
        # But let's check both directions
        our_gray = fft_val ^ (fft_val >> 1)  # Binary to Gray
        our_binary = gray_decode.get(fft_val, -1)  # Gray to Binary
        
        match = "✓" if gr_gray == our_binary else "✗"
        if gr_gray == our_binary:
            matches += 1
            
        print(f"  Symbol {i:2d}: FFT={fft_val:3d} -> GR_Gray={gr_gray:3d}, Our_Gray_to_Bin={our_binary:3d} {match}")
    
    print(f"\nMatches: {matches}/{len(fft_symbols)}")
    return matches == len(fft_symbols)

def analyze_lora_header():
    """Analyze the LoRa header structure"""
    print("\n=== LoRa Header Analysis ===")
    
    # From GNU Radio: first 8 symbols should be header
    header_symbols_fft = [9, 1, 1, 0, 27, 4, 26, 12]
    header_symbols_gray = [13, 1, 1, 0, 22, 6, 23, 10]
    
    print("Header symbols:")
    print(f"  FFT:  {header_symbols_fft}")
    print(f"  Gray: {header_symbols_gray}")
    
    # Expected header for 18-byte payload, CR=2, CRC=true:
    # Payload length = 18 (0x12)
    # CR = 2 (0b010) 
    # CRC = 1 (0b1)
    print("\nExpected header information:")
    print("  Payload length: 18 bytes")
    print("  Coding rate: 2") 
    print("  CRC present: True")

def simulate_our_processing():
    """Simulate what our code should do to match GNU Radio"""
    print("\n=== Our Processing Simulation ===")
    
    print("✓ Step 1: Frame sync with sync words (93, 112)")
    print("    - We successfully detect frame at iteration 8")
    print("    - CFO: 9 + 0.00164447")
    print("    - SNR: -16.9468 dB")
    
    print("\n✗ Step 2: FFT Demodulation") 
    print("    - Should produce symbols: [9, 1, 1, 0, 27, 4, 26, 12, ...]")
    print("    - Need to implement CFO correction in frequency domain")
    print("    - Current issue: symbol_out is empty")
    
    print("\n○ Step 3: Gray mapping")
    print("    - Should convert to: [13, 1, 1, 0, 22, 6, 23, 10, ...]")
    
    print("\n○ Step 4: Deinterleaving & Hamming decode")
    print("    - Process 8 header symbols -> extract header info")
    print("    - Continue with payload symbols")
    
    print("\n○ Step 5: Dewhitening & CRC check")
    print("    - Apply LoRa whitening sequence")
    print("    - Verify CRC")
    print("    - Extract final payload: 'hello stupid world'")

def identify_next_steps():
    """Identify what we need to implement next"""
    print("\n=== Next Implementation Steps ===")
    
    print("🎯 IMMEDIATE: Fix frame sync symbol extraction")
    print("   - Debug why symbol_out is empty in FrameSyncResult")
    print("   - Ensure symbols are properly extracted after CFO correction")
    
    print("\n🎯 PRIORITY 1: Implement FFT demodulation chain")
    print("   - Extract symbols from frame sync output")
    print("   - Apply CFO correction: cfo_int=9, cfo_frac=0.00164447")
    print("   - Match GNU Radio's FFT demod output")
    
    print("\n🎯 PRIORITY 2: Add processing pipeline") 
    print("   - Gray code mapping (working in theory)")
    print("   - Deinterleaving for CR=2")
    print("   - Hamming decoding")
    print("   - Dewhitening")
    print("   - CRC verification")
    
    print("\n📋 SUCCESS CRITERIA:")
    print("   - Extract symbols: [9, 1, 1, 0, 27, 4, 26, 12, ...]")
    print("   - Decode header: payload_len=18, CR=2, CRC=true")
    print("   - Extract payload: 'hello stupid world'")

def main():
    gray_works = test_gray_mapping()
    analyze_lora_header()
    simulate_our_processing()
    identify_next_steps()
    
    if gray_works:
        print("\n✅ Gray mapping algorithm is correct!")
    else:
        print("\n❌ Gray mapping needs adjustment")

if __name__ == "__main__":
    main()
