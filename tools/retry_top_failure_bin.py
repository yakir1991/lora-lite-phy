#!/usr/bin/env python3
"""Retry the worst failure bin with wider CFO sweep and report deltas.

Find the (sf,bw,cr,snr_bin) group with the highest number of failures in a
results JSON, rerun only the failing vectors in that bin with:
  --hdr-cfo-sweep --hdr-cfo-range 300 --hdr-cfo-step 25
Compare before/after counts and print a small delta report.
"""
from __future__ import annotations

import argparse
import json
import subprocess
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Any, List, Tuple

ROOT = Path(__file__).resolve().parents[1]
RESULTS_JSON = ROOT / 'results' / 'streaming_compat_results.json'
CPP_CANDIDATES = [
    ROOT / 'cpp_receiver/build/decode_cli',
    ROOT / 'cpp_receiver/build/Release/decode_cli',
]

@dataclass
class Meta:
    sf: int
    bw: int
    fs: int
    cr: int
    crc: bool
    impl: bool
    ldro: int
    sync_word: int
    payload_len: int


def resolve_cpp() -> Path | None:
    for p in CPP_CANDIDATES:
        if p.exists():
            return p
    return None


def snr_bin(v: float) -> str:
    edges = [-999, -20, -15, -10, -5, 0, 5, 10, 999]
    labels = ["<-20", "[-20,-15)", "[-15,-10)", "[-10,-5)", "[-5,0)", "[0,5)", "[5,10)", ">=10"]
    for i in range(len(edges)-1):
        if edges[i] <= v < edges[i+1]:
            return labels[i]
    return labels[-1]


def pick_top_failure_bin(results: Dict[str, Any]) -> Tuple[Tuple[int,int,int,str], List[Dict[str, Any]]]:
    groups: Dict[Tuple[int,int,int,str], Dict[str, Any]] = defaultdict(lambda: {'total':0,'cpp_ok':0,'items':[]})
    for item in results.get('results', []):
        m = item.get('metadata', {})
        key = (int(m.get('sf',0)), int(m.get('bw',0)), int(m.get('cr',0)), snr_bin(float(m.get('snr_db',0.0))))
        g = groups[key]
        g['total'] += 1
        if item.get('cpp_stream', {}).get('status') == 'success' and item.get('cpp_stream', {}).get('payload_hex'):
            g['cpp_ok'] += 1
        g['items'].append(item)
    # score by failures
    scored = []
    for key, g in groups.items():
        failures = g['total'] - g['cpp_ok']
        if failures > 0:
            scored.append((failures, key))
    scored.sort(reverse=True)
    if not scored:
        return ((0,0,0,''), [])
    _, top_key = scored[0]
    return top_key, groups[top_key]['items']


def run_retry(binary: Path, vec: Path, meta: Meta) -> Dict[str, Any]:
    cmd = [
        str(binary), '--sf', str(meta.sf), '--bw', str(meta.bw), '--fs', str(meta.fs),
        '--ldro', '1' if meta.ldro else '0', '--sync-word', str(meta.sync_word),
        '--streaming', '--hdr-cfo-sweep', '--hdr-cfo-range', '300', '--hdr-cfo-step', '25', str(vec),
    ]
    if meta.impl:
        cmd.extend(['--implicit-header', '--payload-len', str(meta.payload_len), '--cr', str(meta.cr)])
        cmd.append('--has-crc' if meta.crc else '--no-crc')
    res = subprocess.run(cmd, capture_output=True, text=True, timeout=90)
    ok = res.returncode == 0
    payload_hex = None
    for line in res.stdout.splitlines():
        if line.startswith('payload_hex='):
            payload_hex = line.split('=',1)[1].strip()
            break
    return {'ok': ok and bool(payload_hex), 'stdout': res.stdout, 'stderr': res.stderr, 'payload_hex': payload_hex}


def main() -> None:
    ap = argparse.ArgumentParser(description='Retry top failure bin with wider CFO sweep')
    ap.add_argument('--results', type=Path, default=ROOT / 'results' / 'streaming_compat_results_cfo200.json')
    ap.add_argument('--limit', type=int, default=20)
    args = ap.parse_args()

    binary = resolve_cpp()
    if not binary:
        raise SystemExit('decode_cli not found; build cpp_receiver first')

    data = json.loads(args.results.read_text())
    top_key, items = pick_top_failure_bin(data)
    if not items:
        print('No failures to retry')
        return
    sf, bw, cr, binlab = top_key
    print(f'Top failure bin: sf={sf} bw={bw} cr={cr} snr={binlab}')

    # Collect failed items only in this bin
    failed = []
    for it in items:
        cpp = it.get('cpp_stream', {})
        if cpp.get('status') == 'success' and cpp.get('payload_hex'):
            continue
        failed.append(it)
    failed = failed[: args.limit]

    before_ok = 0
    after_ok = 0
    for it in failed:
        vec = Path(it['vector'])
        m = it['metadata']
        meta = Meta(sf=int(m['sf']), bw=int(m['bw']), fs=int(m.get('samp_rate') or m.get('sample_rate')), cr=int(m['cr']), crc=bool(m.get('crc', True)), impl=bool(m.get('impl_header') or m.get('implicit_header')), ldro=int(m.get('ldro_mode',0)), sync_word=int(m.get('sync_word',0x12)), payload_len=int(m.get('payload_len') or m.get('payload_length') or 0))
        if it.get('cpp_stream', {}).get('status') == 'success' and it['cpp_stream'].get('payload_hex'):
            before_ok += 1
        res = run_retry(binary, vec, meta)
        if res['ok']:
            after_ok += 1
        # Print per-vector delta
        status = 'OK' if res['ok'] else 'FAIL'
        print(f" - {vec.name}: {status}")
    improved = after_ok - before_ok
    print(f"Delta: before_ok={before_ok} after_ok={after_ok} improved={improved} over {len(failed)} retried")

if __name__ == '__main__':
    main()
