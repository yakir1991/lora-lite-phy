#!/usr/bin/env python3
"""Reverse engineering script to calculate expected symbols from 'Hello New Pipeline'."""

import numpy as np

def hex_to_bytes(hex_string):
    """Convert hex string to bytes."""
    return bytes.fromhex(hex_string.replace(' ', ''))

def bytes_to_nibbles(data):
    """Convert bytes to nibbles."""
    nibbles = []
    for byte in data:
        nibbles.append(byte >> 4)  # High nibble
        nibbles.append(byte & 0x0F)  # Low nibble
    return nibbles

def nibbles_to_bytes(nibbles):
    """Convert nibbles back to bytes."""
    bytes_data = []
    for i in range(0, len(nibbles), 2):
        if i + 1 < len(nibbles):
            high_nibble = nibbles[i]
            low_nibble = nibbles[i + 1]
            byte = (high_nibble << 4) | low_nibble
            bytes_data.append(byte)
        else:
            # Odd number of nibbles, pad with 0
            bytes_data.append(nibbles[i] << 4)
    return bytes(bytes_data)

def whiten_bytes(bytes_data, whiten_seq):
    """Apply whitening to bytes (matching C++ implementation)."""
    whitened = []
    for i, byte in enumerate(bytes_data):
        offset = i % len(whiten_seq)
        whiten_byte = whiten_seq[offset]
        whitened.append(byte ^ whiten_byte)
    return whitened

def encode_hamming(nibble, cr):
    """Encode a nibble using Hamming code (matching C++ implementation)."""
    nibble &= 0xF
    if cr == 1:  # CR=1 -> CR45 (5 bits)
        # Hamming (5,4) encoding from C++ code
        d0 = (nibble >> 0) & 1
        d1 = (nibble >> 1) & 1
        d2 = (nibble >> 2) & 1
        d3 = (nibble >> 3) & 1
        
        p1 = d0 ^ d1 ^ d3
        
        # Construct codeword: [d0, d1, d2, d3, p1] (LSB first)
        codeword = [d0, d1, d2, d3, p1]
        return codeword
    else:
        # For other CR values, use simplified encoding
        data_bits = [(nibble >> i) & 1 for i in range(4)]
        # Pad with zeros to reach cw_len
        codeword = data_bits + [0] * (cr + 4 - 4)
        return codeword

def gray_encode(value):
    """Gray encode a value (matching C++ implementation)."""
    return value ^ (value >> 1)

def gray_decode(value):
    """Gray decode a value (matching C++ implementation)."""
    res = value
    while value > 0:
        value >>= 1
        res ^= value
    return res

def symbol_to_bits(symbol, sf_app):
    """Convert symbol to bits (LSB-first)."""
    bits = []
    for i in range(sf_app):
        bits.append((symbol >> i) & 1)
    return bits

def bits_to_symbol(bits, sf_app):
    """Convert bits to symbol (LSB-first)."""
    symbol = 0
    for i, bit in enumerate(bits):
        symbol |= (bit << i)
    return symbol

def interleave_bits(bits, sf_app, cw_len):
    """Apply diagonal interleaving (matching C++ implementation)."""
    # Create interleaver mapping like in C++ code
    n_in = sf_app * cw_len
    n_out = n_in
    interleaved = [0] * n_out
    
    rows = sf_app
    cols = cw_len
    
    for col in range(cols):
        for row in range(rows):
            dst = col * rows + row
            rotated = (row - col) % rows
            if rotated < 0:
                rotated += rows
            src = rotated * cols + col
            
            if dst < len(interleaved) and src < len(bits):
                interleaved[dst] = bits[src]
    
    return interleaved

def reverse_symbol_shift(natural_value, N_val, sf_app):
    """Reverse symbol shifting to get raw FFT bin (matching C++ implementation)."""
    # C++ forward process:
    # raw = raw_bins[i] & (N - 1u)
    # shifted = (raw + N - 1u) & (N - 1u)
    # gray_decoded = gray_decode(shifted)
    # natural = (gray_decoded + 1u) & mask
    
    # Reverse process:
    # 1. gray_decoded = (natural - 1) & mask
    # 2. shifted = gray_encode(gray_decoded)  # This is wrong! Should be gray_decode inverse
    # 3. raw = (shifted - (N - 1)) & (N - 1)
    
    mask = 0xFFFFFFFF if sf_app >= 32 else ((1 << sf_app) - 1)
    gray_decoded = (natural_value - 1) & mask
    
    # To reverse gray_decode, we need to find the value that when gray_decoded gives us gray_decoded
    # This is complex, so let's try a different approach
    # Let's assume the symbols we have are already the "natural" values
    # and we need to convert them to FFT bins
    
    # For now, let's try: natural -> gray_decoded -> shifted -> raw
    shifted = gray_encode(gray_decoded)  # This might be wrong
    raw = (shifted - (N_val - 1) + N_val) % N_val
    return raw

def main():
    # Expected result from GNU Radio
    expected_hex = "48 65 6c 6c 6f 20 4e 65 77 20 50 69 70 65 6c 69 6e 65"
    expected_bytes = hex_to_bytes(expected_hex)
    expected_text = expected_bytes.decode('utf-8')
    
    print(f"Expected result: '{expected_text}'")
    print(f"Expected hex: {expected_hex}")
    print(f"Expected bytes: {list(expected_bytes)}")
    
    # Convert to nibbles
    expected_nibbles = bytes_to_nibbles(expected_bytes)
    print(f"Expected nibbles: {[hex(n) for n in expected_nibbles]}")
    
    # Whitening sequence (from GNU Radio)
    whiten_seq = [0xff, 0xfe, 0xfc, 0xf8, 0xf0, 0xe1, 0xc2, 0x85, 0x0b, 0x17, 
                  0x2f, 0x5e, 0xbc, 0x78, 0xf1, 0xe3, 0xc6, 0x8d, 0x1a, 0x34]
    
    # Apply whitening to bytes first, then convert to nibbles
    original_bytes = whiten_bytes(expected_bytes, whiten_seq)
    print(f"Original bytes (before whitening): {[hex(b) for b in original_bytes]}")
    
    # Convert to nibbles
    original_nibbles = bytes_to_nibbles(original_bytes)
    print(f"Original nibbles (before whitening): {[hex(n) for n in original_nibbles]}")
    
    # Hamming encoding
    cr = 1  # Coding rate
    cw_len = cr + 4  # 5 bits
    sf_app = 7  # Spreading factor
    
    print(f"\nHamming encoding (CR={cr}, cw_len={cw_len}, sf_app={sf_app}):")
    
    codewords = []
    for i, nibble in enumerate(original_nibbles):
        cw = encode_hamming(nibble, cr)
        codewords.extend(cw)
        print(f"Nibble[{i}] = {hex(nibble)} -> CW = {cw}")
    
    print(f"All codewords: {codewords}")
    
    # Convert codewords to symbols
    print(f"\nConverting codewords to symbols:")
    
    symbols = []
    for i in range(0, len(codewords), sf_app):
        symbol_bits = codewords[i:i+sf_app]
        if len(symbol_bits) == sf_app:
            symbol = bits_to_symbol(symbol_bits, sf_app)
            symbols.append(symbol)
            print(f"Symbol[{len(symbols)-1}]: bits={symbol_bits} -> symbol={symbol}")
    
    print(f"\nExpected symbols: {symbols}")
    
    # Apply interleaving
    print(f"\nApplying interleaving:")
    interleaved_bits = interleave_bits(codewords, sf_app, cw_len)
    print(f"Interleaved bits: {interleaved_bits}")
    
    # Convert interleaved bits back to symbols
    interleaved_symbols = []
    for i in range(0, len(interleaved_bits), sf_app):
        symbol_bits = interleaved_bits[i:i+sf_app]
        if len(symbol_bits) == sf_app:
            symbol = bits_to_symbol(symbol_bits, sf_app)
            interleaved_symbols.append(symbol)
    
    print(f"Interleaved symbols: {interleaved_symbols}")
    
    # Apply gray encoding
    print(f"\nApplying gray encoding:")
    gray_symbols = []
    for symbol in interleaved_symbols:
        gray_symbol = gray_encode(symbol)
        gray_symbols.append(gray_symbol)
        print(f"Symbol {symbol} -> Gray {gray_symbol}")
    
    print(f"Gray encoded symbols: {gray_symbols}")
    
    # Apply symbol shifting (reverse of what receiver does)
    print(f"\nApplying symbol shifting (reverse):")
    N = 128  # 2^7
    shifted_symbols = []
    for gray_symbol in gray_symbols:
        # C++ forward process:
        # raw = raw_bins[i] & (N - 1u)
        # shifted = (raw + N - 1u) & (N - 1u)
        # gray_decoded = gray_decode(shifted)
        # natural = (gray_decoded + 1u) & mask
        
        # Reverse process:
        # 1. gray_decoded = (natural - 1) & mask
        # 2. shifted = gray_encode(gray_decoded)  # This is wrong! Should be gray_decode inverse
        # 3. raw = (shifted - (N - 1)) & (N - 1)
        
        # For now, let's try: natural -> gray_decoded -> shifted -> raw
        mask = 0xFFFFFFFF if sf_app >= 32 else ((1 << sf_app) - 1)
        gray_decoded = (gray_symbol - 1) & mask
        shifted = gray_encode(gray_decoded)  # This might be wrong
        raw = (shifted - (N - 1) + N) % N
        shifted_symbols.append(raw)
        print(f"Gray {gray_symbol} -> Raw {raw}")
    
    print(f"\nFinal expected FFT bins: {shifted_symbols}")
    
    # Compare with what we got
    our_symbols = [2, 120, 85, 121, 81, 42, 102, 80, 98, 70]
    print(f"\nOur FFT bins: {our_symbols}")
    print(f"Expected FFT bins: {shifted_symbols[:len(our_symbols)]}")
    
    print(f"\nComparison:")
    for i, (ours, expected) in enumerate(zip(our_symbols, shifted_symbols[:len(our_symbols)])):
        match = "✓" if ours == expected else "✗"
        print(f"Symbol[{i}]: ours={ours}, expected={expected} {match}")

if __name__ == "__main__":
    main()
