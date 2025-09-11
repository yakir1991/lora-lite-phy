#!/usr/bin/env python3
import argparse

def read_bytes(path):
    with open(path, 'rb') as f:
        return f.read()

def pn9_bytes(length: int, seed: int = 0x1FF) -> bytes:
    # PN9 LFSR: x^9 + x^5 + 1, 9-bit state, output 8 bits per step
    # This matches common LoRa whitening (see ws.LfsrWhitening::pn9_default)
    state = seed & 0x1FF
    out = bytearray()
    for _ in range(length):
        byte = 0
        for _ in range(8):
            # Output bit is LSB of state
            bit = state & 1
            byte |= (bit << (7 - (_ % 8)))
            # Feedback from taps (bit 0 XOR bit 4) per x^9 + x^5 + 1
            fb = ((state & 1) ^ ((state >> 4) & 1)) & 1
            state = (state >> 1) | (fb << 8)
        out.append(byte)
    return bytes(out)

def to_hex(b: bytes, limit: int = 64) -> str:
    return ' '.join(f"{x:02x}" for x in b[:limit])

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pre', required=True)
    ap.add_argument('--post', required=True)
    ap.add_argument('--payload-len', type=int, required=True)
    args = ap.parse_args()

    pre = read_bytes(args.pre)
    post = read_bytes(args.post)
    n = args.payload_len
    if len(pre) < n + 2 or len(post) < n + 2:
        print(f"error: buffers too short: pre={len(pre)}, post={len(post)}, need>={n+2}")
        return 2

    pn9_guess = bytes(a ^ b for a, b in zip(pre[:n], post[:n]))
    expected = pn9_bytes(n)
    crc_tail_equal = pre[n:n+2] == post[n:n+2]
    equal = pn9_guess == expected

    print("pn9_guess:", to_hex(pn9_guess, n))
    print("pn9_expected:", to_hex(expected, n))
    print("pn9_match:", equal)
    print("crc_tail_equal:", crc_tail_equal, "tail_pre:", to_hex(pre[n:n+2], 2), "tail_post:", to_hex(post[n:n+2], 2))
    return 0 if equal and crc_tail_equal else 1

if __name__ == '__main__':
    raise SystemExit(main())


