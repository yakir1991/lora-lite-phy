#!/usr/bin/env python3
import argparse, os, sys, json, time, random, subprocess
from pathlib import Path

def run(cmd, capture=False, env=None):
    if capture:
        return subprocess.run(cmd, check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    return subprocess.run(cmd, check=True, env=env)

def ensure_dir(p: Path):
    p.mkdir(parents=True, exist_ok=True)

def to_hex(b: bytes) -> str:
    return ''.join(f"{x:02x}" for x in b)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=str(Path(__file__).resolve().parents[1]))
    ap.add_argument('--build', default=str(Path(__file__).resolve().parents[1] / 'build'))
    ap.add_argument('--out', default='reports')
    ap.add_argument('--reps', type=int, default=int(os.environ.get('CROSS_REPS', '2')))
    ap.add_argument('--lengths', default=os.environ.get('CROSS_LENGTHS', '16,24,31,48'))
    ap.add_argument('--sfs', default=os.environ.get('CROSS_SFS', '7,8,9,10,11,12'))
    ap.add_argument('--crs', default=os.environ.get('CROSS_CRS', '45,46,47,48'))
    ap.add_argument('--seed', type=int, default=12345)
    args = ap.parse_args()

    root = Path(args.root)
    build = Path(args.build)
    outdir = build / args.out
    tmpdir = build / 'bench_tmp'
    ensure_dir(outdir)
    ensure_dir(tmpdir)

    # Check tools
    lora_decode = build / 'lora_decode'
    gen_tx = root / 'scripts' / 'gr_tx_pdu_vectors.py'
    if not lora_decode.exists():
        print('[error] lora_decode not built. Build first.', file=sys.stderr)
        return 2
    if not gen_tx.exists():
        print('[error] gr_tx_pdu_vectors.py missing.', file=sys.stderr)
        return 2
    # Check GNURadio
    try:
        import gnuradio, gnuradio.lora_sdr  # type: ignore
    except Exception as e:
        print(f'[error] GNU Radio LoRa SDR not available: {e}', file=sys.stderr)
        return 2

    rnd = random.Random(args.seed)
    sfs = [int(x) for x in args.sfs.split(',') if x]
    crs = [int(x) for x in args.crs.split(',') if x]
    lens = [int(x) for x in args.lengths.split(',') if x]

    csv_path = outdir / 'cross_validate.csv'
    with csv_path.open('w') as f:
        f.write('sf,cr,len,ok,reason,t_encode_s,t_decode_s,mem_kb,detect_os,detect_phase,detect_start\n')

    for sf in sfs:
        for cr in crs:
            for ln in lens:
                for r in range(args.reps):
                    payload = bytes(rnd.randrange(0, 256) for _ in range(ln))
                    pfile = tmpdir / f'sf{sf}_cr{cr}_len{ln}_rep{r}.bin'
                    iqfile = tmpdir / f'sf{sf}_cr{cr}_len{ln}_rep{r}_iq_os4.bin'
                    pfile.write_bytes(payload)
                    # Generate IQ via GNU Radio (OS4)
                    t0 = time.perf_counter()
                    run(['python3', str(gen_tx), '--sf', str(sf), '--cr', str(cr), '--payload', str(pfile), '--out', str(iqfile), '--bw', '125000', '--samp-rate', '500000', '--preamble-len', '8', '--timeout', '30'])
                    t1 = time.perf_counter()
                    # Decode via lora_decode JSON
                    t2 = time.perf_counter()
                    cmd = ['/usr/bin/time','-v', str(lora_decode), '--in', str(iqfile), '--sf', str(sf), '--cr', str(cr), '--format', 'f32', '--json', '--sync', 'auto']
                    res = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                    if res.returncode not in (0,3,4,5,6,7,8,9):
                        raise RuntimeError(f'lora_decode unexpected exit {res.returncode}: {res.stderr}')
                    t3 = time.perf_counter()
                    mem_kb = 0
                    for line in res.stderr.splitlines():
                        if 'Maximum resident set size' in line:
                            try:
                                mem_kb = int(line.split(':')[-1].strip())
                            except Exception:
                                mem_kb = 0
                            break
                    j = json.loads(res.stdout)
                    ok = bool(j.get('success'))
                    reason = j.get('reason') if j.get('reason') else ''
                    dec_hex = j.get('payload_hex', '')
                    if ok:
                        ok = (dec_hex == to_hex(payload))
                        if not ok:
                            reason = 'payload_mismatch'
                    detect_os = j.get('detect_os', -1)
                    detect_phase = j.get('detect_phase', -1)
                    detect_start = j.get('detect_start', 0)
                    with csv_path.open('a') as f:
                        f.write(f"{sf},{cr},{ln},{1 if ok else 0},{reason},{t1-t0:.6f},{t3-t2:.6f},{mem_kb},{detect_os},{detect_phase},{detect_start}\n")

    print(f'Done. CSV: {csv_path}', file=sys.stderr)

    # Optional plotting
    try:
        import numpy as np
        import matplotlib.pyplot as plt
        import csv
        data = []
        with (outdir / 'cross_validate.csv').open('r') as f:
            rd = csv.DictReader(f)
            for row in rd:
                data.append(row)
        if data:
            s_set = sorted({int(r['sf']) for r in data})
            c_set = sorted({int(r['cr']) for r in data})
            si = {s:i for i,s in enumerate(s_set)}
            ci = {c:i for i,c in enumerate(c_set)}
            import numpy as np
            acc_ok = np.zeros((len(s_set), len(c_set)))
            cnt = np.zeros_like(acc_ok)
            acc_t = np.zeros_like(acc_ok, dtype=float)
            for r in data:
                i = si[int(r['sf'])]; j = ci[int(r['cr'])]
                ok = int(r['ok']); acc_ok[i,j] += ok; cnt[i,j] += 1
                acc_t[i,j] += float(r['t_decode_s'])
            succ = np.divide(acc_ok, cnt, out=np.zeros_like(acc_ok), where=cnt>0)
            avg_t = np.divide(acc_t, cnt, out=np.zeros_like(acc_t), where=cnt>0)
            # Success rate heatmap
            fig, ax = plt.subplots(figsize=(8,4)); im=ax.imshow(succ, aspect='auto', vmin=0, vmax=1, cmap='Greens')
            ax.set_xticks(range(len(c_set))); ax.set_xticklabels([str(x) for x in c_set])
            ax.set_yticks(range(len(s_set))); ax.set_yticklabels([str(x) for x in s_set])
            ax.set_xlabel('CR'); ax.set_ylabel('SF'); ax.set_title('Success rate')
            fig.colorbar(im, ax=ax); fig.tight_layout(); fig.savefig(outdir / 'success_rate_heatmap.png', dpi=150); plt.close(fig)
            # Decode time heatmap
            fig, ax = plt.subplots(figsize=(8,4)); im=ax.imshow(avg_t, aspect='auto', cmap='viridis')
            ax.set_xticks(range(len(c_set))); ax.set_xticklabels([str(x) for x in c_set])
            ax.set_yticks(range(len(s_set))); ax.set_yticklabels([str(x) for x in s_set])
            ax.set_xlabel('CR'); ax.set_ylabel('SF'); ax.set_title('Avg decode time (s)')
            fig.colorbar(im, ax=ax); fig.tight_layout(); fig.savefig(outdir / 'decode_time_heatmap.png', dpi=150); plt.close(fig)
    except Exception as e:
        print(f'[warn] plotting skipped: {e}', file=sys.stderr)
    return 0

if __name__ == '__main__':
    sys.exit(main())

