#!/usr/bin/env python3
from pathlib import Path

SF = 7
N = 1 << SF
SF_APP = SF - 2

def gray_decode(x, sf=SF):
    x &= (1<<sf)-1
    x ^= x >> 1
    x ^= x >> 2
    x ^= x >> 4
    return x & ((1<<sf)-1)

def block_cw_from_gray(vals8):
    cw_len = 8
    # inter_bin[i][j] = bits of gnu MSB-first
    inter_bin = [[0]*SF_APP for _ in range(cw_len)]
    for i in range(cw_len):
        gnu = gray_decode(vals8[i], sf=SF)
        for j in range(SF_APP):
            inter_bin[i][j] = (gnu >> (SF_APP - 1 - j)) & 1
    # deinter: deinter_bin[(i - j - 1) mod sf_app][i] = inter_bin[i][j]
    deinter_bin = [[0]*cw_len for _ in range(SF_APP)]
    for i in range(cw_len):
        for j in range(SF_APP):
            r = (i - j - 1) % SF_APP
            deinter_bin[r][i] = inter_bin[i][j]
    # rows → cw bytes
    cw = []
    for r in range(SF_APP):
        val = 0
        for c in range(cw_len):
            val = (val << 1) | (deinter_bin[r][c] & 1)
        cw.append(val)
    return cw

def main():
    p = Path('logs/gr_hdr_gray.bin')
    if not p.exists():
        print('missing logs/gr_hdr_gray.bin'); return 2
    import struct
    data = p.read_bytes()
    vals = list(struct.unpack('<%dH' % (len(data)//2), data))
    first8 = vals[:8]
    cw0 = block_cw_from_gray(first8)
    print('first8:', first8)
    print('cw0:', ' '.join(f'{x:02x}' for x in cw0))
    # Expected first five cw bytes from GR deinterleaver target
    print('target first5:', ' '.join(['00','74','c5','00','c5']))
    return 0

if __name__ == '__main__':
    raise SystemExit(main())

