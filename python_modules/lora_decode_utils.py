#!/usr/bin/env python3
# This file provides the 'lora decode utils' functionality for the LoRa Lite PHY toolkit.
"""
LoRa Decoding Utilities - Python implementation of LoRa decoding chain
Based on the C++ implementations in src/
"""

# Imports the module(s) numpy as np.
import numpy as np
# Imports specific objects with 'from typing import List'.
from typing import List

# Whitening sequence from C++ implementation
# Executes the statement `WHITENING_SEQ = [`.
WHITENING_SEQ = [
    # Executes the statement `0xFF, 0xFE, 0xFC, 0xF8, 0xF0, 0xE1, 0xC2, 0x85, 0x0B, 0x17, 0x2F, 0x5E, 0xBC, 0x78, 0xF1, 0xE3,`.
    0xFF, 0xFE, 0xFC, 0xF8, 0xF0, 0xE1, 0xC2, 0x85, 0x0B, 0x17, 0x2F, 0x5E, 0xBC, 0x78, 0xF1, 0xE3,
    # Executes the statement `0xC6, 0x8D, 0x1A, 0x34, 0x68, 0xD0, 0xA0, 0x40, 0x80, 0x01, 0x02, 0x04, 0x08, 0x11, 0x23, 0x47,`.
    0xC6, 0x8D, 0x1A, 0x34, 0x68, 0xD0, 0xA0, 0x40, 0x80, 0x01, 0x02, 0x04, 0x08, 0x11, 0x23, 0x47,
    # Executes the statement `0x8E, 0x1C, 0x38, 0x71, 0xE2, 0xC4, 0x89, 0x12, 0x25, 0x4B, 0x97, 0x2E, 0x5C, 0xB8, 0x70, 0xE0,`.
    0x8E, 0x1C, 0x38, 0x71, 0xE2, 0xC4, 0x89, 0x12, 0x25, 0x4B, 0x97, 0x2E, 0x5C, 0xB8, 0x70, 0xE0,
    # Executes the statement `0xC0, 0x81, 0x03, 0x06, 0x0C, 0x19, 0x32, 0x64, 0xC9, 0x92, 0x24, 0x49, 0x93, 0x26, 0x4D, 0x9B,`.
    0xC0, 0x81, 0x03, 0x06, 0x0C, 0x19, 0x32, 0x64, 0xC9, 0x92, 0x24, 0x49, 0x93, 0x26, 0x4D, 0x9B,
    # Executes the statement `0x37, 0x6E, 0xDC, 0xB9, 0x72, 0xE4, 0xC8, 0x90, 0x20, 0x41, 0x82, 0x05, 0x0A, 0x15, 0x2B, 0x56,`.
    0x37, 0x6E, 0xDC, 0xB9, 0x72, 0xE4, 0xC8, 0x90, 0x20, 0x41, 0x82, 0x05, 0x0A, 0x15, 0x2B, 0x56,
    # Executes the statement `0xAD, 0x5B, 0xB6, 0x6D, 0xDA, 0xB5, 0x6B, 0xD6, 0xAC, 0x59, 0xB2, 0x65, 0xCB, 0x96, 0x2C, 0x58,`.
    0xAD, 0x5B, 0xB6, 0x6D, 0xDA, 0xB5, 0x6B, 0xD6, 0xAC, 0x59, 0xB2, 0x65, 0xCB, 0x96, 0x2C, 0x58,
    # Executes the statement `0xB0, 0x61, 0xC3, 0x87, 0x0F, 0x1F, 0x3E, 0x7D, 0xFB, 0xF6, 0xED, 0xDB, 0xB7, 0x6F, 0xDE, 0xBD,`.
    0xB0, 0x61, 0xC3, 0x87, 0x0F, 0x1F, 0x3E, 0x7D, 0xFB, 0xF6, 0xED, 0xDB, 0xB7, 0x6F, 0xDE, 0xBD,
    # Executes the statement `0x7A, 0xF5, 0xEB, 0xD7, 0xAE, 0x5D, 0xBA, 0x74, 0xE8, 0xD1, 0xA2, 0x44, 0x88, 0x10, 0x21, 0x43,`.
    0x7A, 0xF5, 0xEB, 0xD7, 0xAE, 0x5D, 0xBA, 0x74, 0xE8, 0xD1, 0xA2, 0x44, 0x88, 0x10, 0x21, 0x43,
    # Executes the statement `0x86, 0x0D, 0x1B, 0x36, 0x6C, 0xD8, 0xB1, 0x63, 0xC7, 0x8F, 0x1E, 0x3C, 0x79, 0xF3, 0xE7, 0xCE,`.
    0x86, 0x0D, 0x1B, 0x36, 0x6C, 0xD8, 0xB1, 0x63, 0xC7, 0x8F, 0x1E, 0x3C, 0x79, 0xF3, 0xE7, 0xCE,
    # Executes the statement `0x9C, 0x39, 0x73, 0xE6, 0xCC, 0x98, 0x31, 0x62, 0xC5, 0x8B, 0x16, 0x2D, 0x5A, 0xB4, 0x69, 0xD2,`.
    0x9C, 0x39, 0x73, 0xE6, 0xCC, 0x98, 0x31, 0x62, 0xC5, 0x8B, 0x16, 0x2D, 0x5A, 0xB4, 0x69, 0xD2,
    # Executes the statement `0xA4, 0x48, 0x91, 0x22, 0x45, 0x8A, 0x14, 0x29, 0x52, 0xA5, 0x4A, 0x95, 0x2A, 0x54, 0xA9, 0x53,`.
    0xA4, 0x48, 0x91, 0x22, 0x45, 0x8A, 0x14, 0x29, 0x52, 0xA5, 0x4A, 0x95, 0x2A, 0x54, 0xA9, 0x53,
    # Executes the statement `0xA7, 0x4E, 0x9D, 0x3B, 0x77, 0xEE, 0xDD, 0xBB, 0x76, 0xEC, 0xD9, 0xB3, 0x67, 0xCF, 0x9E, 0x3D,`.
    0xA7, 0x4E, 0x9D, 0x3B, 0x77, 0xEE, 0xDD, 0xBB, 0x76, 0xEC, 0xD9, 0xB3, 0x67, 0xCF, 0x9E, 0x3D,
    # Executes the statement `0x7B, 0xF7, 0xEF, 0xDF, 0xBF, 0x7E, 0xFD, 0xFA, 0xF4, 0xE9, 0xD3, 0xA6, 0x4C, 0x99, 0x33, 0x66,`.
    0x7B, 0xF7, 0xEF, 0xDF, 0xBF, 0x7E, 0xFD, 0xFA, 0xF4, 0xE9, 0xD3, 0xA6, 0x4C, 0x99, 0x33, 0x66,
    # Executes the statement `0xCD, 0x9A, 0x35, 0x6A, 0xD4, 0xA8, 0x51, 0xA3, 0x46, 0x8C, 0x18, 0x30, 0x60, 0xC1, 0x83, 0x07,`.
    0xCD, 0x9A, 0x35, 0x6A, 0xD4, 0xA8, 0x51, 0xA3, 0x46, 0x8C, 0x18, 0x30, 0x60, 0xC1, 0x83, 0x07,
    # Executes the statement `0x0E, 0x1D, 0x3A, 0x75, 0xEA, 0xD5, 0xAA, 0x55, 0xAB, 0x57, 0xAF, 0x5F, 0xBE, 0x7C, 0xF9, 0xF2,`.
    0x0E, 0x1D, 0x3A, 0x75, 0xEA, 0xD5, 0xAA, 0x55, 0xAB, 0x57, 0xAF, 0x5F, 0xBE, 0x7C, 0xF9, 0xF2,
    # Executes the statement `0xE5, 0xCA, 0x94, 0x28, 0x50, 0xA1, 0x42, 0x84, 0x09, 0x13, 0x27, 0x4F, 0x9F, 0x3F, 0x7F`.
    0xE5, 0xCA, 0x94, 0x28, 0x50, 0xA1, 0x42, 0x84, 0x09, 0x13, 0x27, 0x4F, 0x9F, 0x3F, 0x7F
# Closes the previously opened list indexing or literal.
]

# Defines the function gray_decode.
def gray_decode(g: int) -> int:
    """Gray decode as per C++ implementation"""
    # Executes the statement `n = g`.
    n = g
    # Starts a loop that continues while the condition holds.
    while g > 0:
        # Executes the statement `g >>= 1`.
        g >>= 1
        # Executes the statement `n ^= g`.
        n ^= g
    # Returns the computed value to the caller.
    return n

# Defines the function gray_encode.
def gray_encode(v: int) -> int:
    """Gray encode as per C++ implementation"""
    # Returns the computed value to the caller.
    return v ^ (v >> 1)

# Defines the function deinterleave_bits.
def deinterleave_bits(data: List[int], sf_app: int, cw_len: int) -> List[int]:
    """
    Deinterleave bits based on C++ implementation
    data: input bits (row-major layout)
    sf_app: number of rows (spreading factor)
    cw_len: number of columns (codeword length) 
    """
    # Executes the statement `out = [0] * len(data)`.
    out = [0] * len(data)

    # Starts a loop iterating over a sequence.
    for col in range(cw_len):
        # Starts a loop iterating over a sequence.
        for row in range(sf_app):
            # Executes the statement `dest_row = (col - row - 1) % sf_app`.
            dest_row = (col - row - 1) % sf_app
            # Begins a conditional branch to check a condition.
            if dest_row < 0:
                # Executes the statement `dest_row += sf_app`.
                dest_row += sf_app
            # Executes the statement `out[dest_row * cw_len + col] = data[col * sf_app + row]`.
            out[dest_row * cw_len + col] = data[col * sf_app + row]

    # Returns the computed value to the caller.
    return out

# Defines the function whiten_data.
def whiten_data(data: List[int]) -> List[int]:
    """Apply whitening/dewhitening (XOR with whitening sequence)"""
    # Executes the statement `result = []`.
    result = []
    # Starts a loop iterating over a sequence.
    for i, byte in enumerate(data):
        # Executes the statement `result.append(byte ^ WHITENING_SEQ[i % len(WHITENING_SEQ)])`.
        result.append(byte ^ WHITENING_SEQ[i % len(WHITENING_SEQ)])
    # Returns the computed value to the caller.
    return result

# Defines the function dewhiten_prefix.
def dewhiten_prefix(data: List[int], prefix_len: int) -> List[int]:
    """Dewhiten only the first prefix_len bytes using the fixed table sequence."""
    # Executes the statement `out = list(data)`.
    out = list(data)
    # Executes the statement `limit = min(prefix_len, len(out))`.
    limit = min(prefix_len, len(out))
    # Starts a loop iterating over a sequence.
    for i in range(limit):
        # Executes the statement `out[i] = out[i] ^ WHITENING_SEQ[i % len(WHITENING_SEQ)]`.
        out[i] = out[i] ^ WHITENING_SEQ[i % len(WHITENING_SEQ)]
    # Returns the computed value to the caller.
    return out

# Defines the function _pn9_step_lsb.
def _pn9_step_lsb(state: int) -> int:
    # x^9 + x^5 + 1, LSB-first; returns new state
    # Executes the statement `fb = ((state & 1) ^ ((state >> 5) & 1)) & 1`.
    fb = ((state & 1) ^ ((state >> 5) & 1)) & 1
    # Executes the statement `state = (state >> 1) | (fb << 8)`.
    state = (state >> 1) | (fb << 8)
    # Returns the computed value to the caller.
    return state & 0x1FF

# Defines the function _pn9_step_msb.
def _pn9_step_msb(state: int) -> int:
    # x^9 + x^5 + 1, MSB-first (feedback into LSB)
    # Executes the statement `msb = (state >> 8) & 1`.
    msb = (state >> 8) & 1
    # Executes the statement `bit5 = (state >> 4) & 1`.
    bit5 = (state >> 4) & 1
    # Executes the statement `fb = (msb ^ bit5) & 1`.
    fb = (msb ^ bit5) & 1
    # Executes the statement `state = ((state << 1) & 0x1FF) | fb`.
    state = ((state << 1) & 0x1FF) | fb
    # Returns the computed value to the caller.
    return state & 0x1FF

# Defines the function dewhiten_prefix_mode.
def dewhiten_prefix_mode(data: List[int], prefix_len: int, mode: str = 'table', seed: int = 0x1FF) -> List[int]:
    # Executes the statement `out = list(data)`.
    out = list(data)
    # Executes the statement `L = min(prefix_len, len(out))`.
    L = min(prefix_len, len(out))
    # Begins a conditional branch to check a condition.
    if mode == 'none':
        # Returns the computed value to the caller.
        return out[:L] + out[L:]
    # Begins a conditional branch to check a condition.
    if mode == 'table':
        # Starts a loop iterating over a sequence.
        for i in range(L):
            # Executes the statement `out[i] ^= WHITENING_SEQ[i % len(WHITENING_SEQ)]`.
            out[i] ^= WHITENING_SEQ[i % len(WHITENING_SEQ)]
        # Returns the computed value to the caller.
        return out
    # Begins a conditional branch to check a condition.
    if mode in ('pn9', 'pn9_msb'):
        # Executes the statement `s = seed & 0x1FF`.
        s = seed & 0x1FF
        # Starts a loop iterating over a sequence.
        for i in range(L):
            # Executes the statement `byte = 0`.
            byte = 0
            # Begins a conditional branch to check a condition.
            if mode == 'pn9':
                # LSB-first PN9
                # Starts a loop iterating over a sequence.
                for b in range(8):
                    # Executes the statement `bit = s & 1`.
                    bit = s & 1
                    # Executes the statement `byte |= (bit << b)`.
                    byte |= (bit << b)
                    # Executes the statement `s = _pn9_step_lsb(s)`.
                    s = _pn9_step_lsb(s)
            # Provides the fallback branch when previous conditions fail.
            else:
                # MSB-first PN9 (fill MSB...LSB)
                # Starts a loop iterating over a sequence.
                for b in range(7, -1, -1):
                    # Executes the statement `bit = (s >> 8) & 1`.
                    bit = (s >> 8) & 1
                    # Executes the statement `byte |= (bit << b)`.
                    byte |= (bit << b)
                    # Executes the statement `s = _pn9_step_msb(s)`.
                    s = _pn9_step_msb(s)
            # Executes the statement `out[i] ^= (byte & 0xFF)`.
            out[i] ^= (byte & 0xFF)
        # Returns the computed value to the caller.
        return out
    # Default: no change
    # Returns the computed value to the caller.
    return out

# Defines the function symbols_to_bits.
def symbols_to_bits(symbols: List[int], sf: int) -> List[int]:
    """Convert LoRa symbols to bits"""
    # Executes the statement `bits = []`.
    bits = []
    # Starts a loop iterating over a sequence.
    for symbol in symbols:
        # Gray decode the symbol first
        # Executes the statement `decoded_symbol = gray_decode(symbol)`.
        decoded_symbol = gray_decode(symbol)

        # Extract SF bits from symbol (LSB first for LoRa)
        # Starts a loop iterating over a sequence.
        for bit_pos in range(sf):
            # Executes the statement `bits.append((decoded_symbol >> bit_pos) & 1)`.
            bits.append((decoded_symbol >> bit_pos) & 1)

    # Returns the computed value to the caller.
    return bits

# Defines the function bits_to_bytes.
def bits_to_bytes(bits: List[int]) -> List[int]:
    """Convert bits to bytes (8 bits per byte, LSB first)"""
    # Executes the statement `bytes_list = []`.
    bytes_list = []

    # Process bits in groups of 8
    # Starts a loop iterating over a sequence.
    for i in range(0, len(bits), 8):
        # Begins a conditional branch to check a condition.
        if i + 8 <= len(bits):
            # Executes the statement `byte_val = 0`.
            byte_val = 0
            # Starts a loop iterating over a sequence.
            for j in range(8):
                # Executes the statement `byte_val |= (bits[i + j] << j)`.
                byte_val |= (bits[i + j] << j)
            # Executes the statement `bytes_list.append(byte_val)`.
            bytes_list.append(byte_val)

    # Returns the computed value to the caller.
    return bytes_list

# Defines the function rev_bits8.
def rev_bits8(x: int) -> int:
    """Reverse 8 bits."""
    # Executes the statement `x &= 0xFF`.
    x &= 0xFF
    # Executes the statement `x = ((x & 0xF0) >> 4) | ((x & 0x0F) << 4)`.
    x = ((x & 0xF0) >> 4) | ((x & 0x0F) << 4)
    # Executes the statement `x = ((x & 0xCC) >> 2) | ((x & 0x33) << 2)`.
    x = ((x & 0xCC) >> 2) | ((x & 0x33) << 2)
    # Executes the statement `x = ((x & 0xAA) >> 1) | ((x & 0x55) << 1)`.
    x = ((x & 0xAA) >> 1) | ((x & 0x55) << 1)
    # Returns the computed value to the caller.
    return x

# Defines the function rev4.
def rev4(x: int) -> int:
    """Reverse 4-bit nibble."""
    # Executes the statement `x &= 0xF`.
    x &= 0xF
    # Executes the statement `x = ((x & 0xC) >> 2) | ((x & 0x3) << 2)`.
    x = ((x & 0xC) >> 2) | ((x & 0x3) << 2)
    # Executes the statement `x = ((x & 0xA) >> 1) | ((x & 0x5) << 1)`.
    x = ((x & 0xA) >> 1) | ((x & 0x5) << 1)
    # Returns the computed value to the caller.
    return x

# Defines the function rev_bits_k.
def rev_bits_k(x: int, k: int) -> int:
    """Reverse the lowest k bits of x."""
    # Executes the statement `out = 0`.
    out = 0
    # Starts a loop iterating over a sequence.
    for i in range(k):
        # Executes the statement `out = (out << 1) | ((x >> i) & 1)`.
        out = (out << 1) | ((x >> i) & 1)
    # Returns the computed value to the caller.
    return out & ((1 << k) - 1)

# Defines the function pack_bits_custom.
def pack_bits_custom(bits: List[int], bit_offset: int, msb_first: bool) -> List[int]:
    """Pack bits to bytes from an arbitrary bit offset, with MSB/LSB-first option."""
    # Begins a conditional branch to check a condition.
    if bit_offset < 0:
        # Executes the statement `bit_offset = 0`.
        bit_offset = 0
    # Begins a conditional branch to check a condition.
    if bit_offset >= len(bits):
        # Returns the computed value to the caller.
        return []
    # Executes the statement `nbits = len(bits) - bit_offset`.
    nbits = len(bits) - bit_offset
    # Executes the statement `nbytes = (nbits + 7) // 8`.
    nbytes = (nbits + 7) // 8
    # Executes the statement `out = [0] * nbytes`.
    out = [0] * nbytes
    # Begins a conditional branch to check a condition.
    if not msb_first:
        # Starts a loop iterating over a sequence.
        for i in range(nbits):
            # Begins a conditional branch to check a condition.
            if bits[bit_offset + i] & 1:
                # Executes the statement `out[i >> 3] |= (1 << (i & 7))`.
                out[i >> 3] |= (1 << (i & 7))
    # Provides the fallback branch when previous conditions fail.
    else:
        # Starts a loop iterating over a sequence.
        for i in range(nbits):
            # Begins a conditional branch to check a condition.
            if bits[bit_offset + i] & 1:
                # Executes the statement `out[i >> 3] |= (1 << (7 - (i & 7)))`.
                out[i >> 3] |= (1 << (7 - (i & 7)))
    # Returns the computed value to the caller.
    return out

# Defines the function pack_nibbles_to_bytes.
def pack_nibbles_to_bytes(nibbles: List[int], start: int, high_first: bool) -> List[int]:
    """Pack 4-bit nibbles into bytes starting at nibble index, order selectable."""
    # Begins a conditional branch to check a condition.
    if start < 0:
        # Executes the statement `start = 0`.
        start = 0
    # Begins a conditional branch to check a condition.
    if start >= len(nibbles):
        # Returns the computed value to the caller.
        return []
    # Executes the statement `pairs = (len(nibbles) - start) // 2`.
    pairs = (len(nibbles) - start) // 2
    # Executes the statement `out = []`.
    out = []
    # Starts a loop iterating over a sequence.
    for i in range(pairs):
        # Executes the statement `n0 = nibbles[start + 2*i] & 0xF`.
        n0 = nibbles[start + 2*i] & 0xF
        # Executes the statement `n1 = nibbles[start + 2*i + 1] & 0xF`.
        n1 = nibbles[start + 2*i + 1] & 0xF
        # Begins a conditional branch to check a condition.
        if high_first:
            # Executes the statement `out.append(((n0 << 4) | n1) & 0xFF)`.
            out.append(((n0 << 4) | n1) & 0xFF)
        # Provides the fallback branch when previous conditions fail.
        else:
            # Executes the statement `out.append(((n1 << 4) | n0) & 0xFF)`.
            out.append(((n1 << 4) | n0) & 0xFF)
    # Returns the computed value to the caller.
    return out

# Defines the function decode_lora_payload.
def decode_lora_payload(symbols: List[int], sf: int, cr: int, sf_app: int | None = None, *, quick: bool = False, verbose: bool = True, max_L: int | None = None, exact_L: int | None = None) -> List[int]:
    """
    Complete LoRa payload decoding chain following C++ implementation exactly

    Args:
        symbols: Raw LoRa symbols (after header)
        sf: Spreading factor
        cr: Coding rate
        sf_app: Effective rows used for interleaver (SF minus LDRO). If None, uses sf.

    Returns:
        Decoded payload bytes
    """
    # Begins a conditional branch to check a condition.
    if not symbols:
        # Returns the computed value to the caller.
        return []

    # Begins a conditional branch to check a condition.
    if verbose:
        # Outputs diagnostic or user-facing text.
        print(f"🔧 Decoding {len(symbols)} symbols with SF={sf}, CR={cr}")

    # Begins a block that monitors for exceptions.
    try:
        # Process symbols in codeword blocks as per C++ implementation
        # Executes the statement `sf_app = int(sf if sf_app is None else max(1, min(int(sf_app), int(sf))))`.
        sf_app = int(sf if sf_app is None else max(1, min(int(sf_app), int(sf))))
        # Executes the statement `cw_len = 4 + cr  # Codeword length`.
        cw_len = 4 + cr  # Codeword length

        # We'll search mapping profiles over column direction, bit reversal, nibble reversal, and column phase
        # to mirror the C++ variant sweep. First build the shared deinterleaver input (in_bits) per codeword.
        # Executes the statement `base_blocks_inbits_ord0 = []  # row = sf_app-1-b`.
        base_blocks_inbits_ord0 = []  # row = sf_app-1-b
        # Executes the statement `base_blocks_inbits_ord1 = []  # row = b`.
        base_blocks_inbits_ord1 = []  # row = b

        # Process symbols in codeword blocks to construct in_bits for each block
        # Executes the statement `sym_index = 0`.
        sym_index = 0
        # Starts a loop that continues while the condition holds.
        while sym_index + cw_len <= len(symbols):
            # Executes the statement `cw_symbols = symbols[sym_index:sym_index + cw_len]`.
            cw_symbols = symbols[sym_index:sym_index + cw_len]
            # Executes the statement `in_bits0 = [0] * (sf_app * cw_len)`.
            in_bits0 = [0] * (sf_app * cw_len)
            # Executes the statement `in_bits1 = [0] * (sf_app * cw_len)`.
            in_bits1 = [0] * (sf_app * cw_len)
            # Starts a loop iterating over a sequence.
            for col, symbol in enumerate(cw_symbols):
                # Executes the statement `nb = gray_decode(symbol)`.
                nb = gray_decode(symbol)
                # Begins a conditional branch to check a condition.
                if sf_app < sf:
                    # Executes the statement `nb >>= (sf - sf_app)`.
                    nb >>= (sf - sf_app)
                # Starts a loop iterating over a sequence.
                for b in range(sf_app):
                    # Executes the statement `bit = (nb >> b) & 1`.
                    bit = (nb >> b) & 1
                    # Executes the statement `row0 = sf_app - 1 - b`.
                    row0 = sf_app - 1 - b
                    # Executes the statement `row1 = b`.
                    row1 = b
                    # Executes the statement `in_bits0[col * sf_app + row0] = bit`.
                    in_bits0[col * sf_app + row0] = bit
                    # Executes the statement `in_bits1[col * sf_app + row1] = bit`.
                    in_bits1[col * sf_app + row1] = bit
            # Executes the statement `base_blocks_inbits_ord0.append(in_bits0)`.
            base_blocks_inbits_ord0.append(in_bits0)
            # Executes the statement `base_blocks_inbits_ord1.append(in_bits1)`.
            base_blocks_inbits_ord1.append(in_bits1)
            # Executes the statement `sym_index += cw_len`.
            sym_index += cw_len

        # Define deinterleave function (legacy variant per C++)
        # Defines the function deint_legacy.
        def deint_legacy(in_bits: List[int]) -> List[int]:
            # Executes the statement `out_bits = [0] * len(in_bits)`.
            out_bits = [0] * len(in_bits)
            # Starts a loop iterating over a sequence.
            for col in range(cw_len):
                # Starts a loop iterating over a sequence.
                for row in range(sf_app):
                    # Executes the statement `dest_row = (col - row - 1) % sf_app`.
                    dest_row = (col - row - 1) % sf_app
                    # Begins a conditional branch to check a condition.
                    if dest_row < 0:
                        # Executes the statement `dest_row += sf_app`.
                        dest_row += sf_app
                    # Executes the statement `out_bits[dest_row * cw_len + col] = in_bits[col * sf_app + row]`.
                    out_bits[dest_row * cw_len + col] = in_bits[col * sf_app + row]
            # Returns the computed value to the caller.
            return out_bits

        # Defines the function deint_lora.
        def deint_lora(in_bits: List[int]) -> List[int]:
            # Alternative deinterleaver consistent with some LoRa implementations
            # Executes the statement `out_bits = [0] * len(in_bits)`.
            out_bits = [0] * len(in_bits)
            # Starts a loop iterating over a sequence.
            for col in range(cw_len):
                # Starts a loop iterating over a sequence.
                for row in range(sf_app):
                    # Executes the statement `dest_row = (row + col) % sf_app`.
                    dest_row = (row + col) % sf_app
                    # Executes the statement `out_bits[dest_row * cw_len + col] = in_bits[col * sf_app + row]`.
                    out_bits[dest_row * cw_len + col] = in_bits[col * sf_app + row]
            # Returns the computed value to the caller.
            return out_bits

        # Profile sweep: col_rev, bit_rev, nib_rev, col_phase
        # Executes the statement `best_L = 0`.
        best_L = 0
        # Executes the statement `best_payload: List[int] = []`.
        best_payload: List[int] = []
        # Executes the statement `best_dbg = None`.
        best_dbg = None
        # Executes the statement `col_rev_opts = (0, 1)`.
        col_rev_opts = (0, 1)
        # Executes the statement `bit_rev_opts = (0, 1)`.
        bit_rev_opts = (0, 1)
        # Executes the statement `nib_rev_opts = (0, 1)`.
        nib_rev_opts = (0, 1)
        # Executes the statement `col_phase_range = range(0, cw_len if not quick else min(cw_len, 4))`.
        col_phase_range = range(0, cw_len if not quick else min(cw_len, 4))
        # Executes the statement `which_orders = (0, 1) if not quick else (0,)  # limit to first order in quick mode`.
        which_orders = (0, 1) if not quick else (0,)  # limit to first order in quick mode
        # Executes the statement `deint_modes = (0, 1) if not quick else (0,)   # limit to legacy deinterleaver in quick mode`.
        deint_modes = (0, 1) if not quick else (0,)   # limit to legacy deinterleaver in quick mode
        # Starts a loop iterating over a sequence.
        for col_rev in col_rev_opts:
            # Starts a loop iterating over a sequence.
            for bit_rev in bit_rev_opts:
                # Starts a loop iterating over a sequence.
                for nib_rev in nib_rev_opts:
                    # Starts a loop iterating over a sequence.
                    for col_phase in col_phase_range:
                        # Build out_bits for all blocks and then decode Hamming into bits/nibbles
                        # Executes the statement `all_data_bits = []`.
                        all_data_bits = []
                        # Executes the statement `all_nibbles = []`.
                        all_nibbles = []
                        # Try row-order and deinterleaver variants (limited in quick mode)
                        # Starts a loop iterating over a sequence.
                        for which_order in which_orders:
                            # Executes the statement `in_list = base_blocks_inbits_ord0 if which_order == 0 else base_blocks_inbits_ord1`.
                            in_list = base_blocks_inbits_ord0 if which_order == 0 else base_blocks_inbits_ord1
                            # Starts a loop iterating over a sequence.
                            for in_bits in in_list:
                                # Starts a loop iterating over a sequence.
                                for deint_mode in deint_modes:
                                    # Executes the statement `out_bits = deint_legacy(in_bits) if deint_mode == 0 else deint_lora(in_bits)`.
                                    out_bits = deint_legacy(in_bits) if deint_mode == 0 else deint_lora(in_bits)
                                    # Row-wise pack with column direction and phase
                                    # Starts a loop iterating over a sequence.
                                    for row in range(sf_app):
                                        # Executes the statement `code_k = 0`.
                                        code_k = 0
                                        # Starts a loop iterating over a sequence.
                                        for col in range(cw_len):
                                            # Executes the statement `cc0 = (cw_len - 1 - col) if col_rev else col`.
                                            cc0 = (cw_len - 1 - col) if col_rev else col
                                            # Executes the statement `cc = (cc0 + col_phase) % cw_len`.
                                            cc = (cc0 + col_phase) % cw_len
                                            # Executes the statement `code_k = (code_k << 1) | (out_bits[row * cw_len + cc] & 1)`.
                                            code_k = (code_k << 1) | (out_bits[row * cw_len + cc] & 1)
                                        # Executes the statement `ck = rev_bits_k(code_k, cw_len) if bit_rev else code_k`.
                                        ck = rev_bits_k(code_k, cw_len) if bit_rev else code_k
                                        # Executes the statement `decoded_nib = hamming_decode4(ck, cr) & 0xF`.
                                        decoded_nib = hamming_decode4(ck, cr) & 0xF
                                        # Begins a conditional branch to check a condition.
                                        if nib_rev:
                                            # Executes the statement `decoded_nib = rev4(decoded_nib)`.
                                            decoded_nib = rev4(decoded_nib)
                                        # Executes the statement `all_nibbles.append(decoded_nib)`.
                                        all_nibbles.append(decoded_nib)
                                        # Starts a loop iterating over a sequence.
                                        for b in range(4):
                                            # Executes the statement `all_data_bits.append((decoded_nib >> b) & 1)`.
                                            all_data_bits.append((decoded_nib >> b) & 1)

                        # With these bits/nibbles, run packing+CRC scan for this variant only
                        # Executes the statement `packed_variants: List[List[int]] = []`.
                        packed_variants: List[List[int]] = []
                        # Executes the statement `off_bits_range = range(0, 8 if not quick else 2)`.
                        off_bits_range = range(0, 8 if not quick else 2)
                        # Executes the statement `bit_slide_range = range(0, 8 if not quick else 3)`.
                        bit_slide_range = range(0, 8 if not quick else 3)
                        # Executes the statement `start_byte_limit = 8 if not quick else 3`.
                        start_byte_limit = 8 if not quick else 3
                        # Starts a loop iterating over a sequence.
                        for off_bits in off_bits_range:
                            # Executes the statement `packed_variants.append(pack_bits_custom(all_data_bits, off_bits, msb_first=False))`.
                            packed_variants.append(pack_bits_custom(all_data_bits, off_bits, msb_first=False))
                            # Begins a conditional branch to check a condition.
                            if not quick:
                                # Executes the statement `packed_variants.append(pack_bits_custom(all_data_bits, off_bits, msb_first=True))`.
                                packed_variants.append(pack_bits_custom(all_data_bits, off_bits, msb_first=True))
                        # Starts a loop iterating over a sequence.
                        for nib_start in (0, 1):
                            # Executes the statement `packed_variants.append(pack_nibbles_to_bytes(all_nibbles, nib_start, high_first=True))`.
                            packed_variants.append(pack_nibbles_to_bytes(all_nibbles, nib_start, high_first=True))
                            # Begins a conditional branch to check a condition.
                            if not quick:
                                # Executes the statement `packed_variants.append(pack_nibbles_to_bytes(all_nibbles, nib_start, high_first=False))`.
                                packed_variants.append(pack_nibbles_to_bytes(all_nibbles, nib_start, high_first=False))

                        # Starts a loop iterating over a sequence.
                        for packed_try_full in packed_variants:
                            # Begins a conditional branch to check a condition.
                            if len(packed_try_full) < 3:
                                # Skips to the next iteration of the loop.
                                continue
                            # Starts a loop iterating over a sequence.
                            for bit_slide in bit_slide_range:
                                # Begins a conditional branch to check a condition.
                                if bit_slide == 0:
                                    # Executes the statement `bit_slid = list(packed_try_full)`.
                                    bit_slid = list(packed_try_full)
                                # Provides the fallback branch when previous conditions fail.
                                else:
                                    # Executes the statement `bit_slid = []`.
                                    bit_slid = []
                                    # Executes the statement `carry = 0`.
                                    carry = 0
                                    # Starts a loop iterating over a sequence.
                                    for b in packed_try_full:
                                        # Executes the statement `val = ((b & 0xFF) << bit_slide) | carry`.
                                        val = ((b & 0xFF) << bit_slide) | carry
                                        # Executes the statement `bit_slid.append(val & 0xFF)`.
                                        bit_slid.append(val & 0xFF)
                                        # Executes the statement `carry = (val >> 8) & 0xFF`.
                                        carry = (val >> 8) & 0xFF
                                    # Begins a conditional branch to check a condition.
                                    if carry:
                                        # Executes the statement `bit_slid.append(carry)`.
                                        bit_slid.append(carry)
                                # Begins a conditional branch to check a condition.
                                if len(bit_slid) < 3:
                                    # Skips to the next iteration of the loop.
                                    continue
                                # Executes the statement `slide_limit = min(start_byte_limit, len(bit_slid))`.
                                slide_limit = min(start_byte_limit, len(bit_slid))
                                # Executes the statement `candidate_base = bit_slid`.
                                candidate_base = bit_slid
                                # Whitening configurations to try
                                # Begins a conditional branch to check a condition.
                                if quick:
                                    # Executes the statement `whiten_modes = [('table', 0x000), ('pn9', 0x1A5), ('pn9', 0x100), ('pn9', 0x101)]`.
                                    whiten_modes = [('table', 0x000), ('pn9', 0x1A5), ('pn9', 0x100), ('pn9', 0x101)]
                                # Provides the fallback branch when previous conditions fail.
                                else:
                                    # Executes the statement `whiten_modes = [('table', 0x000), ('pn9', 0x1FF), ('pn9', 0x101), ('pn9', 0x1A5), ('pn9', 0x100), ('pn9', 0x17F), ('pn9_msb', 0x1FF)]`.
                                    whiten_modes = [('table', 0x000), ('pn9', 0x1FF), ('pn9', 0x101), ('pn9', 0x1A5), ('pn9', 0x100), ('pn9', 0x17F), ('pn9_msb', 0x1FF)]
                                # Starts a loop iterating over a sequence.
                                for start_byte in range(0, slide_limit):
                                    # Executes the statement `candidate = candidate_base if start_byte == 0 else candidate_base[start_byte:]`.
                                    candidate = candidate_base if start_byte == 0 else candidate_base[start_byte:]
                                    # Begins a conditional branch to check a condition.
                                    if len(candidate) < 3:
                                        # Exits the nearest enclosing loop early.
                                        break
                                    # Executes the statement `L_cap = len(candidate) - 2`.
                                    L_cap = len(candidate) - 2
                                    # Executes the statement `max_scan = min(L_cap, 64 if not quick else 16)`.
                                    max_scan = min(L_cap, 64 if not quick else 16)
                                    # Begins a conditional branch to check a condition.
                                    if isinstance(max_L, int) and max_L > 0:
                                        # Executes the statement `max_scan = min(max_scan, max_L)`.
                                        max_scan = min(max_scan, max_L)
                                    # Starts a loop iterating over a sequence.
                                    for L in range(1, max_scan + 1):
                                        # Begins a conditional branch to check a condition.
                                        if isinstance(exact_L, int) and exact_L > 0 and L != exact_L:
                                            # Enforce exact target payload length when requested
                                            # Skips to the next iteration of the loop.
                                            continue
                                        # Starts a loop iterating over a sequence.
                                        for (wmode, wseed) in whiten_modes:
                                            # Executes the statement `dewhite = dewhiten_prefix_mode(candidate, L, mode=wmode, seed=wseed)`.
                                            dewhite = dewhiten_prefix_mode(candidate, L, mode=wmode, seed=wseed)
                                            # Executes the statement `payload_bytes = dewhite[:L]`.
                                            payload_bytes = dewhite[:L]
                                            # Executes the statement `crc = crc16_lora(payload_bytes)`.
                                            crc = crc16_lora(payload_bytes)
                                            # Executes the statement `crc_lsb = crc & 0xFF`.
                                            crc_lsb = crc & 0xFF
                                            # Executes the statement `crc_msb = (crc >> 8) & 0xFF`.
                                            crc_msb = (crc >> 8) & 0xFF
                                            # Executes the statement `obs_lsb = candidate[L] if L < len(candidate) else 0`.
                                            obs_lsb = candidate[L] if L < len(candidate) else 0
                                            # Executes the statement `obs_msb = candidate[L+1] if (L+1) < len(candidate) else 0`.
                                            obs_msb = candidate[L+1] if (L+1) < len(candidate) else 0
                                            # Begins a conditional branch to check a condition.
                                            if crc_lsb == obs_lsb and crc_msb == obs_msb:
                                                # Begins a conditional branch to check a condition.
                                                if L >= 8:
                                                    # Begins a conditional branch to check a condition.
                                                    if verbose:
                                                        # Outputs diagnostic or user-facing text.
                                                        print(f"   CRC matched L={L} wmode={wmode} seed=0x{wseed:X} col_rev={col_rev} bit_rev={bit_rev} nib_rev={nib_rev} col_phase={col_phase} start_byte={start_byte} bit_slide={bit_slide}")
                                                    # Returns the computed value to the caller.
                                                    return payload_bytes
                                                # Begins a conditional branch to check a condition.
                                                if L > best_L:
                                                    # Executes the statement `best_L = L`.
                                                    best_L = L
                                                    # Executes the statement `best_payload = payload_bytes`.
                                                    best_payload = payload_bytes
                                                    # Executes the statement `best_dbg = (col_rev, bit_rev, nib_rev, col_phase, start_byte, bit_slide)`.
                                                    best_dbg = (col_rev, bit_rev, nib_rev, col_phase, start_byte, bit_slide)
        # Fallback: build bits using order-0 and legacy deinterleaver, no transforms
        # Executes the statement `simple_data_bits: List[int] = []`.
        simple_data_bits: List[int] = []
        # Starts a loop iterating over a sequence.
        for in_bits in base_blocks_inbits_ord0:
            # Executes the statement `out_bits = deint_legacy(in_bits)`.
            out_bits = deint_legacy(in_bits)
            # Starts a loop iterating over a sequence.
            for row in range(sf_app):
                # Executes the statement `code_k = 0`.
                code_k = 0
                # Starts a loop iterating over a sequence.
                for col in range(cw_len):
                    # Executes the statement `code_k = (code_k << 1) | (out_bits[row * cw_len + col] & 1)`.
                    code_k = (code_k << 1) | (out_bits[row * cw_len + col] & 1)
                # Executes the statement `decoded_nib = hamming_decode4(code_k, cr) & 0xF`.
                decoded_nib = hamming_decode4(code_k, cr) & 0xF
                # Starts a loop iterating over a sequence.
                for b in range(4):
                    # Executes the statement `simple_data_bits.append((decoded_nib >> b) & 1)`.
                    simple_data_bits.append((decoded_nib >> b) & 1)
        # Executes the statement `packed_bytes = pack_bits_lsb_first(simple_data_bits)`.
        packed_bytes = pack_bits_lsb_first(simple_data_bits)
        # Begins a conditional branch to check a condition.
        if verbose:
            # Outputs diagnostic or user-facing text.
            print(f"   Fallback packed to {len(packed_bytes)} bytes")
        # Executes the statement `payload_len = 11  # expected for demo vector`.
        payload_len = 11  # expected for demo vector
        # Begins a conditional branch to check a condition.
        if len(packed_bytes) >= payload_len:
            # Executes the statement `dewhitened_bytes = whiten_data(packed_bytes[:payload_len])`.
            dewhitened_bytes = whiten_data(packed_bytes[:payload_len])
            # Begins a conditional branch to check a condition.
            if verbose:
                # Outputs diagnostic or user-facing text.
                print(f"   Fallback dewhitened {len(dewhitened_bytes)} bytes")
            # Returns the computed value to the caller.
            return dewhitened_bytes
        # Provides the fallback branch when previous conditions fail.
        else:
            # Executes the statement `dewhitened_bytes = whiten_data(packed_bytes)`.
            dewhitened_bytes = whiten_data(packed_bytes)
            # Begins a conditional branch to check a condition.
            if verbose:
                # Outputs diagnostic or user-facing text.
                print(f"   Fallback dewhitened {len(dewhitened_bytes)} bytes (insufficient)")
            # Returns the computed value to the caller.
            return dewhitened_bytes

    # Handles a specific exception from the try block.
    except Exception as e:
        # Begins a conditional branch to check a condition.
        if verbose:
            # Outputs diagnostic or user-facing text.
            print(f"   Error in LoRa decoding: {e}")
        # Returns the computed value to the caller.
        return []

# Defines the function pack_bits_lsb_first.
def pack_bits_lsb_first(bits: List[int]) -> List[int]:
    """
    Pack bits into bytes, LSB-first within each byte (matches C++ implementation)
    """
    # Executes the statement `bytes_list = []`.
    bytes_list = []
    # Executes the statement `cur = 0`.
    cur = 0
    # Executes the statement `sh = 0`.
    sh = 0

    # Starts a loop iterating over a sequence.
    for bit in bits:
        # Executes the statement `cur |= (bit & 1) << sh`.
        cur |= (bit & 1) << sh
        # Executes the statement `sh += 1`.
        sh += 1
        # Begins a conditional branch to check a condition.
        if sh == 8:
            # Executes the statement `bytes_list.append(cur)`.
            bytes_list.append(cur)
            # Executes the statement `cur = 0`.
            cur = 0
            # Executes the statement `sh = 0`.
            sh = 0

    # Begins a conditional branch to check a condition.
    if sh > 0:
        # Executes the statement `bytes_list.append(cur)`.
        bytes_list.append(cur)

    # Returns the computed value to the caller.
    return bytes_list

# Defines the function hamming_decode4.
def hamming_decode4(code: int, cr: int) -> int:
    """
    LoRa Hamming decoder - converts codeword to 4-bit data nibble
    Based on C++ implementation in hamming.cpp
    """
    # Build lookup tables for different coding rates
    # Executes the statement `enc_tables = {}`.
    enc_tables = {}

    # Build encoding table for CR=2 (4/6 coding rate)
    # Begins a conditional branch to check a condition.
    if cr == 2:
        # Executes the statement `enc6 = {}`.
        enc6 = {}
        # Starts a loop iterating over a sequence.
        for n in range(16):
            # Executes the statement `d3, d2, d1, d0 = (n>>3)&1, (n>>2)&1, (n>>1)&1, n&1`.
            d3, d2, d1, d0 = (n>>3)&1, (n>>2)&1, (n>>1)&1, n&1
            # Executes the statement `p0 = d3^d2^d1`.
            p0 = d3^d2^d1
            # Executes the statement `p1 = d2^d1^d0`.
            p1 = d2^d1^d0
            # Executes the statement `p2 = d3^d2^d0`.
            p2 = d3^d2^d0  
            # Executes the statement `p3 = d3^d1^d0`.
            p3 = d3^d1^d0
            # Executes the statement `full8 = (d3<<7)|(d2<<6)|(d1<<5)|(d0<<4)|(p0<<3)|(p1<<2)|(p2<<1)|p3`.
            full8 = (d3<<7)|(d2<<6)|(d1<<5)|(d0<<4)|(p0<<3)|(p1<<2)|(p2<<1)|p3
            # Executes the statement `enc6[n] = full8 >> 2  # CR=2 uses 6 bits`.
            enc6[n] = full8 >> 2  # CR=2 uses 6 bits

        # Try direct lookup first
        # Starts a loop iterating over a sequence.
        for n in range(16):
            # Begins a conditional branch to check a condition.
            if enc6[n] == (code & 0x3F):  # 6-bit mask
                # Returns the computed value to the caller.
                return n

        # If no direct match, find closest (error correction)
        # Executes the statement `best_dist = 10`.
        best_dist = 10
        # Executes the statement `best_n = -1`.
        best_n = -1
        # Starts a loop iterating over a sequence.
        for n in range(16):
            # Executes the statement `dist = bin(enc6[n] ^ (code & 0x3F)).count('1')  # Hamming distance`.
            dist = bin(enc6[n] ^ (code & 0x3F)).count('1')  # Hamming distance
            # Begins a conditional branch to check a condition.
            if dist < best_dist:
                # Executes the statement `best_dist = dist`.
                best_dist = dist
                # Executes the statement `best_n = n`.
                best_n = n

        # Begins a conditional branch to check a condition.
        if best_dist <= 2 and best_n >= 0:  # Allow up to 2-bit errors
            # Returns the computed value to the caller.
            return best_n
    # Build encoding table for CR=4 (4/8 coding rate) used by LoRa header
    # Begins a conditional branch to check a condition.
    if cr == 4:
        # Executes the statement `enc8 = {}`.
        enc8 = {}
        # Starts a loop iterating over a sequence.
        for n in range(16):
            # Executes the statement `d3, d2, d1, d0 = (n>>3)&1, (n>>2)&1, (n>>1)&1, n&1`.
            d3, d2, d1, d0 = (n>>3)&1, (n>>2)&1, (n>>1)&1, n&1
            # Same parity relations as above but keep full 8 bits
            # Executes the statement `p0 = d3^d2^d1`.
            p0 = d3^d2^d1
            # Executes the statement `p1 = d2^d1^d0`.
            p1 = d2^d1^d0
            # Executes the statement `p2 = d3^d2^d0`.
            p2 = d3^d2^d0
            # Executes the statement `p3 = d3^d1^d0`.
            p3 = d3^d1^d0
            # Executes the statement `full8 = (d3<<7)|(d2<<6)|(d1<<5)|(d0<<4)|(p0<<3)|(p1<<2)|(p2<<1)|p3`.
            full8 = (d3<<7)|(d2<<6)|(d1<<5)|(d0<<4)|(p0<<3)|(p1<<2)|(p2<<1)|p3
            # Executes the statement `enc8[n] = full8 & 0xFF`.
            enc8[n] = full8 & 0xFF
        # Direct match
        # Executes the statement `code8 = code & 0xFF`.
        code8 = code & 0xFF
        # Starts a loop iterating over a sequence.
        for n in range(16):
            # Begins a conditional branch to check a condition.
            if enc8[n] == code8:
                # Returns the computed value to the caller.
                return n
        # Nearest by Hamming distance (up to 2-bit errors)
        # Executes the statement `best_dist = 10`.
        best_dist = 10
        # Executes the statement `best_n = -1`.
        best_n = -1
        # Starts a loop iterating over a sequence.
        for n in range(16):
            # Executes the statement `dist = bin(enc8[n] ^ code8).count('1')`.
            dist = bin(enc8[n] ^ code8).count('1')
            # Begins a conditional branch to check a condition.
            if dist < best_dist:
                # Executes the statement `best_dist = dist`.
                best_dist = dist
                # Executes the statement `best_n = n`.
                best_n = n
        # Begins a conditional branch to check a condition.
        if best_dist <= 2 and best_n >= 0:
            # Returns the computed value to the caller.
            return best_n

    # Fallback: extract upper 4 bits as simple approximation
    # Returns the computed value to the caller.
    return (code >> 2) & 0xF

# Defines the function crc16_lora.
def crc16_lora(data: List[int]) -> int:
    """
    Simple CRC16 calculation for LoRa (polynomial 0x1021)
    """
    # Executes the statement `crc = 0x0000`.
    crc = 0x0000
    # Starts a loop iterating over a sequence.
    for byte in data:
        # Executes the statement `crc ^= (byte << 8)`.
        crc ^= (byte << 8)
        # Starts a loop iterating over a sequence.
        for _ in range(8):
            # Begins a conditional branch to check a condition.
            if crc & 0x8000:
                # Executes the statement `crc = (crc << 1) ^ 0x1021`.
                crc = (crc << 1) ^ 0x1021
            # Provides the fallback branch when previous conditions fail.
            else:
                # Executes the statement `crc <<= 1`.
                crc <<= 1
            # Executes the statement `crc &= 0xFFFF`.
            crc &= 0xFFFF
    # Returns the computed value to the caller.
    return crc


# Defines the function compute_header_crc.
def compute_header_crc(n0: int, n1: int, n2: int) -> int:
    """Compute LoRa header 5-bit checksum from first three nibbles.

    Mirrors the C++ compute_header_crc used in the GR-compatible header decode.
    Returns a 5-bit value with bit4..bit0.
    """
    # Executes the statement `n0 &= 0xF`.
    n0 &= 0xF
    # Executes the statement `n1 &= 0xF`.
    n1 &= 0xF
    # Executes the statement `n2 &= 0xF`.
    n2 &= 0xF
    # Executes the statement `c4 = ((n0 >> 3) & 1) ^ ((n0 >> 2) & 1) ^ ((n0 >> 1) & 1) ^ (n0 & 1)`.
    c4 = ((n0 >> 3) & 1) ^ ((n0 >> 2) & 1) ^ ((n0 >> 1) & 1) ^ (n0 & 1)
    # Executes the statement `c3 = ((n0 >> 3) & 1) ^ ((n1 >> 3) & 1) ^ ((n1 >> 2) & 1) ^ ((n1 >> 1) & 1) ^ (n2 & 1)`.
    c3 = ((n0 >> 3) & 1) ^ ((n1 >> 3) & 1) ^ ((n1 >> 2) & 1) ^ ((n1 >> 1) & 1) ^ (n2 & 1)
    # Executes the statement `c2 = ((n0 >> 2) & 1) ^ ((n1 >> 3) & 1) ^ (n1 & 1) ^ ((n2 >> 3) & 1) ^ ((n2 >> 1) & 1)`.
    c2 = ((n0 >> 2) & 1) ^ ((n1 >> 3) & 1) ^ (n1 & 1) ^ ((n2 >> 3) & 1) ^ ((n2 >> 1) & 1)
    # Executes the statement `c1 = ((n0 >> 1) & 1) ^ ((n1 >> 2) & 1) ^ (n1 & 1) ^ ((n2 >> 2) & 1) ^ ((n2 >> 1) & 1) ^ (n2 & 1)`.
    c1 = ((n0 >> 1) & 1) ^ ((n1 >> 2) & 1) ^ (n1 & 1) ^ ((n2 >> 2) & 1) ^ ((n2 >> 1) & 1) ^ (n2 & 1)
    # Executes the statement `c0 = (n0 & 1) ^ ((n1 >> 1) & 1) ^ ((n2 >> 3) & 1) ^ ((n2 >> 2) & 1) ^ ((n2 >> 1) & 1) ^ (n2 & 1)`.
    c0 = (n0 & 1) ^ ((n1 >> 1) & 1) ^ ((n2 >> 3) & 1) ^ ((n2 >> 2) & 1) ^ ((n2 >> 1) & 1) ^ (n2 & 1)
    # Returns the computed value to the caller.
    return ((c4 & 1) << 4) | ((c3 & 1) << 3) | ((c2 & 1) << 2) | ((c1 & 1) << 1) | (c0 & 1)


# Defines the function decode_lora_header.
def decode_lora_header(header_symbols: List[int], sf: int) -> dict | None:
    """Decode LoRa explicit header from oriented header symbols (expects 16 symbols for SF7).

    Algorithm:
      - Use two 8-symbol blocks from header_symbols (total 16) as the header region
      - For each block, apply -44 then divide by 4, then Gray-decode using sf_app=sf-2 bits
      - Build in_bits for both row orders (which_order=0: row=sf_app-1-b, which_order=1: row=b)
      - For mapping variants over deinterleaver/col_rev/bit_rev/nib_rev/col_phase, decode Hamming(4/8)
        per row to get nibbles. For a given which_order, concatenate nibbles across both blocks to get 10 nibbles.
      - Search rotations and byte orders to satisfy the 5-bit checksum; return fields on success.
    """
    # Begins a block that monitors for exceptions.
    try:
        # Begins a conditional branch to check a condition.
        if not isinstance(header_symbols, list) or len(header_symbols) < 8:
            # Returns the computed value to the caller.
            return None
        # Executes the statement `sf = int(sf)`.
        sf = int(sf)
        # Executes the statement `sf_app = (sf - 2) if sf > 2 else sf`.
        sf_app = (sf - 2) if sf > 2 else sf
        # Executes the statement `cw_len = 8`.
        cw_len = 8
        # Executes the statement `N = 1 << sf`.
        N = 1 << sf
        # Executes the statement `blocks = max(1, min(2, len(header_symbols) // cw_len))`.
        blocks = max(1, min(2, len(header_symbols) // cw_len))

        # Build base in_bits per block for both which_orders
        # Executes the statement `base_blocks_ord0: list[list[int]] = []  # row index = sf_app-1-b`.
        base_blocks_ord0: list[list[int]] = []  # row index = sf_app-1-b
        # Executes the statement `base_blocks_ord1: list[list[int]] = []  # row index = b`.
        base_blocks_ord1: list[list[int]] = []  # row index = b
        # Starts a loop iterating over a sequence.
        for b in range(blocks):
            # Executes the statement `base = b * cw_len`.
            base = b * cw_len
            # Executes the statement `in_bits0 = [0] * (sf_app * cw_len)`.
            in_bits0 = [0] * (sf_app * cw_len)
            # Executes the statement `in_bits1 = [0] * (sf_app * cw_len)`.
            in_bits1 = [0] * (sf_app * cw_len)
            # Starts a loop iterating over a sequence.
            for col in range(cw_len):
                # Executes the statement `sym = int(header_symbols[base + col]) % N`.
                sym = int(header_symbols[base + col]) % N
                # Executes the statement `sym = (sym - 44) % N`.
                sym = (sym - 44) % N
                # Executes the statement `div4 = sym // 4`.
                div4 = sym // 4
                # Executes the statement `gb = gray_decode(div4)`.
                gb = gray_decode(div4)
                # Begins a conditional branch to check a condition.
                if sf_app < sf:
                    # Executes the statement `gb &= (1 << sf_app) - 1`.
                    gb &= (1 << sf_app) - 1
                # Starts a loop iterating over a sequence.
                for bit_idx in range(sf_app):
                    # Executes the statement `bit = (gb >> bit_idx) & 1`.
                    bit = (gb >> bit_idx) & 1
                    # Executes the statement `row0 = sf_app - 1 - bit_idx`.
                    row0 = sf_app - 1 - bit_idx
                    # Executes the statement `row1 = bit_idx`.
                    row1 = bit_idx
                    # Executes the statement `in_bits0[col * sf_app + row0] = bit`.
                    in_bits0[col * sf_app + row0] = bit
                    # Executes the statement `in_bits1[col * sf_app + row1] = bit`.
                    in_bits1[col * sf_app + row1] = bit
            # Executes the statement `base_blocks_ord0.append(in_bits0)`.
            base_blocks_ord0.append(in_bits0)
            # Executes the statement `base_blocks_ord1.append(in_bits1)`.
            base_blocks_ord1.append(in_bits1)

        # Deinterleavers
        # Defines the function deint_legacy.
        def deint_legacy(in_bits: List[int]) -> List[int]:
            # Executes the statement `out_bits = [0] * len(in_bits)`.
            out_bits = [0] * len(in_bits)
            # Starts a loop iterating over a sequence.
            for col in range(cw_len):
                # Starts a loop iterating over a sequence.
                for row in range(sf_app):
                    # Executes the statement `dest_row = (col - row - 1) % sf_app`.
                    dest_row = (col - row - 1) % sf_app
                    # Executes the statement `out_bits[dest_row * cw_len + col] = in_bits[col * sf_app + row]`.
                    out_bits[dest_row * cw_len + col] = in_bits[col * sf_app + row]
            # Returns the computed value to the caller.
            return out_bits

        # Defines the function deint_alt.
        def deint_alt(in_bits: List[int]) -> List[int]:
            # Executes the statement `out_bits = [0] * len(in_bits)`.
            out_bits = [0] * len(in_bits)
            # Starts a loop iterating over a sequence.
            for col in range(cw_len):
                # Starts a loop iterating over a sequence.
                for row in range(sf_app):
                    # Executes the statement `dest_row = (row + col) % sf_app`.
                    dest_row = (row + col) % sf_app
                    # Executes the statement `out_bits[dest_row * cw_len + col] = in_bits[col * sf_app + row]`.
                    out_bits[dest_row * cw_len + col] = in_bits[col * sf_app + row]
            # Returns the computed value to the caller.
            return out_bits

        # Executes the statement `best_rec: dict | None = None`.
        best_rec: dict | None = None
        # Executes the statement `best_score: int = -10**9`.
        best_score: int = -10**9
        # Starts a loop iterating over a sequence.
        for which_order in (0, 1):
            # Executes the statement `blocks_src = base_blocks_ord0 if which_order == 0 else base_blocks_ord1`.
            blocks_src = base_blocks_ord0 if which_order == 0 else base_blocks_ord1
            # Starts a loop iterating over a sequence.
            for deint_mode in (0, 1):
                # Starts a loop iterating over a sequence.
                for col_rev in (0, 1):
                    # Starts a loop iterating over a sequence.
                    for bit_rev in (0, 1):
                        # Starts a loop iterating over a sequence.
                        for nib_rev in (0, 1):
                            # Starts a loop iterating over a sequence.
                            for col_phase in range(0, cw_len):
                                # Decode nibbles for this order across all blocks
                                # Executes the statement `nibbles_all: list[int] = []`.
                                nibbles_all: list[int] = []
                                # Starts a loop iterating over a sequence.
                                for in_bits in blocks_src:
                                    # Executes the statement `out_bits = deint_legacy(in_bits) if deint_mode == 0 else deint_alt(in_bits)`.
                                    out_bits = deint_legacy(in_bits) if deint_mode == 0 else deint_alt(in_bits)
                                    # Starts a loop iterating over a sequence.
                                    for row in range(sf_app):
                                        # Executes the statement `code_k = 0`.
                                        code_k = 0
                                        # Starts a loop iterating over a sequence.
                                        for col in range(cw_len):
                                            # Executes the statement `cc0 = (cw_len - 1 - col) if col_rev else col`.
                                            cc0 = (cw_len - 1 - col) if col_rev else col
                                            # Executes the statement `cc = (cc0 + col_phase) % cw_len`.
                                            cc = (cc0 + col_phase) % cw_len
                                            # Executes the statement `code_k = (code_k << 1) | (out_bits[row * cw_len + cc] & 1)`.
                                            code_k = (code_k << 1) | (out_bits[row * cw_len + cc] & 1)
                                        # Executes the statement `ck = rev_bits_k(code_k, cw_len) if bit_rev else code_k`.
                                        ck = rev_bits_k(code_k, cw_len) if bit_rev else code_k
                                        # Executes the statement `nib = hamming_decode4(ck, 4) & 0xF`.
                                        nib = hamming_decode4(ck, 4) & 0xF
                                        # Begins a conditional branch to check a condition.
                                        if nib_rev:
                                            # Executes the statement `nib = rev4(nib)`.
                                            nib = rev4(nib)
                                        # Executes the statement `nibbles_all.append(nib)`.
                                        nibbles_all.append(nib)
                                # Expect exactly blocks*sf_app nibbles; for SF7 with 2 blocks, that's 10
                                # Begins a conditional branch to check a condition.
                                if len(nibbles_all) < 10:
                                    # Skips to the next iteration of the loop.
                                    continue
                                # Use first 10 for rotation search (if >10 due to extra blocks)
                                # Executes the statement `nibbles = nibbles_all[:10]`.
                                nibbles = nibbles_all[:10]
                                # Evaluate rotations and byte orders for checksum
                                # Starts a loop iterating over a sequence.
                                for rot in range(0, 10):
                                    # Executes the statement `rot_nib = nibbles[rot:] + nibbles[:rot]`.
                                    rot_nib = nibbles[rot:] + nibbles[:rot]
                                    # Starts a loop iterating over a sequence.
                                    for order in (0, 1):
                                        # Executes the statement `hdr_bytes: list[int] = []`.
                                        hdr_bytes: list[int] = []
                                        # Starts a loop iterating over a sequence.
                                        for i in range(5):
                                            # Executes the statement `a = rot_nib[2 * i]`.
                                            a = rot_nib[2 * i]
                                            # Executes the statement `b = rot_nib[2 * i + 1]`.
                                            b = rot_nib[2 * i + 1]
                                            # Executes the statement `low = a if order == 0 else b`.
                                            low = a if order == 0 else b
                                            # Executes the statement `high = b if order == 0 else a`.
                                            high = b if order == 0 else a
                                            # Executes the statement `hdr_bytes.append(((high & 0xF) << 4) | (low & 0xF))`.
                                            hdr_bytes.append(((high & 0xF) << 4) | (low & 0xF))
                                        # Executes the statement `n0 = hdr_bytes[0] & 0x0F`.
                                        n0 = hdr_bytes[0] & 0x0F
                                        # Executes the statement `n1 = hdr_bytes[1] & 0x0F`.
                                        n1 = hdr_bytes[1] & 0x0F
                                        # Executes the statement `n2 = hdr_bytes[2] & 0x0F`.
                                        n2 = hdr_bytes[2] & 0x0F
                                        # Executes the statement `chk_rx = (((hdr_bytes[3] & 0x01) << 4) | (hdr_bytes[4] & 0x0F)) & 0x1F`.
                                        chk_rx = (((hdr_bytes[3] & 0x01) << 4) | (hdr_bytes[4] & 0x0F)) & 0x1F
                                        # Executes the statement `chk_calc = compute_header_crc(n0, n1, n2) & 0x1F`.
                                        chk_calc = compute_header_crc(n0, n1, n2) & 0x1F
                                        # Begins a conditional branch to check a condition.
                                        if chk_rx == chk_calc:
                                            # Executes the statement `payload_len = ((n0 & 0xF) << 4) | (n1 & 0xF)`.
                                            payload_len = ((n0 & 0xF) << 4) | (n1 & 0xF)
                                            # Executes the statement `has_crc = bool(n2 & 0x1)`.
                                            has_crc = bool(n2 & 0x1)
                                            # Executes the statement `cr_idx = (n2 >> 1) & 0x7`.
                                            cr_idx = (n2 >> 1) & 0x7
                                            # Executes the statement `rec = {`.
                                            rec = {
                                                # Executes the statement `'payload_len': int(payload_len),`.
                                                'payload_len': int(payload_len),
                                                # Executes the statement `'has_crc': bool(has_crc),`.
                                                'has_crc': bool(has_crc),
                                                # Executes the statement `'cr_idx': int(cr_idx),`.
                                                'cr_idx': int(cr_idx),
                                                # Executes the statement `'nibbles': rot_nib,`.
                                                'nibbles': rot_nib,
                                                # Executes the statement `'header_bytes': hdr_bytes,`.
                                                'header_bytes': hdr_bytes,
                                                # Executes the statement `'rotation': rot,`.
                                                'rotation': rot,
                                                # Executes the statement `'order': order,`.
                                                'order': order,
                                                # Executes the statement `'mapping': {`.
                                                'mapping': {
                                                    # Executes the statement `'which_order': int(which_order),`.
                                                    'which_order': int(which_order),
                                                    # Executes the statement `'deinterleaver': ('legacy' if deint_mode == 0 else 'alt'),`.
                                                    'deinterleaver': ('legacy' if deint_mode == 0 else 'alt'),
                                                    # Executes the statement `'col_rev': int(col_rev),`.
                                                    'col_rev': int(col_rev),
                                                    # Executes the statement `'bit_rev': int(bit_rev),`.
                                                    'bit_rev': int(bit_rev),
                                                    # Executes the statement `'nib_rev': int(nib_rev),`.
                                                    'nib_rev': int(nib_rev),
                                                    # Executes the statement `'col_phase': int(col_phase),`.
                                                    'col_phase': int(col_phase),
                                                # Closes the previously opened dictionary or set literal.
                                                }
                                            # Closes the previously opened dictionary or set literal.
                                            }
                                            # Executes the statement `simplicity = 0`.
                                            simplicity = 0
                                            # Begins a conditional branch to check a condition.
                                            if deint_mode == 0 and col_rev == 0 and bit_rev == 0 and nib_rev == 0 and col_phase == 0:
                                                # Executes the statement `simplicity += 10`.
                                                simplicity += 10
                                            # Prefer plausible payload lengths and CRC-on headers
                                            # Executes the statement `score = (200 if has_crc else 0) + max(0, 64 - int(payload_len)) + simplicity`.
                                            score = (200 if has_crc else 0) + max(0, 64 - int(payload_len)) + simplicity
                                            # Begins a conditional branch to check a condition.
                                            if score > best_score:
                                                # Executes the statement `best_score = score`.
                                                best_score = score
                                                # Executes the statement `best_rec = rec`.
                                                best_rec = rec
        # Returns the computed value to the caller.
        return best_rec
    # Handles a specific exception from the try block.
    except Exception:
        # Returns the computed value to the caller.
        return None


# Defines the function extract_post_hamming_bits.
def extract_post_hamming_bits(symbols: List[int], sf: int, cr: int, sf_app: int | None = None) -> List[int]:
    """Extract our post-Hamming data bits from symbols using a fixed mapping (legacy deinterleaver).

    This is intended for debugging/diff vs GNU Radio dump (stages.post_hamming_bits_b).
    It builds in_bits using order-0 (row = sf_app-1-b), applies legacy deinterleaver, decodes Hamming,
    and packs the 4-bit nibbles into LSB-first bits (b0..b3).
    """
    # Begins a conditional branch to check a condition.
    if not symbols:
        # Returns the computed value to the caller.
        return []
    # Begins a block that monitors for exceptions.
    try:
        # Executes the statement `sf_app_eff = int(sf if sf_app is None else max(1, min(int(sf_app), int(sf))))`.
        sf_app_eff = int(sf if sf_app is None else max(1, min(int(sf_app), int(sf))))
        # Executes the statement `cw_len = 4 + cr`.
        cw_len = 4 + cr
        # Build input bits per codeword with order-0
        # Executes the statement `base_blocks_inbits_ord0: list[list[int]] = []`.
        base_blocks_inbits_ord0: list[list[int]] = []
        # Executes the statement `sym_index = 0`.
        sym_index = 0
        # Starts a loop that continues while the condition holds.
        while sym_index + cw_len <= len(symbols):
            # Executes the statement `cw_symbols = symbols[sym_index:sym_index + cw_len]`.
            cw_symbols = symbols[sym_index:sym_index + cw_len]
            # Executes the statement `in_bits0 = [0] * (sf_app_eff * cw_len)`.
            in_bits0 = [0] * (sf_app_eff * cw_len)
            # Starts a loop iterating over a sequence.
            for col, symbol in enumerate(cw_symbols):
                # Executes the statement `nb = gray_decode(symbol)`.
                nb = gray_decode(symbol)
                # Begins a conditional branch to check a condition.
                if sf_app_eff < sf:
                    # Executes the statement `nb >>= (sf - sf_app_eff)`.
                    nb >>= (sf - sf_app_eff)
                # Starts a loop iterating over a sequence.
                for b in range(sf_app_eff):
                    # Executes the statement `bit = (nb >> b) & 1`.
                    bit = (nb >> b) & 1
                    # Executes the statement `row0 = sf_app_eff - 1 - b`.
                    row0 = sf_app_eff - 1 - b
                    # Executes the statement `in_bits0[col * sf_app_eff + row0] = bit`.
                    in_bits0[col * sf_app_eff + row0] = bit
            # Executes the statement `base_blocks_inbits_ord0.append(in_bits0)`.
            base_blocks_inbits_ord0.append(in_bits0)
            # Executes the statement `sym_index += cw_len`.
            sym_index += cw_len
        # Legacy deinterleaver then Hamming decode per row to nibbles, then to bits
        # Executes the statement `out_bits_total: List[int] = []`.
        out_bits_total: List[int] = []
        # Starts a loop iterating over a sequence.
        for in_bits in base_blocks_inbits_ord0:
            # Executes the statement `out_bits = [0] * len(in_bits)`.
            out_bits = [0] * len(in_bits)
            # Starts a loop iterating over a sequence.
            for col in range(cw_len):
                # Starts a loop iterating over a sequence.
                for row in range(sf_app_eff):
                    # Executes the statement `dest_row = (col - row - 1) % sf_app_eff`.
                    dest_row = (col - row - 1) % sf_app_eff
                    # Begins a conditional branch to check a condition.
                    if dest_row < 0:
                        # Executes the statement `dest_row += sf_app_eff`.
                        dest_row += sf_app_eff
                    # Executes the statement `out_bits[dest_row * cw_len + col] = in_bits[col * sf_app_eff + row]`.
                    out_bits[dest_row * cw_len + col] = in_bits[col * sf_app_eff + row]
            # Starts a loop iterating over a sequence.
            for row in range(sf_app_eff):
                # Executes the statement `code_k = 0`.
                code_k = 0
                # Starts a loop iterating over a sequence.
                for col in range(cw_len):
                    # Executes the statement `code_k = (code_k << 1) | (out_bits[row * cw_len + col] & 1)`.
                    code_k = (code_k << 1) | (out_bits[row * cw_len + col] & 1)
                # Executes the statement `decoded_nib = hamming_decode4(code_k, cr) & 0xF`.
                decoded_nib = hamming_decode4(code_k, cr) & 0xF
                # Starts a loop iterating over a sequence.
                for b in range(4):
                    # Executes the statement `out_bits_total.append((decoded_nib >> b) & 1)`.
                    out_bits_total.append((decoded_nib >> b) & 1)
        # Returns the computed value to the caller.
        return out_bits_total
    # Handles a specific exception from the try block.
    except Exception:
        # Returns the computed value to the caller.
        return []

# Begins a conditional branch to check a condition.
if __name__ == "__main__":
    # Test functions
    # Outputs diagnostic or user-facing text.
    print("🧪 Testing LoRa decode utils...")

    # Test Gray coding
    # Starts a loop iterating over a sequence.
    for i in range(8):
        # Executes the statement `encoded = gray_encode(i)`.
        encoded = gray_encode(i)
        # Executes the statement `decoded = gray_decode(encoded)`.
        decoded = gray_decode(encoded)
        # Outputs diagnostic or user-facing text.
        print(f"Gray: {i} -> {encoded} -> {decoded}")

    # Test symbol to bits
    # Executes the statement `test_symbols = [72, 69, 76, 76, 79]  # Example`.
    test_symbols = [72, 69, 76, 76, 79]  # Example
    # Executes the statement `bits = symbols_to_bits(test_symbols, 7)`.
    bits = symbols_to_bits(test_symbols, 7)
    # Outputs diagnostic or user-facing text.
    print(f"Symbols {test_symbols} -> {len(bits)} bits: {bits[:20]}...")

    # Executes the statement `bytes_result = bits_to_bytes(bits)`.
    bytes_result = bits_to_bytes(bits)
    # Outputs diagnostic or user-facing text.
    print(f"Bits -> {len(bytes_result)} bytes: {bytes_result}")
