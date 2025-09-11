#!/usr/bin/env python3
import struct

SF = 7
N = 1 << SF
SF_APP = SF - 2

def read_shorts(path, count=None):
    data = open(path,'rb').read()
    n = len(data) // 2
    vals = list(struct.unpack('<%dH' % n, data[:2*(count if count else n)]))
    return vals

def read_nibbles(path, count=None):
    data = open(path,'rb').read()
    if count is None:
        return list(data)
    return list(data[:count])
def read_bytes(path, count=None):
    data = open(path,'rb').read()
    if count is None:
        return list(data)
    return list(data[:count])

def enc48(n):
    d0 = (n >> 0) & 1
    d1 = (n >> 1) & 1
    d2 = (n >> 2) & 1
    d3 = (n >> 3) & 1
    p1 = d0 ^ d1 ^ d3
    p2 = d0 ^ d2 ^ d3
    p3 = d1 ^ d2 ^ d3
    p0 = d0 ^ d1 ^ d2 ^ d3 ^ p1 ^ p2 ^ p3
    return (n & 0xF) | (p1 << 4) | (p2 << 5) | (p3 << 6) | (p0 << 7)

INV = {enc48(n): n for n in range(16)}

def build_bits(vals, msb_first=True, gray_domain=True):
    bits = []
    for v in vals:
        # GR tap is GrayEncode(raw). Convert to raw bin, apply header offset -44, re-Gray to get g, then reduce.
        def gray_decode7(x):
            x &= (N-1)
            x ^= x >> 1
            x ^= x >> 2
            x ^= x >> 4
            return x & (N-1)
        def gray_encode7(x):
            return (x ^ (x >> 1)) & (N-1)
        raw_bin = gray_decode7(v)
        bin_corr = (raw_bin - 44) % N
        g_corr = gray_encode7(bin_corr)
        gnu_red = (g_corr - 1) // 4
        if msb_first:
            rng = range(SF_APP-1, -1, -1)
        else:
            rng = range(0, SF_APP)
        for b in rng:
            bits.append((gnu_red >> b) & 1)
    return bits

def make_map(nrows, ncols, origin_type, shift):
    # origin_type selects mapping variant:
    # 0: in_row = (r + c + shift) % nrows (row-major)
    # 1: in_row = (r - c - shift) % nrows (row-major)
    # 2: like 0 but col-major flatten
    # 3: like 1 but col-major flatten
    m = [0] * (nrows * ncols)
    if origin_type in (0,1):
        for r in range(nrows):
            for c in range(ncols):
                in_row = (r + c + shift) % nrows if origin_type == 0 else (r - c - shift) % nrows
                out_idx = r * ncols + c
                in_idx  = in_row * ncols + c
                m[out_idx] = in_idx
    else:
        for r in range(nrows):
            for c in range(ncols):
                in_row = (r + c + shift) % nrows if origin_type == 2 else (r - c - shift) % nrows
                out_idx = c * nrows + r
                in_idx  = c * nrows + in_row
                m[out_idx] = in_idx
    return m

def deinterleave(bits, m):
    out = [0]*len(bits)
    blk = len(m)
    for off in range(0, len(bits), blk):
        for i in range(blk):
            out[off + m[i]] = bits[off + i]
    return out

def assemble_header_codewords(deint_bits):
    # deint_bits length = 80. Interpret as two blocks of (SF_APP x 8) bits, row-major: r=0..SF_APP-1, c=0..7
    if len(deint_bits) != SF_APP * 8 * 2:
        return None
    blk0 = deint_bits[:SF_APP*8]
    blk1 = deint_bits[SF_APP*8:]
    def get(blk, r, c):
        return blk[r*8 + c]
    cws = []
    # Codewords 0..7: for each column j, take 5 bits from block0 rows 0..4, then 3 bits from block1 rows 0..2, MSB-first
    for j in range(8):
        bits = [get(blk0, r, j) for r in range(SF_APP)] + [get(blk1, r, j) for r in range(3)]
        cw = 0
        for b in bits:
            cw = (cw << 1) | (b & 1)
        cws.append(cw)
    # Codeword 8: block1 row3, columns 0..7 (MSB-first across columns)
    bits8 = [get(blk1, 3, j) for j in range(8)]
    cw8 = 0
    for b in bits8:
        cw8 = (cw8 << 1) | (b & 1)
    cws.append(cw8)
    # Codeword 9: block1 row4, columns 0..7
    bits9 = [get(blk1, 4, j) for j in range(8)]
    cw9 = 0
    for b in bits9:
        cw9 = (cw9 << 1) | (b & 1)
    cws.append(cw9)
    return cws

def rx_cw_from_gray(vals):
    # vals: 16 Gray-coded header symbols (shorts) from gray_mapping (i.e., Gray(gnu)).
    # Build inter_bin using the TOP sf_app bits (MSBs) of the Gray-coded value.
    assert len(vals) >= 16
    cw_len = 8
    cw_all = []
    for blk in range(2):
        inter_bin = [[0]*SF_APP for _ in range(cw_len)]
        for i in range(cw_len):
            full = vals[blk*cw_len + i] & (N-1)
            sub  = full & ((1 << SF_APP) - 1)  # use LSB sf_app bits (matches hard path int2bool)
            for j in range(SF_APP):
                inter_bin[i][j] = (sub >> (SF_APP - 1 - j)) & 1
        deinter_bin = [[0]*cw_len for _ in range(SF_APP)]
        for i in range(cw_len):
            for j in range(SF_APP):
                r = (i - j - 1) % SF_APP
                deinter_bin[r][i] = inter_bin[i][j]
        for r in range(SF_APP):
            cw = 0
            for c in range(cw_len):
                cw = (cw << 1) | (deinter_bin[r][c] & 1)
            cw_all.append(cw)
    return cw_all

def decode_hamming_nibbles_from_cws(cws):
    nibb = []
    for cw in cws:
        if cw in INV:
            nibb.append(INV[cw])
        else:
            return None
    return nibb

def main():
    # Prefer raw bins; otherwise use gray-coded symbols
    # Always use gray-coded header tap for this probe
    use_raw = False
    vals = read_shorts('logs/gr_hdr_gray.bin')
    # Target CW bytes from GR deinterleaver tap (directly 10 bytes)
    try:
        cw_target = read_bytes('logs/gr_deint_bits.bin', 10)
    except FileNotFoundError:
        print('Missing logs/gr_deint_bits.bin; cannot calibrate to CW target')
        return 2
    print('Target CW:', ' '.join(f'{x:02x}' for x in cw_target))
    # Target nibbles from GR hamming_dec tap
    try:
        nibs_gr = read_nibbles('logs/gr_hdr_nibbles.bin', 10)
    except FileNotFoundError:
        nibs_gr = None
    # Search for a 16-symbol window that matches
    best = (999, None)
    for start in range(0, max(0, len(vals) - 16 + 1)):
        chunk = vals[start:start+16]
        cws = rx_cw_from_gray(chunk)
        dist = sum(1 for a,b in zip(cws[:10], cw_target[:10]) if a!=b)
        if dist < best[0]:
            best = (dist, (start, cws[:10]))
        if dist == 0:
            print('MATCH via exact inverse mapping at start', start)
            print('cwbytes:', ' '.join(f'{x:02x}' for x in cws[:10]))
            return 0
    print('No exact CW match. Best candidate:', best[0], 'byte mismatches')
    # If nibble target available, try Hamming decode comparison
    if nibs_gr is not None:
        bestn = (999, None)
        for start in range(0, max(0, len(vals) - 16 + 1)):
            chunk = vals[start:start+16]
            cws = rx_cw_from_gray(chunk)[:10]
            nibb = decode_hamming_nibbles_from_cws(cws)
            if nibb is None:
                continue
            dn = sum(1 for a,b in zip(nibb, nibs_gr) if a!=b)
            if dn < bestn[0]:
                bestn = (dn, (start, nibb))
            if dn == 0:
                print('MATCH via Hamming(nibbles) at start', start)
                print('nibbles:', [hex(x) for x in nibb])
                return 0
        print('No exact nibble match. Best candidate:', bestn[0], 'nibble mismatches')
        if bestn[1]:
            start, nibb = bestn[1]
            print(' start', start)
            print(' cand nibbles:', [hex(x) for x in nibb])
            print(' gr   nibbles:', [hex(x) for x in nibs_gr])
    if best[1]:
        start, cws = best[1]
        print(' start', start)
        print('cand cwbytes:', ' '.join(f'{x:02x}' for x in cws))
        print('gr   cwbytes:', ' '.join(f'{x:02x}' for x in cw_target[:10]))
    return 1

if __name__=='__main__':
    raise SystemExit(main())
