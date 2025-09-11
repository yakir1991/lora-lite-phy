#!/usr/bin/env python3
import struct, sys

SF=7
SF_APP=SF-2
CW_LEN=8

def main():
    b=open('logs/gr_hdr_gray.bin','rb').read()
    vals=list(struct.unpack('<%dH' % (len(b)//2), b))[:16]
    cwbytes=[]
    for blk in range(2):
        inter=[[0]*SF_APP for _ in range(CW_LEN)]
        for i in range(CW_LEN):
            full = vals[blk*CW_LEN+i] & ((1<<SF)-1)
            v = (full >> (SF - SF_APP)) & ((1<<SF_APP)-1)  # take top sf_app bits (MSBs)
            for j in range(SF_APP):
                inter[i][j] = (v >> (SF_APP-1-j)) & 1
        deint=[[0]*CW_LEN for _ in range(SF_APP)]
        for i in range(CW_LEN):
            for j in range(SF_APP):
                r = (i - j - 1) % SF_APP
                deint[r][i] = inter[i][j]
        for r in range(SF_APP):
            cw=0
            for i in range(CW_LEN):
                cw = (cw<<1) | deint[r][i]
            cwbytes.append(cw)
    print('cw from hdr_gray:', ' '.join(f'{x:02x}' for x in cwbytes))
    tgt=list(open('logs/gr_deint_bits.bin','rb').read()[:10])
    print(' target cwbytes:', ' '.join(f'{x:02x}' for x in tgt))

if __name__=='__main__':
    main()
