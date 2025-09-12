#!/usr/bin/env python3
import json, re, struct
from pathlib import Path

SF = 7
N = 1 << SF
SF_APP = SF - 2

def read_bytes(path, count=None):
    b = Path(path).read_bytes()
    return list(b[:count] if count else b)

def parse_last_json(path):
    # Read last JSON object from lite_ld.json
    txt = Path(path).read_text(errors='ignore')
    # Find the last JSON object braces
    objs = re.findall(r"\{[\s\S]*?\}\s*$", txt, re.M)
    if objs:
        for obj in reversed(objs):
            try:
                return json.loads(obj)
            except Exception:
                continue
    # Fallback: try to parse whole file if it's a single JSON
    try:
        return json.loads(txt)
    except Exception:
        return None

def parse_debug_gray_from_log(path):
    # Fallback: parse lines like "DEBUG: Header symbol k: raw=.., corr=.., gray=.."
    txt = Path(path).read_text(errors='ignore').splitlines()
    pat = re.compile(r"^DEBUG: Header symbol\s+(\d+):\s+raw=\d+,\s+corr=\d+,\s+gray=(\d+)")
    syms = {}
    for ln in txt:
        m = pat.match(ln.strip())
        if not m:
            continue
        k = int(m.group(1)); g = int(m.group(2))
        if 0 <= k < 16:
            syms[k] = g
    if len(syms) >= 16:
        return [syms[i] for i in range(16)]
    return None

def parse_debug_raw_from_log(path):
    # Parse lines like "DEBUG: Header symbol k: raw=R, corr=.., gray=.."
    txt = Path(path).read_text(errors='ignore').splitlines()
    pat = re.compile(r"^DEBUG: Header symbol\s+(\d+):\s+raw=(\d+),\s+corr=\d+,\s+gray=\d+")
    syms = {}
    for ln in txt:
        m = pat.match(ln.strip())
        if not m:
            continue
        k = int(m.group(1)); r = int(m.group(2))
        if 0 <= k < 16:
            syms[k] = r
    if len(syms) >= 16:
        return [syms[i] for i in range(16)]
    return None

def gray_encode(x, bits=7):
    # generic Gray encode for 0..(2^bits-1)
    return (x ^ (x >> 1)) & ((1 << bits) - 1)

def rx_cw_from_gray(vals):
    # vals: list of 16 gray-coded header symbols (0..127)
    cw_all = []
    cw_len = 8
    for blk in range(2):
        inter_bin = [[0]*SF_APP for _ in range(cw_len)]
        for i in range(cw_len):
            full = vals[blk*cw_len + i] & (N-1)
            sub  = full & ((1 << SF_APP) - 1)  # use LSB sf_app bits (matches GR hard path int2bool)
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
    return cw_all[:10]

def rx_cw_with_block1_variants(vals):
    # Build CWs with GR mapping for block0; apply transforms to block1
    assert len(vals) >= 16
    cw_len = 8
    # Block0: GR mapping
    inter0 = [[0]*SF_APP for _ in range(cw_len)]
    for i in range(cw_len):
        sub = vals[i] & ((1 << SF_APP) - 1)
        for j in range(SF_APP):
            inter0[i][j] = (sub >> (SF_APP - 1 - j)) & 1
    deint0 = [[0]*cw_len for _ in range(SF_APP)]
    for i in range(cw_len):
        for j in range(SF_APP):
            r = (i - j - 1) % SF_APP
            deint0[r][i] = inter0[i][j]
    cw0 = []
    for r in range(SF_APP):
        c = 0
        for k in range(cw_len):
            c = (c << 1) | (deint0[r][k] & 1)
        cw0.append(c)
    # Block1 base inter
    inter1 = [[0]*SF_APP for _ in range(cw_len)]
    for i in range(cw_len):
        sub = vals[cw_len + i] & ((1 << SF_APP) - 1)
        for j in range(SF_APP):
            inter1[i][j] = (sub >> (SF_APP - 1 - j)) & 1
    res = []
    for diagshift in range(0, SF_APP):
        # build deint base via shifted diagonal r = (i - j - 1 + diagshift) mod sf_app
        base = [[0]*cw_len for _ in range(SF_APP)]
        for i in range(cw_len):
            for j in range(SF_APP):
                r = (i - j - 1 + diagshift) % SF_APP
                base[r][i] = inter1[i][j]
        for rot1 in range(0, SF_APP):
            # rotate rows
            rot = [[0]*cw_len for _ in range(SF_APP)]
            for r in range(SF_APP):
                rr = (r + rot1) % SF_APP
                rot[r] = base[rr][:]
            for rowrev in (0,1):
                if rowrev:
                    rr = [[0]*cw_len for _ in range(SF_APP)]
                    for r in range(SF_APP):
                        rr[r] = rot[SF_APP-1-r][:]
                    mat = rr
                else:
                    mat = rot
                for colrev in (0,1):
                    for colshift in range(0, 8):
                        # assemble cw1 rows under column shift/rev
                        cw1 = []
                        for r in range(SF_APP):
                            c = 0
                            for i in range(cw_len):
                                j = (cw_len-1-i if colrev else i)
                                jj = (j + colshift) & 7
                                c = (c << 1) | (mat[r][jj] & 1)
                            cw1.append(c)
                        cw = cw0 + cw1
                        res.append(((diagshift, rot1, rowrev, colrev, colshift), cw))
    return res

def main():
    scan_path = Path('logs/lite_hdr_scan.json')
    # If hdr-scan dump exists, analyze all windows
    if scan_path.exists():
        try:
            tgt = read_bytes('logs/gr_deint_bits.bin', 10)
        except FileNotFoundError:
            # Fallback target for the known SF7 reference vector
            tgt = [0x00, 0x74, 0xC5, 0x00, 0xC5, 0x1D, 0x12, 0x1B, 0x12, 0x00]
            print('Missing logs/gr_deint_bits.bin; using fallback target CWs for SF7 vector')
        scans = json.loads(scan_path.read_text())
        # Score all scans by timing-only diff
        scored_all = []
        for e in scans:
            raw = e.get('syms_raw', [])
            if len(raw) < 16:
                continue
            # Header path: reduce to gnu=(raw-1)>>2 (sf_app bits), then Gray-encode over sf_app bits
            gvals = [gray_encode(((r - 1) & (N - 1)) >> 2, bits=SF_APP) for r in raw]
            cw = rx_cw_from_gray(gvals)
            d = sum(1 for a,b in zip(cw, tgt) if a!=b)
            scored_all.append((d, e, cw, gvals))
        scored_all.sort(key=lambda x: x[0])
        best = scored_all[0]
        print('Best across hdr-scan (timing only): full_diff=', best[0])
        e, cw, gvals = best[1], best[2], best[3]
        print(' best params:', {k:e[k] for k in ('off0','samp0','off1','samp1')})
        print('   lite cw  :', ' '.join(f'{x:02x}' for x in cw))
        print('   gr   cw  :', ' '.join(f'{x:02x}' for x in tgt))

        # Try block1 variants for the top-K timing candidates and report global best
        TOPK = min(len(scored_all), 2048)
        global_best = None
        for d, e, cw, gvals in scored_all[:TOPK]:
            variants = rx_cw_with_block1_variants(gvals)
            for params, cwv in variants:
                full = sum(1 for a,b in zip(cwv, tgt) if a!=b)
                last5 = sum(1 for a,b in zip(cwv[5:], tgt[5:]) if a!=b)
                item = (full, last5, e, params, cwv)
                if (global_best is None) or (item[:2] < global_best[:2]):
                    global_best = item
        if global_best:
            full, last5, ebest, params, cwbest = global_best
            diagshift, rot1, rowrev, colrev, colshift = params
            print('Best with block1 variants across top-%d timings:' % TOPK)
            print(' params:', {k:ebest[k] for k in ('off0','samp0','off1','samp1')})
            print('  diagshift=%d rot1=%d rowrev=%d colrev=%d colshift=%d' % (diagshift, rot1, rowrev, colrev, colshift))
            print('  full_diff=%d last5_diff=%d' % (full, last5))
            print('  cwbytes:', ' '.join(f'{x:02x}' for x in cwbest))
        return 0
    # Else, evaluate last lite_ld.json / DEBUG
    raw = None
    lite = parse_last_json('logs/lite_ld.json')
    if lite and isinstance(lite, dict):
        dbg = lite.get('dbg_hdr') if isinstance(lite.get('dbg_hdr'), dict) else None
        if dbg and 'syms_raw' in dbg:
            try:
                raw = [int(t) for t in dbg['syms_raw'].split(',')][:16]
            except Exception:
                raw = None
    if raw is None:
        raw = parse_debug_raw_from_log('logs/lite_ld.json')
    vals = None
    if raw is not None:
        gvals = [gray_encode(((r - 1) & (N - 1)) >> 2, bits=SF_APP) for r in raw]
        cw = rx_cw_from_gray(gvals)
        vsrc = gvals
    else:
        # Fallback to syms_gray from logs (may be Gray(corr), less reliable)
        if lite and isinstance(lite, dict):
            dbg = lite.get('dbg_hdr') if isinstance(lite.get('dbg_hdr'), dict) else None
            if dbg and 'syms_gray' in dbg:
                try:
                    vals = [int(t) for t in dbg['syms_gray'].split(',')][:16]
                except Exception:
                    vals = None
        if vals is None:
            vals = parse_debug_gray_from_log('logs/lite_ld.json')
        if vals is None:
            print('Could not find header syms_raw/syms_gray in JSON or DEBUG; enable debug header printing.')
            return 3
        cw = rx_cw_from_gray(vals)
        vsrc = vals
        try:
            tgt = read_bytes('logs/gr_deint_bits.bin', 10)
        except FileNotFoundError:
            tgt = [0x00, 0x74, 0xC5, 0x00, 0xC5, 0x1D, 0x12, 0x1B, 0x12, 0x00]
            print('Missing logs/gr_deint_bits.bin; using fallback target CWs for SF7 vector')
    print('Lite cwbytes:', ' '.join(f'{x:02x}' for x in cw))
    if tgt:
        print(' GR  cwbytes:', ' '.join(f'{x:02x}' for x in tgt))
        diff = sum(1 for a,b in zip(cw,tgt) if a!=b)
        print(' byte mismatches:', diff)
    variants = rx_cw_with_block1_variants(vsrc)
    if tgt:
        scored = []
        for params, cwv in variants:
            full = sum(1 for a,b in zip(cwv, tgt) if a!=b)
            last5 = sum(1 for a,b in zip(cwv[5:], tgt[5:]) if a!=b)
            scored.append((full, last5, params, cwv))
        scored.sort(key=lambda x: (x[0], x[1]))
        best = scored[0]
        (full, last5, (diagshift, rot1, rowrev, colrev, colshift), cwbest) = best
        print('Best variant vs GR: full_diff=%d last5_diff=%d' % (full, last5))
        print(' params: diagshift=%d rot1=%d rowrev=%d colrev=%d colshift=%d' % (diagshift, rot1, rowrev, colrev, colshift))
        print('  cwbytes:', ' '.join(f'{x:02x}' for x in cwbest))
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
