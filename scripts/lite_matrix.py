#!/usr/bin/env python3
import argparse, os, sys, time, json
from pathlib import Path

import random


def rand_bytes(n: int, seed: int) -> bytes:
    rnd = random.Random(seed)
    return bytes(rnd.randrange(0, 256) for _ in range(n))


def add_awgn_f32(iq_path: Path, snr_db: float, sr: int, out_path: Path) -> None:
    if snr_db == 0.0:
        data = iq_path.read_bytes()
        out_path.write_bytes(data)
        return
    import numpy as np
    a = np.fromfile(iq_path, dtype=np.float32)
    if a.size % 2 != 0:
        a = a[:-1]
    a = a.reshape(-1, 2)
    z = a[:,0].astype(np.float64) + 1j * a[:,1].astype(np.float64)
    sigp = np.mean(z.real**2 + z.imag**2)
    if sigp <= 0:
        sigp = 1.0
    snr_lin = 10.0 ** (snr_db / 10.0)
    np.random.seed(123)
    npw = sigp / snr_lin
    noise = (np.sqrt(npw/2.0) * (np.random.randn(z.size) + 1j*np.random.randn(z.size)))
    z = z + noise
    a2 = np.empty((z.size, 2), dtype=np.float32)
    a2[:,0] = z.real.astype(np.float32)
    a2[:,1] = z.imag.astype(np.float32)
    a2.tofile(out_path)


def run_case(build: Path, sf: int, cr: int, bw: int, length: int, snr_db: float, seed: int = 1000, timeout: float = 6.0, os_rate: int = 1):
    gen_lite = build / 'gen_lite_tx_iq'
    lora_decode = build / 'lora_decode'
    if not gen_lite.exists() or not lora_decode.exists():
        raise RuntimeError('Required binaries not built: gen_lite_tx_iq and lora_decode')

    # temp paths under build/bench_tmp
    tmpdir = build / 'bench_tmp'
    tmpdir.mkdir(parents=True, exist_ok=True)
    base = f'sf{sf}_cr{cr}_bw{bw}_len{length}_snr{int(snr_db)}_seed{seed}'
    payload_bin = tmpdir / f'{base}_payload.bin'
    iq_clean = tmpdir / f'{base}_iq_clean.bin'
    iq_ch = tmpdir / f'{base}_iq_ch.bin'

    payload = rand_bytes(length, seed)
    payload_bin.write_bytes(payload)
    # Generate clean IQ with public sync 0x34 for maximal interoperability
    import subprocess
    subprocess.run([str(gen_lite), '--sf', str(sf), '--cr', str(cr), '--payload', str(payload_bin), '--out', str(iq_clean), '--preamble', '8', '--sync', '0x34', '--os', str(os_rate)], check=True)
    # Add AWGN to achieve target SNR
    sr = 4 * bw  # aligns with original matrix notion of sr; OS=1 keeps file small; decoder auto-detects OS
    add_awgn_f32(iq_clean, snr_db, sr, iq_ch)

    # Decode via lora_decode JSON and compare payload
    t0 = time.perf_counter()
    res = subprocess.run([str(lora_decode), '--in', str(iq_ch), '--sf', str(sf), '--cr', str(cr), '--format', 'f32', '--json', '--sync', 'auto'], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    t1 = time.perf_counter()
    ok = 0; reason = 'no_output'
    try:
        if res.returncode in (0,3,4,5,6,7,8,9) and res.stdout.strip():
            j = json.loads(res.stdout)
            if j.get('success'):
                dec_hex = j.get('payload_hex','')
                ok = 1 if dec_hex == ''.join(f"{b:02x}" for b in payload) else 0
                reason = '' if ok else 'payload_mismatch'
            else:
                reason = j.get('reason') or 'decode_failed'
        else:
            reason = 'decoder_error'
    except Exception:
        reason = 'json_error'
    return ok, reason, t1 - t0


def write_header(csv: Path, expected_header: str):
    need_header = True
    if csv.exists() and csv.stat().st_size > 0:
        try:
            first = csv.read_text().splitlines()[0].strip()
            need_header = (first != expected_header)
        except Exception:
            pass
    with csv.open('a' if not need_header else 'w') as f:
        if need_header:
            f.write(expected_header + '\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default='reports_lite')
    ap.add_argument('--sfs', default='7,8,9,10,11,12')
    ap.add_argument('--crs', default='45,46,47,48')
    ap.add_argument('--bws', default='125000,250000,500000')
    ap.add_argument('--lengths', default='14,16,48')
    ap.add_argument('--reps', type=int, default=1)
    ap.add_argument('--timeout', type=float, default=6.0)
    ap.add_argument('--snrs', default='0', help='Comma-separated SNR values in dB (e.g., "0,5,10,15")')
    ap.add_argument('--os', type=int, default=1, help='Oversample used for TX IQ (1,2,4,8). Decoder auto-detects OS.')
    ap.add_argument('--quick', action='store_true', help='Run a small sanity subset (sf=7, cr=45, bw=125k, len=16)')
    args = ap.parse_args()

    root = Path(__file__).resolve().parents[1]
    build = root / 'build'
    outdir = build / args.out
    outdir.mkdir(parents=True, exist_ok=True)
    csv = outdir / 'lite_validate.csv'
    header = 'sf,cr,bw,len,snr,ok,reason'
    write_header(csv, header)

    if args.quick:
        sfs = [7]; crs = [45]; bws = [125000]; lens = [16]
    else:
        sfs = [int(x) for x in args.sfs.split(',') if x]
        crs = [int(x) for x in args.crs.split(',') if x]
        bws = [int(x) for x in args.bws.split(',') if x]
        lens= [int(x) for x in args.lengths.split(',') if x]
    snrs = [float(x) for x in args.snrs.split(',') if str(x).strip()!='']

    total = len(sfs)*len(crs)*len(bws)*len(lens)*max(1,len(snrs))*int(args.reps)
    print(f"[lite] starting matrix: {total} case(s)", flush=True)

    processed = 0
    try:
        for sf in sfs:
            for cr in crs:
                for bw in bws:
                    for ln in lens:
                        for snr in snrs or [0.0]:
                            for r in range(args.reps):
                                print(f"[lite] run sf={sf} cr={cr} bw={bw} len={ln} snr={snr} rep={r}", flush=True)
                                ok = 0; reason = ''
                                try:
                                    ok, reason, _ = run_case(build, sf, cr, bw, ln, snr, seed=(1000+r), timeout=args.timeout, os_rate=int(args.os))
                                except MemoryError:
                                    ok = 0; reason = 'alloc_failed'
                                except KeyboardInterrupt:
                                    ok = 0; reason = 'interrupted'
                                    with csv.open('a') as f:
                                        f.write(f"{sf},{cr},{bw},{ln},{snr},{ok},{reason}\n")
                                    processed += 1
                                    raise
                                except Exception:
                                    ok = 0; reason = 'exception'
                                with csv.open('a') as f:
                                    f.write(f"{sf},{cr},{bw},{ln},{snr},{ok},{reason}\n")
                                processed += 1
                                print(f"[lite] done sf={sf} cr={cr} bw={bw} len={ln} snr={snr} ok={bool(ok)}", flush=True)
                                time.sleep(0.01)
    except KeyboardInterrupt:
        print('[lite] interrupted by user', file=sys.stderr)
    finally:
        # write summary using summarize_matrix.py
        try:
            from summarize_matrix import load_rows, summarize, write_readme
        except Exception:
            sys.path.insert(0, str(Path(__file__).parent))
            from summarize_matrix import load_rows, summarize, write_readme
        rows = load_rows(csv)
        sm = summarize(rows)
        write_readme(outdir, sm)
    print(f"Done. CSV: {csv}", file=sys.stderr)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

