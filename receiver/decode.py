from __future__ import annotations

from typing import List

from .utils import gray_to_binary


def payload_symbols_to_bytes(payload_symbols: List[int], sf: int, cr: int, bin_offset: int = 0, invert_bins: bool = False, ldro_mode: int = 0) -> List[int]:
    if not payload_symbols:
        return []
    try:
        from lora_decode_utils import decode_lora_payload
        N = 1 << sf
        nat_syms: List[int] = []
        for k in payload_symbols:
            k_no_off = (k - bin_offset) % N
            if invert_bins:
                sym_m1 = (-k_no_off) % N
            else:
                sym_m1 = k_no_off
            # Align to GNU Radio's symbol convention (no +1 wrap)
            sym_nat = sym_m1 % N
            nat_syms.append(int(sym_nat))
        sf_app = sf - 1 if ldro_mode == 2 else sf
        decoded_bytes = decode_lora_payload(nat_syms, sf, cr, sf_app=sf_app)
        return decoded_bytes
    except Exception:
        # Fallback: naive bit packing
        return _simple_symbol_to_bytes(payload_symbols, sf)


def _simple_symbol_to_bytes(payload_symbols: List[int], sf: int) -> List[int]:
    bytes_list: List[int] = []
    bit_buffer: List[int] = []
    for symbol in payload_symbols:
                raise ImportError("receiver.decode removed. Use external tooling or lora_decode_utils if present")
            continue
        for bit_pos in range(sf):
            bit_buffer.append((symbol >> bit_pos) & 1)
        while len(bit_buffer) >= 8:
            byte_val = 0
            for i in range(8):
                byte_val |= (bit_buffer.pop(0) << i)
            bytes_list.append(byte_val)
    return bytes_list
