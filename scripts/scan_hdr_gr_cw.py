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
    pat2v = re.compile(r"hdr_gr cwbytes \(2blk-var samp0=([\-\d]+) off0=(\d+) samp1=([\-\d]+) off1=(\d+) mode=(raw|corr) rot1=(\d+) rowrev1=(\d+) colrev1=(\d+)\):\s+([0-9a-f]{2}(?:\s+[0-9a-f]{2}){9})", re.I)
    pat2vs = re.compile(r"hdr_gr cwbytes \(2blk-varshift samp0=([\-\d]+) off0=(\d+) samp1=([\-\d]+) off1=(\d+) mode=(raw|corr) rot1=(\d+) rowrev1=(\d+) colshift1=(\d+) colrev1=(\d+)\):\s+([0-9a-f]{2}(?:\s+[0-9a-f]{2}){9})", re.I)
    pat2c = re.compile(r"hdr_gr cwbytes \(2blk-col samp0=([\-\d]+) off0=(\d+) samp1=([\-\d]+) off1=(\d+) mode=(raw|corr) colrev=(\d+) rowrev=(\d+) swap=(\d+)\):\s+([0-9a-f]{2}(?:\s+[0-9a-f]{2}){9})", re.I)
    pat2v2 = re.compile(r"hdr_gr cwbytes \(2blk-var2 samp0=([\-\d]+) off0=(\d+) samp1=([\-\d]+) off1=(\d+) mode=(raw|corr) altdiag=1 colrev1=(\d+)\):\s+([0-9a-f]{2}(?:\s+[0-9a-f]{2}){9})", re.I)
    pat2dv = re.compile(r"hdr_gr cwbytes \(2blk-diagvar samp0=([\-\d]+) off0=(\d+) samp1=([\-\d]+) off1=(\d+) mode=(raw|corr) diagshift=(\-?\d+) rot1=(\d+) rowrev1=(\d+) colshift1=(\d+) colrev1=(\d+)\):\s+([0-9a-f]{2}(?:\s+[0-9a-f]{2}){9})", re.I)
    pat2ds = re.compile(r"hdr_gr cwbytes \(2blk-diagshift samp0=([\-\d]+) off0=(\d+) samp1=([\-\d]+) off1=(\d+) mode=(raw|corr) diagshift=([\-\d]+) colrev1=(\d+)\):\s+([0-9a-f]{2}(?:\s+[0-9a-f]{2}){9})", re.I)
    pat2vs = re.compile(r"hdr_gr cwbytes \(2blk-varshift samp0=([\-\d]+) off0=(\d+) samp1=([\-\d]+) off1=(\d+) mode=(raw|corr) rot1=(\d+) rowrev1=(\d+) colshift1=(\d+) colrev1=(\d+)\):\s+([0-9a-f]{2}(?:\s+[0-9a-f]{2}){9})", re.I)
    best=(999,None,'')
    for ln in logp.read_text(errors='ignore').splitlines():
        m1=pat1.search(ln)
        m2=pat2.search(ln)
        m2v=pat2v.search(ln)
        m2vs=pat2vs.search(ln)
        m2c=pat2c.search(ln)
        m2v2=pat2v2.search(ln)
        m2dv=pat2dv.search(ln)
        m2ds=pat2ds.search(ln)
        m2vs=pat2vs.search(ln)
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
        elif m2v:
            samp0=int(m2v.group(1)); off0=int(m2v.group(2)); samp1=int(m2v.group(3)); off1=int(m2v.group(4)); mode=m2v.group(5)
            rot1=int(m2v.group(6)); rowrev1=int(m2v.group(7)); colrev1=int(m2v.group(8))
            vals=[int(x,16) for x in m2v.group(9).split()]
            d=sum(1 for a,b in zip(vals,tgt) if a!=b)
            if d<best[0]: best=(d,(samp0,off0,samp1,off1,mode,rot1,rowrev1,colrev1,vals),'2blk-var')
        elif m2vs:
            samp0=int(m2vs.group(1)); off0=int(m2vs.group(2)); samp1=int(m2vs.group(3)); off1=int(m2vs.group(4)); mode=m2vs.group(5)
            rot1=int(m2vs.group(6)); rowrev1=int(m2vs.group(7)); colshift1=int(m2vs.group(8)); colrev1=int(m2vs.group(9))
            vals=[int(x,16) for x in m2vs.group(10).split()]
            d=sum(1 for a,b in zip(vals,tgt) if a!=b)
            if d<best[0]: best=(d,(samp0,off0,samp1,off1,mode,rot1,rowrev1,colshift1,colrev1,vals),'2blk-varshift')
        elif m2c:
            samp0=int(m2c.group(1)); off0=int(m2c.group(2)); samp1=int(m2c.group(3)); off1=int(m2c.group(4)); mode=m2c.group(5)
            colrev=int(m2c.group(6)); rowrev=int(m2c.group(7)); swap=int(m2c.group(8))
            vals=[int(x,16) for x in m2c.group(9).split()]
            d=sum(1 for a,b in zip(vals,tgt) if a!=b)
            if d<best[0]: best=(d,(samp0,off0,samp1,off1,mode,colrev,rowrev,swap,vals),'2blk-col')
        elif m2v2:
            samp0=int(m2v2.group(1)); off0=int(m2v2.group(2)); samp1=int(m2v2.group(3)); off1=int(m2v2.group(4)); mode=m2v2.group(5)
            colrev1=int(m2v2.group(6))
            vals=[int(x,16) for x in m2v2.group(7).split()]
            d=sum(1 for a,b in zip(vals,tgt) if a!=b)
            if d<best[0]: best=(d,(samp0,off0,samp1,off1,mode,colrev1,vals),'2blk-var2')
        elif m2dv:
            samp0=int(m2dv.group(1)); off0=int(m2dv.group(2)); samp1=int(m2dv.group(3)); off1=int(m2dv.group(4)); mode=m2dv.group(5)
            diagshift=int(m2dv.group(6)); rot1=int(m2dv.group(7)); rowrev1=int(m2dv.group(8)); colshift1=int(m2dv.group(9)); colrev1=int(m2dv.group(10))
            vals=[int(x,16) for x in m2dv.group(11).split()]
            d=sum(1 for a,b in zip(vals,tgt) if a!=b)
            if d<best[0]: best=(d,(samp0,off0,samp1,off1,mode,diagshift,rot1,rowrev1,colshift1,colrev1,vals),'2blk-diagvar')
        elif m2ds:
            samp0=int(m2ds.group(1)); off0=int(m2ds.group(2)); samp1=int(m2ds.group(3)); off1=int(m2ds.group(4)); mode=m2ds.group(5)
            diagshift=int(m2ds.group(6)); colrev1=int(m2ds.group(7))
            vals=[int(x,16) for x in m2ds.group(8).split()]
            d=sum(1 for a,b in zip(vals,tgt) if a!=b)
            if d<best[0]: best=(d,(samp0,off0,samp1,off1,mode,diagshift,colrev1,vals),'2blk-diagshift')
        elif m2vs:
            samp0=int(m2vs.group(1)); off0=int(m2vs.group(2)); samp1=int(m2vs.group(3)); off1=int(m2vs.group(4)); mode=m2vs.group(5)
            rot1=int(m2vs.group(6)); rowrev1=int(m2vs.group(7)); colshift1=int(m2vs.group(8)); colrev1=int(m2vs.group(9))
            vals=[int(x,16) for x in m2vs.group(10).split()]
            d=sum(1 for a,b in zip(vals,tgt) if a!=b)
            if d<best[0]: best=(d,(samp0,off0,samp1,off1,mode,rot1,rowrev1,colshift1,colrev1,vals),'2blk-varshift')
    print('best cw distance:', best[0], 'pattern', best[2])
    if best[1]:
        if best[2]=='1blk':
            samp,off,mode,vals=best[1]
            print('at samp',samp,'off',off,'mode',mode,'vals',' '.join(f'{x:02x}' for x in vals))
        else:
            if best[2]=='2blk':
                samp0,off0,samp1,off1,mode,vals=best[1]
                print('at samp0',samp0,'off0',off0,'samp1',samp1,'off1',off1,'mode',mode,'vals',' '.join(f'{x:02x}' for x in vals))
            elif best[2]=='2blk-col':
                samp0,off0,samp1,off1,mode,colrev,rowrev,swap,vals=best[1]
                print('at samp0',samp0,'off0',off0,'samp1',samp1,'off1',off1,'mode',mode,'colrev',colrev,'rowrev',rowrev,'swap',swap,'vals',' '.join(f'{x:02x}' for x in vals))
            elif best[2]=='2blk-var2':
                samp0,off0,samp1,off1,mode,colrev1,vals=best[1]
                print('at samp0',samp0,'off0',off0,'samp1',samp1,'off1',off1,'mode',mode,'altdiag',1,'colrev1',colrev1,'vals',' '.join(f'{x:02x}' for x in vals))
            elif best[2]=='2blk-varshift':
                samp0,off0,samp1,off1,mode,rot1,rowrev1,colshift1,colrev1,vals=best[1]
                print('at samp0',samp0,'off0',off0,'samp1',samp1,'off1',off1,'mode',mode,'rot1',rot1,'rowrev1',rowrev1,'colshift1',colshift1,'colrev1',colrev1,'vals',' '.join(f'{x:02x}' for x in vals))
            elif best[2]=='2blk-diagvar':
                samp0,off0,samp1,off1,mode,diagshift,rot1,rowrev1,colshift1,colrev1,vals=best[1]
                print('at samp0',samp0,'off0',off0,'samp1',samp1,'off1',off1,'mode',mode,'diagshift',diagshift,'rot1',rot1,'rowrev1',rowrev1,'colshift1',colshift1,'colrev1',colrev1,'vals',' '.join(f'{x:02x}' for x in vals))
            elif best[2]=='2blk-diagshift':
                samp0,off0,samp1,off1,mode,diagshift,colrev1,vals=best[1]
                print('at samp0',samp0,'off0',off0,'samp1',samp1,'off1',off1,'mode',mode,'diagshift',diagshift,'colrev1',colrev1,'vals',' '.join(f'{x:02x}' for x in vals))
            elif best[2]=='2blk-varshift':
                samp0,off0,samp1,off1,mode,rot1,rowrev1,colshift1,colrev1,vals=best[1]
                print('at samp0',samp0,'off0',off0,'samp1',samp1,'off1',off1,'mode',mode,'rot1',rot1,'rowrev1',rowrev1,'colshift1',colshift1,'colrev1',colrev1,'vals',' '.join(f'{x:02x}' for x in vals))
            else:
                samp0,off0,samp1,off1,mode,rot1,rowrev1,colrev1,vals=best[1]
                print('at samp0',samp0,'off0',off0,'samp1',samp1,'off1',off1,'mode',mode,'rot1',rot1,'rowrev1',rowrev1,'colrev1',colrev1,'vals',' '.join(f'{x:02x}' for x in vals))
        print('tgt',' '.join(f'{x:02x}' for x in tgt))
    return 0

if __name__=='__main__':
    raise SystemExit(main())
