#!/usr/bin/env python3
import re, sys
from pathlib import Path

def main():
    logp = Path('logs/lite_ld.json')
    if not logp.exists():
        print('missing logs/lite_ld.json'); return 2
    tgt = list(open('logs/gr_deint_bits.bin','rb').read()[:10])
    pat1 = re.compile(r"hdr_gr cwbytes \(samp=([\-\d]+) off=(\d+) mode=(raw|corr)\):\s+([0-9a-f]{2}(?:\s+[0-9a-f]{2}){9})", re.I)
    pat2 = re.compile(r"hdr_gr cwbytes \(2blk samp0=([\-\d]+) off0=(\d+) samp1=([\-\d]+) off1=(\d+) mode=(raw|corr)\):\s+([0-9a-f]{2}(?:\s+[0-9a-f]{2}){9})", re.I)
    pat2c = re.compile(r"hdr_gr cwbytes \(2blk-col samp0=([\-\d]+) off0=(\d+) samp1=([\-\d]+) off1=(\d+) mode=(raw|corr) colrev=(\d+) rowrev=(\d+) swap=(\d+)\):\s+([0-9a-f]{2}(?:\s+[0-9a-f]{2}){9})", re.I)
    best=(999,None,'')
    for ln in logp.read_text(errors='ignore').splitlines():
        m1=pat1.search(ln)
        m2=pat2.search(ln)
        m2c=pat2c.search(ln)
        if m1:
            samp=int(m1.group(1)); off=int(m1.group(2)); mode=m1.group(3)
            vals=[int(x,16) for x in m1.group(4).split()]
            d=sum(1 for a,b in zip(vals,tgt) if a!=b)
            if d<best[0]: best=(d,(samp,off,mode,vals),'1blk')
        elif m2:
            samp0=int(m2.group(1)); off0=int(m2.group(2)); samp1=int(m2.group(3)); off1=int(m2.group(4)); mode=m2.group(5)
            vals=[int(x,16) for x in m2.group(6).split()]
            d=sum(1 for a,b in zip(vals,tgt) if a!=b)
            if d<best[0]: best=(d,(samp0,off0,samp1,off1,mode,vals),'2blk')
        elif m2c:
            samp0=int(m2c.group(1)); off0=int(m2c.group(2)); samp1=int(m2c.group(3)); off1=int(m2c.group(4)); mode=m2c.group(5)
            colrev=int(m2c.group(6)); rowrev=int(m2c.group(7)); swap=int(m2c.group(8))
            vals=[int(x,16) for x in m2c.group(9).split()]
            d=sum(1 for a,b in zip(vals,tgt) if a!=b)
            if d<best[0]: best=(d,(samp0,off0,samp1,off1,mode,colrev,rowrev,swap,vals),'2blk-col')
    print('best cw distance:', best[0], 'pattern', best[2])
    if best[1]:
        if best[2]=='1blk':
            samp,off,mode,vals=best[1]
            print('at samp',samp,'off',off,'mode',mode,'vals',' '.join(f'{x:02x}' for x in vals))
        else:
            if best[2]=='2blk':
                samp0,off0,samp1,off1,mode,vals=best[1]
                print('at samp0',samp0,'off0',off0,'samp1',samp1,'off1',off1,'mode',mode,'vals',' '.join(f'{x:02x}' for x in vals))
            else:
                samp0,off0,samp1,off1,mode,colrev,rowrev,swap,vals=best[1]
                print('at samp0',samp0,'off0',off0,'samp1',samp1,'off1',off1,'mode',mode,'colrev',colrev,'rowrev',rowrev,'swap',swap,'vals',' '.join(f'{x:02x}' for x in vals))
        print('tgt',' '.join(f'{x:02x}' for x in tgt))
    return 0

if __name__=='__main__':
    raise SystemExit(main())
