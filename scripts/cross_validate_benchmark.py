#!/usr/bin/env python3
import argparse, os, sys, json, time, random, subprocess, textwrap
from pathlib import Path


def run(cmd, capture=False, env=None, check=True):
    if capture:
        return subprocess.run(cmd, check=check, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    return subprocess.run(cmd, check=check, env=env)


def ensure_dir(p: Path):
    p.mkdir(parents=True, exist_ok=True)


def to_hex(b: bytes) -> str:
    return ''.join(f"{x:02x}" for x in b)


def write_readme_report(outdir: Path, csv_path: Path):
    try:
        import csv
        import statistics as stats
        rows = []
        with csv_path.open('r') as f:
            rd = csv.DictReader(f)
            for r in rd:
                rows.append(r)
        if not rows:
            return
        # Aggregate key metrics
        total = len(rows)
        ok_lite = sum(1 for r in rows if r['ok_lite'] == '1')
        ok_orig = sum(1 for r in rows if r['ok_orig'] == '1')
        avg_t_lite = stats.mean(float(r['t_decode_lite_s']) for r in rows)
        avg_t_orig = stats.mean(float(r.get('t_decode_orig_s', '0') or '0') for r in rows)
        # Breakdown by tx_src if present
        def filt(src):
            return [r for r in rows if r.get('tx_src', 'lite') == src]
        lite_rows = filt('lite')
        orig_rows = filt('orig')
        def succ(rows_, key):
            return (sum(1 for r in rows_ if r.get(key,'0') == '1'), len(rows_)) if rows_ else (0,0)
        lite_on_lite = succ(lite_rows, 'ok_lite')
        orig_on_lite = succ(lite_rows, 'ok_orig')
        lite_on_orig = succ(orig_rows, 'ok_lite')
        orig_on_orig = succ(orig_rows, 'ok_orig')
        # Minimal concise report
        md = [
            '# Cross-Validation Report: LoRa Lite vs Original LoRa',
            '',
            f'- Date: {time.strftime("%Y-%m-%d %H:%M:%S")}',
            f'- Cases: {total}',
            f'- LoRa Lite success: {ok_lite}/{total} ({100.0*ok_lite/total:.1f}%)',
            f'- Original LoRa success: {ok_orig}/{total} ({100.0*ok_orig/total:.1f}%)',
            f'- Avg decode time LoRa Lite: {avg_t_lite:.4f} s',
            f'- Avg decode time Original: {avg_t_orig:.4f} s',
            '',
            '## Breakdown (TX×RX)',
            f"- lite→lite: {lite_on_lite[0]}/{lite_on_lite[1]}" + (f" ({100.0*lite_on_lite[0]/lite_on_lite[1]:.1f}%)" if lite_on_lite[1] else ''),
            f"- lite→orig: {orig_on_lite[0]}/{orig_on_lite[1]}" + (f" ({100.0*orig_on_lite[0]/orig_on_lite[1]:.1f}%)" if orig_on_lite[1] else ''),
            f"- orig→lite: {lite_on_orig[0]}/{lite_on_orig[1]}" + (f" ({100.0*lite_on_orig[0]/lite_on_orig[1]:.1f}%)" if lite_on_orig[1] else ''),
            f"- orig→orig: {orig_on_orig[0]}/{orig_on_orig[1]}" + (f" ({100.0*orig_on_orig[0]/orig_on_orig[1]:.1f}%)" if orig_on_orig[1] else ''),
            '',
            '## Heatmaps',
            '- `success_rate_heatmap.png`: LoRa Lite success rate by SF/CR',
            '- `decode_time_heatmap.png`: LoRa Lite avg decode time by SF/CR',
            '',
            '## CSV Columns',
            textwrap.dedent('''
            - sf: spreading factor
            - cr: coding rate (45..48)
            - bw: bandwidth (Hz)
            - len: payload length (bytes)
            - snr_db: AWGN SNR (dB)
            - cfo_hz: carrier frequency offset (Hz)
            - sto_smpl: integer sample shift applied (Lite only)
            - ok_lite, reason_lite: Lite decoder result
            - ok_orig, reason_orig: Original decoder result
            - t_txrx_s: time to generate+channel+orig RX
            - t_decode_lite_s, t_decode_orig_s: decode times
            - mem_lite_kb, mem_orig_kb: peak RSS (approx)
            - detect_os, detect_phase, detect_start: Lite preamble detection diagnostics
            '''),
        ]
        (outdir / 'README.md').write_text('\n'.join(md))
    except Exception as e:
        print(f'[warn] README generation skipped: {e}', file=sys.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=str(Path(__file__).resolve().parents[1]))
    ap.add_argument('--build', default=str(Path(__file__).resolve().parents[1] / 'build'))
    ap.add_argument('--out', default='reports')
    ap.add_argument('--reps', type=int, default=int(os.environ.get('CROSS_REPS', '2')))
    ap.add_argument('--lengths', default=os.environ.get('CROSS_LENGTHS', '16,24,31,48'))
    ap.add_argument('--sfs', default=os.environ.get('CROSS_SFS', '7,8,9,10,11,12'))
    ap.add_argument('--crs', default=os.environ.get('CROSS_CRS', '45,46,47,48'))
    ap.add_argument('--bws', default=os.environ.get('CROSS_BWS', '7800,10400,15600,20800,31250,41700,62500,125000,250000,500000'))
    ap.add_argument('--snrs', default=os.environ.get('CROSS_SNRS', '0,5,10,15'))  # dB values
    ap.add_argument('--cfo-hz', default=os.environ.get('CROSS_CFO_HZ', '0,100,500'))  # Hz list
    ap.add_argument('--sto', default=os.environ.get('CROSS_STO', '0'))  # integer sample shift list e.g. '0,16,-16'
    ap.add_argument('--clk-ppm', default=os.environ.get('CROSS_CLK_PPM', '0'))  # List of PPM for clock offset
    ap.add_argument('--seed', type=int, default=12345)
    ap.add_argument('--timeout', type=float, default=20.0)
    ap.add_argument('--lite-tx', action='store_true', default=(os.environ.get('CROSS_LITE_TX','1')=='1'), help='Use built-in LoRa Lite TX generator for IQ')
    ap.add_argument('--compare-both', action='store_true', default=(os.environ.get('CROSS_COMPARE_BOTH','0')=='1'), help='Generate both Lite TX and Original TX and decode with both RX')
    args = ap.parse_args()

    root = Path(args.root)
    build = Path(args.build)
    outdir = build / args.out
    tmpdir = build / 'bench_tmp'
    ensure_dir(outdir)
    ensure_dir(tmpdir)

    # Check tools
    lora_decode = build / 'lora_decode'
    gr_orig_rx = root / 'scripts' / 'gr_original_rx_only.py'
    gen_lite = build / 'gen_lite_tx_iq'
    gen_tx = root / 'scripts' / 'gr_tx_pdu_vectors.py'
    if not lora_decode.exists():
        print('[error] lora_decode not built. Build first (cmake && make).', file=sys.stderr)
        return 2
    if not gr_orig_rx.exists():
        print('[error] gr_original_rx_only.py missing. Please ensure scripts are in place.', file=sys.stderr)
        return 2
    if args.lite_tx and not gen_lite.exists():
        print('[error] gen_lite_tx_iq not built. Run cmake --build build.', file=sys.stderr)
        return 2
    if not gen_tx.exists():
        print('[error] gr_tx_pdu_vectors.py missing.', file=sys.stderr)
        return 2
    # Check GNURadio availability early
    try:
        import gnuradio, gnuradio.lora_sdr  # type: ignore
    except Exception as e:
        print(f'[error] GNU Radio LoRa SDR not available: {e}', file=sys.stderr)
        return 2

    rnd = random.Random(args.seed)
    sfs = [int(x) for x in args.sfs.split(',') if x]
    crs = [int(x) for x in args.crs.split(',') if x]
    lens = [int(x) for x in args.lengths.split(',') if x]
    bws = [int(float(x)) for x in args.bws.split(',') if x]
    snrs = [float(x) for x in args.snrs.split(',') if x]
    cfo_list = [float(x) for x in args.cfo_hz.split(',') if x]
    sto_list = [int(x) for x in args.sto.split(',') if x]
    clk_ppm_list = [float(x) for x in args.clk_ppm.split(',') if x]

    csv_path = outdir / 'cross_validate.csv'
    with csv_path.open('w') as f:
        f.write('tx_src,sf,cr,bw,len,snr_db,cfo_hz,sto_smpl,clk_ppm,'
                'ok_lite,reason_lite,ok_orig,reason_orig,'
                't_txrx_s,t_decode_lite_s,t_decode_orig_s,'
                'mem_lite_kb,mem_orig_kb,detect_os,detect_phase,detect_start\n')

    # Iterate sweeps
    for sf in sfs:
        for cr in crs:
            for bw in bws:
                for ln in lens:
                    for snr in snrs or [0.0]:
                        for cfo_hz in cfo_list or [0.0]:
                            for clk_ppm in clk_ppm_list or [0.0]:
                                for sto in sto_list or [0]:
                                    for r in range(args.reps):
                                        payload = bytes(rnd.randrange(0, 256) for _ in range(ln))
                                        base = f'sf{sf}_cr{cr}_bw{bw}_len{ln}_snr{int(snr)}_cfo{int(cfo_hz)}_ppm{int(clk_ppm)}_sto{sto}_rep{r}'
                                        pfile = tmpdir / f'{base}_payload.bin'
                                        iq_clean = tmpdir / f'{base}_iq_clean.bin'
                                        iq_ch_file = tmpdir / f'{base}_iq_ch.bin'
                                        iq_lite_in = iq_ch_file  # may change if STO applied
                                        rx_payload = tmpdir / f'{base}_rx_payload_orig.bin'
                                        pfile.write_bytes(payload)

                                        # Generate clean IQ (Lite TX or Original TX)
                                        sr = 4 * bw  # default oversample 4x
                                        t0 = time.perf_counter()
                                        if args.lite_tx:
                                            # Use LoRa Lite TX generator with public sync 0x34 (OS=1 for maximal compatibility)
                                            run([str(gen_lite), '--sf', str(sf), '--cr', str(cr), '--payload', str(pfile), '--out', str(iq_clean), '--preamble', '8', '--sync', '0x34', '--os', '1'])
                                        else:
                                            run(['python3', str(gen_tx), '--sf', str(sf), '--cr', str(cr), '--payload', str(pfile), '--out', str(iq_clean), '--bw', str(bw), '--samp-rate', str(sr), '--preamble-len', '8', '--timeout', str(args.timeout)])
                                        # Apply impairments in numpy to produce channel IQ
                                        try:
                                            import numpy as np
                                            a = np.fromfile(iq_clean, dtype=np.float32)
                                            a = a.reshape(-1, 2)
                                            z = a[:,0] + 1j*a[:,1]
                                            if cfo_hz != 0.0:
                                                n = np.arange(z.size, dtype=np.float64)
                                                z *= np.exp(1j * 2.0*np.pi * (cfo_hz/float(sr)) * n)
                                            if clk_ppm != 0.0:
                                                # Resample via simple stretch (nearest neighbor) for small ppm values
                                                ppm = clk_ppm * 1e-6
                                                idx = (np.arange(z.size) * (1.0 + ppm)).astype(np.int64)
                                                idx = np.clip(idx, 0, z.size-1)
                                                z = z[idx]
                                            if snr != 0.0:
                                                sigp = np.mean((z.real**2 + z.imag**2))
                                                if sigp <= 0:
                                                    sigp = 1.0
                                                snr_lin = 10.0**(snr/10.0)
                                                np.random.seed(123)
                                                npw = sigp/snr_lin
                                                z += (np.sqrt(npw/2.0) * (np.random.randn(z.size) + 1j*np.random.randn(z.size)))
                                            a2 = np.empty((z.size,2), dtype=np.float32)
                                            a2[:,0] = z.real.astype(np.float32)
                                            a2[:,1] = z.imag.astype(np.float32)
                                            a2.tofile(iq_ch_file)
                                        except Exception as e:
                                            print(f'[warn] impairments failed: {e}', file=sys.stderr)
                                            iq_ch_file = iq_clean
                                        t1 = time.perf_counter()

                                        # Run original RX-only on the channel IQ and compare payload
                                        ok_orig = 0
                                        reason_orig = ''
                                        mem_orig_kb = 0
                                        try:
                                            cmd_orig = ['/usr/bin/time','-v','python3', str(gr_orig_rx), '--in-iq', str(iq_ch_file), '--sf', str(sf), '--cr', str(cr), '--bw', str(bw), '--samp-rate', str(sr), '--pay-len', str(ln), '--out-rx-payload', str(rx_payload), '--timeout', str(args.timeout)]
                                            res_orig = subprocess.run(cmd_orig, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                                            for line in res_orig.stderr.splitlines():
                                                if 'Maximum resident set size' in line:
                                                    try:
                                                        mem_orig_kb = int(line.split(':')[-1].strip())
                                                    except Exception:
                                                        mem_orig_kb = 0
                                                    break
                                            rx_bytes = b''
                                            try:
                                                if rx_payload.exists():
                                                    rx_bytes = rx_payload.read_bytes()
                                            except Exception:
                                                rx_bytes = b''
                                            ok_orig = 1 if (rx_bytes == payload) else 0
                                            reason_orig = '' if ok_orig else ('no_output' if not rx_bytes else 'payload_mismatch')
                                        except Exception as e:
                                            reason_orig = f'orig_exception:{e}'

                                        # Apply integer sample shift to Lite input if requested
                                        if sto != 0:
                                            iq_shifted = tmpdir / f'{base}_iq_ch_sto.bin'
                                            try:
                                                import numpy as np
                                                a = np.fromfile(iq_ch_file, dtype=np.float32)
                                                if a.size % 2 != 0:
                                                    a = a[:-1]
                                                a = a.reshape(-1, 2)
                                                if sto > 0:
                                                    a = a[sto:, :]
                                                else:
                                                    pad = np.zeros((-sto, 2), dtype=np.float32)
                                                    a = np.vstack([pad, a])
                                                a.astype(np.float32).tofile(iq_shifted)
                                                iq_lite_in = iq_shifted
                                            except Exception as e:
                                                print(f'[warn] STO shift failed: {e}', file=sys.stderr)
                                                iq_lite_in = iq_ch_file

                                        # If channel IQ is empty (orig RX failed), fall back to clean TX then apply impairments
                                        try:
                                            if not iq_ch_file.exists() or iq_ch_file.stat().st_size == 0:
                                                iq_tmp = tmpdir / f'{base}_iq_tx.bin'
                                                run(['python3', str(gen_tx), '--sf', str(sf), '--cr', str(cr), '--payload', str(pfile), '--out', str(iq_tmp), '--bw', str(bw), '--samp-rate', str(sr), '--preamble-len', '8', '--timeout', str(args.timeout)])
                                                # Apply CFO + AWGN in numpy
                                                import numpy as np
                                                a = np.fromfile(iq_tmp, dtype=np.float32)
                                                a = a.reshape(-1, 2)
                                                z = a[:,0] + 1j*a[:,1]
                                                if cfo_hz != 0.0:
                                                    n = np.arange(z.size, dtype=np.float64)
                                                    z *= np.exp(1j * 2.0*np.pi * (cfo_hz/float(sr)) * n)
                                                if snr != 0.0:
                                                    sigp = np.mean((z.real**2 + z.imag**2))
                                                    if sigp <= 0:
                                                        sigp = 1.0
                                                    snr_lin = 10.0**(snr/10.0)
                                                    np.random.seed(123)
                                                    npw = sigp/snr_lin
                                                    z += (np.sqrt(npw/2.0) * (np.random.randn(z.size) + 1j*np.random.randn(z.size)))
                                                a2 = np.empty((z.size,2), dtype=np.float32)
                                                a2[:,0] = z.real.astype(np.float32)
                                                a2[:,1] = z.imag.astype(np.float32)
                                                a2.tofile(iq_ch_file)
                                        except Exception as e:
                                            print(f'[warn] fallback impairments failed: {e}', file=sys.stderr)

                                        # Decode via lora_decode JSON (LoRa Lite)
                                        t2 = time.perf_counter()
                                        cmd_lite = ['/usr/bin/time','-v', str(lora_decode), '--in', str(iq_lite_in), '--sf', str(sf), '--cr', str(cr), '--format', 'f32', '--json', '--sync', 'auto']
                                        res_lite = subprocess.run(cmd_lite, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                                        t3 = time.perf_counter()
                                        mem_lite_kb = 0
                                        for line in res_lite.stderr.splitlines():
                                            if 'Maximum resident set size' in line:
                                                try:
                                                    mem_lite_kb = int(line.split(':')[-1].strip())
                                                except Exception:
                                                    mem_lite_kb = 0
                                                break
                                        # Handle cases where JSON may not be produced (e.g., early read failure)
                                        j = {}
                                        if res_lite.returncode in (0,3,4,5,6,7,8,9) and res_lite.stdout.strip():
                                            try:
                                                j = json.loads(res_lite.stdout)
                                            except Exception:
                                                j = {}
                                        ok_lite = 1 if j.get('success') else 0
                                        reason_lite = j.get('reason') if j.get('reason') else ''
                                        dec_hex = j.get('payload_hex', '') if j else ''
                                        if ok_lite:
                                            ok_lite = 1 if (dec_hex == to_hex(payload)) else 0
                                            if not ok_lite:
                                                reason_lite = 'payload_mismatch'
                                        detect_os = j.get('detect_os', -1)
                                        detect_phase = j.get('detect_phase', -1)
                                        detect_start = j.get('detect_start', 0)

                                        with csv_path.open('a') as f:
                                            f.write(
                                                f"lite,{sf},{cr},{bw},{ln},{snr},{cfo_hz},{sto},{clk_ppm},"
                                                f"{ok_lite},{reason_lite},{ok_orig},{reason_orig},"
                                                f"{t1-t0:.6f},{t3-t2:.6f},0.0000,{mem_lite_kb},{mem_orig_kb},{detect_os},{detect_phase},{detect_start}\n"
                                            )

                                        # Optionally also run Original TX path
                                        if args.compare_both:
                                            iq_clean2 = tmpdir / f'{base}_iq_clean_origtx.bin'
                                            iq_ch2 = tmpdir / f'{base}_iq_ch_origtx.bin'
                                            rx_payload2 = tmpdir / f'{base}_rx_payload_origtx.bin'
                                            t0b = time.perf_counter()
                                            run(['python3', str(gen_tx), '--sf', str(sf), '--cr', str(cr), '--payload', str(pfile), '--out', str(iq_clean2), '--bw', str(bw), '--samp-rate', str(sr), '--preamble-len', '8', '--timeout', str(args.timeout)])
                                            try:
                                                import numpy as np
                                                a = np.fromfile(iq_clean2, dtype=np.float32).reshape(-1,2)
                                                z = a[:,0] + 1j*a[:,1]
                                                if cfo_hz != 0.0:
                                                    n = np.arange(z.size, dtype=np.float64)
                                                    z *= np.exp(1j*2.0*np.pi*(cfo_hz/float(sr))*n)
                                                if snr != 0.0:
                                                    sigp = np.mean((z.real**2 + z.imag**2)) or 1.0
                                                    snr_lin = 10.0**(snr/10.0)
                                                    np.random.seed(123)
                                                    npw = sigp/snr_lin
                                                    z += (np.sqrt(npw/2.0) * (np.random.randn(z.size) + 1j*np.random.randn(z.size)))
                                                a2 = np.empty((z.size,2), dtype=np.float32); a2[:,0]=z.real.astype(np.float32); a2[:,1]=z.imag.astype(np.float32)
                                                a2.tofile(iq_ch2)
                                            except Exception as e:
                                                print(f'[warn] impairments(origtx) failed: {e}', file=sys.stderr)
                                                iq_ch2 = iq_clean2
                                            t1b = time.perf_counter()
                                            # Lite decoder on Original TX
                                            t2b = time.perf_counter()
                                            res_lite2 = subprocess.run(['/usr/bin/time','-v', str(lora_decode), '--in', str(iq_ch2), '--sf', str(sf), '--cr', str(cr), '--format', 'f32', '--json', '--sync', 'auto'], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                                            t3b = time.perf_counter()
                                            mem_lite2 = 0
                                            for line in res_lite2.stderr.splitlines():
                                                if 'Maximum resident set size' in line:
                                                    try: mem_lite2 = int(line.split(':')[-1].strip())
                                                    except Exception: mem_lite2 = 0
                                                    break
                                            j2 = {}
                                            if res_lite2.returncode in (0,3,4,5,6,7,8,9) and res_lite2.stdout.strip():
                                                try: j2 = json.loads(res_lite2.stdout)
                                                except Exception: j2 = {}
                                            ok_lite2 = 1 if j2.get('success') and j2.get('payload_hex','') == to_hex(payload) else 0
                                            reason_lite2 = '' if ok_lite2 else (j2.get('reason','') or 'payload_mismatch')
                                            # Original RX on Original TX
                                            ok_orig2 = 0; reason_orig2 = ''; mem_orig2 = 0
                                            try:
                                                res_orig2 = subprocess.run(['/usr/bin/time','-v','python3', str(gr_orig_rx), '--in-iq', str(iq_ch2), '--sf', str(sf), '--cr', str(cr), '--bw', str(bw), '--samp-rate', str(sr), '--pay-len', str(ln), '--out-rx-payload', str(rx_payload2), '--timeout', str(args.timeout)], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                                                for line in res_orig2.stderr.splitlines():
                                                    if 'Maximum resident set size' in line:
                                                        try: mem_orig2 = int(line.split(':')[-1].strip())
                                                        except Exception: mem_orig2 = 0
                                                        break
                                                rx2 = b''
                                                try:
                                                    if rx_payload2.exists(): rx2 = rx_payload2.read_bytes()
                                                except Exception: rx2 = b''
                                                ok_orig2 = 1 if rx2 == payload else 0
                                                reason_orig2 = '' if ok_orig2 else ('no_output' if not rx2 else 'payload_mismatch')
                                            except Exception as e:
                                                reason_orig2 = f'orig_exception:{e}'

                                            with csv_path.open('a') as f:
                                                f.write(
                                                    f"orig,{sf},{cr},{bw},{ln},{snr},{cfo_hz},{sto},{clk_ppm},"
                                                    f"{ok_lite2},{reason_lite2},{ok_orig2},{reason_orig2},"
                                                    f"{t1b-t0b:.6f},{t3b-t2b:.6f},0.0000,{mem_lite2},{mem_orig2},{j2.get('detect_os',-1)},{j2.get('detect_phase',-1)},{j2.get('detect_start',0)}\n"
                                                )

    print(f'Done. CSV: {csv_path}', file=sys.stderr)

    # Optional plotting (LoRa Lite focused heatmaps)
    try:
        import numpy as np
        import matplotlib.pyplot as plt
        import csv
        data = []
        with (csv_path).open('r') as f:
            rd = csv.DictReader(f)
            for row in rd:
                data.append(row)
        if data:
            # Focus heatmaps on LoRa Lite results from lite TX only
            data_lite = [r for r in data if r.get('tx_src','lite') == 'lite']
            s_set = sorted({int(r['sf']) for r in data_lite})
            c_set = sorted({int(r['cr']) for r in data_lite})
            si = {s:i for i,s in enumerate(s_set)}
            ci = {c:i for i,c in enumerate(c_set)}
            acc_ok = np.zeros((len(s_set), len(c_set)))
            cnt = np.zeros_like(acc_ok)
            acc_t = np.zeros_like(acc_ok, dtype=float)
            for r in data_lite:
                i = si[int(r['sf'])]; j = ci[int(r['cr'])]
                ok = int(r['ok_lite']); acc_ok[i,j] += ok; cnt[i,j] += 1
                acc_t[i,j] += float(r['t_decode_lite_s'])
            succ = np.divide(acc_ok, cnt, out=np.zeros_like(acc_ok), where=cnt>0)
            avg_t = np.divide(acc_t, cnt, out=np.zeros_like(acc_t), where=cnt>0)
            # Success rate heatmap
            fig, ax = plt.subplots(figsize=(8,4)); im=ax.imshow(succ, aspect='auto', vmin=0, vmax=1, cmap='Greens')
            ax.set_xticks(range(len(c_set))); ax.set_xticklabels([str(x) for x in c_set])
            ax.set_yticks(range(len(s_set))); ax.set_yticklabels([str(x) for x in s_set])
            ax.set_xlabel('CR'); ax.set_ylabel('SF'); ax.set_title('LoRa Lite success rate')
            fig.colorbar(im, ax=ax); fig.tight_layout(); fig.savefig(outdir / 'success_rate_heatmap.png', dpi=150); plt.close(fig)
            # Decode time heatmap
            fig, ax = plt.subplots(figsize=(8,4)); im=ax.imshow(avg_t, aspect='auto', cmap='viridis')
            ax.set_xticks(range(len(c_set))); ax.set_xticklabels([str(x) for x in c_set])
            ax.set_yticks(range(len(s_set))); ax.set_yticklabels([str(x) for x in s_set])
            ax.set_xlabel('CR'); ax.set_ylabel('SF'); ax.set_title('LoRa Lite avg decode time (s)')
            fig.colorbar(im, ax=ax); fig.tight_layout(); fig.savefig(outdir / 'decode_time_heatmap.png', dpi=150); plt.close(fig)
    except Exception as e:
        print(f'[warn] plotting skipped: {e}', file=sys.stderr)

    # Generate concise README.md report
    write_readme_report(outdir, csv_path)
    return 0


if __name__ == '__main__':
    sys.exit(main())
