
import sys

def gray_decode(val):
    res = val
    while val > 0:
        val >>= 1
        res ^= val
    return res

def gray_encode(val):
    return val ^ (val >> 1)

def hamming_decode(byte_val, msb_first=True, nibble_order_normal=True):
    bits = []
    for i in range(8):
        if msb_first:
            bits.append((byte_val >> (7-i)) & 1)
        else:
            bits.append((byte_val >> i) & 1)
    
    # bits[0] is now the first bit (MSB if msb_first)
    
    if nibble_order_normal:
        # data = {bits[3], bits[2], bits[1], bits[0]}
        data = [bits[3], bits[2], bits[1], bits[0]]
    else:
        # data = {bits[0], bits[1], bits[2], bits[3]}
        data = [bits[0], bits[1], bits[2], bits[3]]
        
    # Syndrome
    s0 = bits[0] ^ bits[1] ^ bits[2] ^ bits[4]
    s1 = bits[1] ^ bits[2] ^ bits[3] ^ bits[5]
    s2 = bits[0] ^ bits[1] ^ bits[3] ^ bits[6]
    
    syndrom = s0 | (s1 << 1) | (s2 << 2)
    
    if syndrom == 5: data[3] ^= 1
    elif syndrom == 7: data[2] ^= 1
    elif syndrom == 3: data[1] ^= 1
    elif syndrom == 6: data[0] ^= 1
    
    res = 0
    for b in data:
        res = (res << 1) | b
    return res

def compute_checksum(payload_len, has_crc, cr):
    h = [
        (payload_len >> 7) & 1,
        (payload_len >> 6) & 1,
        (payload_len >> 5) & 1,
        (payload_len >> 4) & 1,
        (payload_len >> 3) & 1,
        (payload_len >> 2) & 1,
        (payload_len >> 1) & 1,
        payload_len & 1,
        has_crc,
        (cr >> 2) & 1,
        (cr >> 1) & 1,
        cr & 1
    ]
    G = [
        [1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0],
        [1, 0, 0, 0, 1, 1, 1, 0, 1, 0, 0, 0],
        [0, 1, 0, 0, 1, 0, 0, 1, 0, 1, 0, 1],
        [0, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 1],
        [0, 0, 0, 1, 0, 0, 1, 0, 1, 1, 1, 1]
    ]
    bits = [0]*5
    for row in range(5):
        acc = 0
        for col in range(12):
            acc ^= G[row][col] & h[col]
        bits[row] = acc & 1
    return (bits[0] << 4) | (bits[1] << 3) | (bits[2] << 2) | (bits[3] << 1) | bits[4]

raw_symbols = [58, 16, 1, 45, 125, 109, 81, 69]
sf = 7
mask_sf = (1 << sf) - 1

print(f"Searching for len=32, crc=1, cr=1 in symbols: {raw_symbols}")

# Options
offsets = [0, 1, -1]
gray_modes = ['decode', 'encode', 'none']
sf_apps = [5, 6] 
shifts = range(-5, 5) 
msb_firsts = [True, False]
nibble_orders = [True, False]
inter_msb_firsts = [True, False]

for offset in offsets:
    for gray_mode in gray_modes:
        for sf_app in sf_apps:
            for inter_msb in inter_msb_firsts:
                    
                    # Deinterleave
                    inter_matrix = []
                    for i in range(8):
                        raw = raw_symbols[i]
                        if gray_mode == 'decode':
                            mapped = gray_decode(raw)
                        elif gray_mode == 'encode':
                            mapped = gray_encode(raw)
                        else:
                            mapped = raw
                        
                        mapped = (mapped + offset) & mask_sf
                        # Take sf_app LSBs
                        mapped &= ((1 << sf_app) - 1)
                        
                        # int to bits
                        bits = []
                        for b in range(sf_app):
                            if inter_msb:
                                bits.append((mapped >> (sf_app-1-b)) & 1)
                            else:
                                bits.append((mapped >> b) & 1)
                        inter_matrix.append(bits)
                    
                    for shift in shifts:
                        # Diagonal
                        codewords = []
                        for row in range(5): 
                            cw_bits = []
                            for i in range(8):
                                j = (i + shift - row) % sf_app
                                cw_bits.append(inter_matrix[i][j])
                            
                            cw_val = 0
                            for b in cw_bits:
                                cw_val = (cw_val << 1) | b
                            codewords.append(cw_val)
                        
                        # Hamming decode
                        for msb in msb_firsts:
                            for nib_ord in nibble_orders:
                                nibbles = []
                                for cw in codewords:
                                    nibbles.append(hamming_decode(cw, msb, nib_ord))
                                
                                n0 = nibbles[0]
                                n1 = nibbles[1]
                                n2 = nibbles[2]
                                n3 = nibbles[3]
                                n4 = nibbles[4]
                                
                                payload_len = (n0 << 4) | n1
                                has_crc = (n2 & 1) != 0
                                cr = (n2 >> 1) & 7
                                chk = ((n3 & 1) << 4) | n4
                                
                            if payload_len > 0:
                                computed = compute_checksum(payload_len, has_crc, cr)
                                if chk == computed:
                                    print(f"MATCH! offset={offset}, gray={gray_mode}, sf_app={sf_app}, inter_msb={inter_msb}, shift={shift}, msb={msb}, nib_ord={nib_ord}")
                                    print(f"Nibbles: {nibbles}, Len: {payload_len}, CRC: {has_crc}, CR: {cr}, Chk: {chk}")
                                    if payload_len == 32 and has_crc and cr == 1:
                                        print("*** EXACT MATCH FOR TARGET ***")
                                        sys.exit(0)

print("Done")
