#!/usr/bin/env python3
import argparse, sys, math
from pathlib import Path

def load_rows(csv: Path):
    rows = []
    if not csv.exists():
        return rows
    lines = csv.read_text().splitlines()
    for i, line in enumerate(lines):
        if not line.strip():
            continue
        if i == 0 and line.startswith('sf,'):
            continue
        cols = line.split(',')
        if len(cols) < 7:
            cols += [''] * (7 - len(cols))
        sf, cr, bw, ln, snr, ok, reason = cols[:7]
        try:
            rows.append({
                'sf': int(sf), 'cr': int(cr), 'bw': int(bw), 'len': int(ln), 'snr': float(snr),
                'ok': int(ok), 'reason': reason
            })
        except Exception:
            continue
    return rows

def summarize(rows):
    from collections import defaultdict
    agg = lambda: {'tot': 0, 'ok': 0}
    by_sfcr = defaultdict(agg)
    by_bw = defaultdict(agg)
    by_len = defaultdict(agg)
    by_snr = defaultdict(agg)
    reasons = defaultdict(int)
    for r in rows:
        key_sfcr = (r['sf'], r['cr'])
        by_sfcr[key_sfcr]['tot'] += 1
        by_sfcr[key_sfcr]['ok'] += r['ok']
        by_bw[r['bw']]['tot'] += 1
        by_bw[r['bw']]['ok'] += r['ok']
        by_len[r['len']]['tot'] += 1
        by_len[r['len']]['ok'] += r['ok']
        by_snr[r['snr']]['tot'] += 1
        by_snr[r['snr']]['ok'] += r['ok']
        if r['ok'] == 0:
            reasons[r['reason']] += 1
    total = len(rows)
    ok = sum(r['ok'] for r in rows)
    return {
        'total': total, 'ok': ok,
        'by_sfcr': dict(by_sfcr), 'by_bw': dict(by_bw), 'by_len': dict(by_len), 'by_snr': dict(by_snr),
        'reasons': dict(reasons)
    }

def write_readme(outdir: Path, summary: dict):
    rd = outdir / 'README.md'
    lines = []
    lines.append('# Original (hierarchical) TX→RX Validation')
    lines.append('')
    total = summary['total']
    ok = summary['ok']
    rate = 100.0 * ok / total if total else 0.0
    lines.append(f'- Cases: {total}')
    lines.append(f'- Success: {ok}/{total} ({rate:.1f}%)')
    lines.append('')
    # By SF/CR
    lines.append('## By SF/CR')
    for (sf, cr) in sorted(summary['by_sfcr'].keys(), key=lambda x: (int(x[0]), int(x[1]))):
        v = summary['by_sfcr'][(sf, cr)]
        r = 100.0 * v['ok'] / v['tot'] if v['tot'] else 0.0
        lines.append(f'- SF={sf} CR={cr}: {v["ok"]}/{v["tot"]} ({r:.1f}%)')
    # By BW
    lines.append('')
    lines.append('## By BW')
    for bw in sorted(summary['by_bw'].keys()):
        v = summary['by_bw'][bw]
        r = 100.0 * v['ok'] / v['tot'] if v['tot'] else 0.0
        lines.append(f'- BW={bw}: {v["ok"]}/{v["tot"]} ({r:.1f}%)')
    # By Length
    lines.append('')
    lines.append('## By Length')
    for ln in sorted(summary['by_len'].keys()):
        v = summary['by_len'][ln]
        r = 100.0 * v['ok'] / v['tot'] if v['tot'] else 0.0
        lines.append(f'- LEN={ln}: {v["ok"]}/{v["tot"]} ({r:.1f}%)')
    # By SNR
    lines.append('')
    lines.append('## By SNR')
    for snr in sorted(summary['by_snr'].keys()):
        v = summary['by_snr'][snr]
        r = 100.0 * v['ok'] / v['tot'] if v['tot'] else 0.0
        # format snr without trailing .0 when int
        snr_s = f'{snr:g}'
        lines.append(f'- SNR={snr_s}: {v["ok"]}/{v["tot"]} ({r:.1f}%)')
    # Reasons (if any)
    if summary['reasons']:
        lines.append('')
        lines.append('## Failure Reasons')
        for k, v in summary['reasons'].items():
            lines.append(f'- {k or "(none)"}: {v}')

    rd.write_text('\n'.join(lines) + '\n')

def plot_heatmaps(outdir: Path, rows):
    try:
        import numpy as np
        import matplotlib.pyplot as plt
    except Exception as e:
        print(f'[warn] plotting skipped: {e}', file=sys.stderr)
        return
    # Build SFxCR matrix of success rates aggregated over other dims
    sfs = sorted({r['sf'] for r in rows})
    crs = sorted({r['cr'] for r in rows})
    # Overall success rate heatmap per (sf,cr)
    mat = np.zeros((len(sfs), len(crs)))
    for i, sf in enumerate(sfs):
        for j, cr in enumerate(crs):
            subset = [r for r in rows if r['sf'] == sf and r['cr'] == cr]
            ok = sum(r['ok'] for r in subset)
            tot = len(subset)
            mat[i, j] = (ok / tot) if tot else 0.0
    fig, ax = plt.subplots(figsize=(8, 4))
    im = ax.imshow(mat, aspect='auto', vmin=0, vmax=1, cmap='Greens')
    ax.set_xticks(range(len(crs))); ax.set_xticklabels([str(c) for c in crs])
    ax.set_yticks(range(len(sfs))); ax.set_yticklabels([str(s) for s in sfs])
    ax.set_xlabel('CR'); ax.set_ylabel('SF'); ax.set_title('Success Rate by SF/CR')
    fig.colorbar(im, ax=ax)
    fig.tight_layout(); fig.savefig(outdir / 'success_rate_heatmap.png', dpi=150); plt.close(fig)

    # Per-BW heatmaps (aggregate over len,snr)
    bws = sorted({r['bw'] for r in rows})
    for bw in bws:
        mat = np.zeros((len(sfs), len(crs)))
        for i, sf in enumerate(sfs):
            for j, cr in enumerate(crs):
                subset = [r for r in rows if r['sf'] == sf and r['cr'] == cr and r['bw'] == bw]
                ok = sum(r['ok'] for r in subset)
                tot = len(subset)
                mat[i, j] = (ok / tot) if tot else 0.0
        fig, ax = plt.subplots(figsize=(8, 4))
        im = ax.imshow(mat, aspect='auto', vmin=0, vmax=1, cmap='Greens')
        ax.set_xticks(range(len(crs))); ax.set_xticklabels([str(c) for c in crs])
        ax.set_yticks(range(len(sfs))); ax.set_yticklabels([str(s) for s in sfs])
        ax.set_xlabel('CR'); ax.set_ylabel('SF'); ax.set_title(f'Success Rate by SF/CR (BW={bw})')
        fig.colorbar(im, ax=ax)
        fig.tight_layout(); fig.savefig(outdir / f'success_rate_heatmap_bw_{bw}.png', dpi=150); plt.close(fig)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--csv', required=True, help='Path to orig_validate.csv')
    ap.add_argument('--outdir', help='Output directory to write README/plots (default: CSV directory)')
    ap.add_argument('--plots', action='store_true', help='Generate heatmaps if matplotlib available')
    args = ap.parse_args()

    csv = Path(args.csv)
    outdir = Path(args.outdir) if args.outdir else csv.parent
    rows = load_rows(csv)
    if not rows:
        print(f'[error] no rows found in {csv}', file=sys.stderr)
        return 2
    summary = summarize(rows)
    write_readme(outdir, summary)
    if args.plots:
        plot_heatmaps(outdir, rows)
    print(f'Wrote summary to {outdir / "README.md"}', file=sys.stderr)
    return 0

if __name__ == '__main__':
    raise SystemExit(main())

