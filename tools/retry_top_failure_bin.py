#!/usr/bin/env python3
# This file provides the 'retry top failure bin' functionality for the LoRa Lite PHY toolkit.
"""Retry the worst failure bin with wider CFO sweep and report deltas.

Find the (sf,bw,cr,snr_bin) group with the highest number of failures in a
results JSON, rerun only the failing vectors in that bin with:
  --hdr-cfo-sweep --hdr-cfo-range 300 --hdr-cfo-step 25
Compare before/after counts and print a small delta report.
"""
# Imports specific objects with 'from __future__ import annotations'.
from __future__ import annotations

# Imports the module(s) argparse.
import argparse
# Imports the module(s) json.
import json
# Imports the module(s) subprocess.
import subprocess
# Imports specific objects with 'from collections import defaultdict'.
from collections import defaultdict
# Imports specific objects with 'from dataclasses import dataclass'.
from dataclasses import dataclass
# Imports specific objects with 'from pathlib import Path'.
from pathlib import Path
# Imports specific objects with 'from typing import Dict, Any, List, Tuple'.
from typing import Dict, Any, List, Tuple

# Executes the statement `ROOT = Path(__file__).resolve().parents[1]`.
ROOT = Path(__file__).resolve().parents[1]
# Executes the statement `RESULTS_JSON = ROOT / 'results' / 'streaming_compat_results.json'`.
RESULTS_JSON = ROOT / 'results' / 'streaming_compat_results.json'
# Executes the statement `CPP_CANDIDATES = [`.
CPP_CANDIDATES = [
    # Executes the statement `ROOT / 'cpp_receiver/build/decode_cli',`.
    ROOT / 'cpp_receiver/build/decode_cli',
    # Executes the statement `ROOT / 'cpp_receiver/build/Release/decode_cli',`.
    ROOT / 'cpp_receiver/build/Release/decode_cli',
# Closes the previously opened list indexing or literal.
]

# Executes the statement `@dataclass`.
@dataclass
# Declares the class Meta.
class Meta:
    # Executes the statement `sf: int`.
    sf: int
    # Executes the statement `bw: int`.
    bw: int
    # Executes the statement `fs: int`.
    fs: int
    # Executes the statement `cr: int`.
    cr: int
    # Executes the statement `crc: bool`.
    crc: bool
    # Executes the statement `impl: bool`.
    impl: bool
    # Executes the statement `ldro: int`.
    ldro: int
    # Executes the statement `sync_word: int`.
    sync_word: int
    # Executes the statement `payload_len: int`.
    payload_len: int


# Defines the function resolve_cpp.
def resolve_cpp() -> Path | None:
    # Starts a loop iterating over a sequence.
    for p in CPP_CANDIDATES:
        # Begins a conditional branch to check a condition.
        if p.exists():
            # Returns the computed value to the caller.
            return p
    # Returns the computed value to the caller.
    return None


# Defines the function snr_bin.
def snr_bin(v: float) -> str:
    # Executes the statement `edges = [-999, -20, -15, -10, -5, 0, 5, 10, 999]`.
    edges = [-999, -20, -15, -10, -5, 0, 5, 10, 999]
    # Executes the statement `labels = ["<-20", "[-20,-15)", "[-15,-10)", "[-10,-5)", "[-5,0)", "[0,5)", "[5,10)", ">=10"]`.
    labels = ["<-20", "[-20,-15)", "[-15,-10)", "[-10,-5)", "[-5,0)", "[0,5)", "[5,10)", ">=10"]
    # Starts a loop iterating over a sequence.
    for i in range(len(edges)-1):
        # Begins a conditional branch to check a condition.
        if edges[i] <= v < edges[i+1]:
            # Returns the computed value to the caller.
            return labels[i]
    # Returns the computed value to the caller.
    return labels[-1]


# Defines the function pick_top_failure_bin.
def pick_top_failure_bin(results: Dict[str, Any]) -> Tuple[Tuple[int,int,int,str], List[Dict[str, Any]]]:
    # Executes the statement `groups: Dict[Tuple[int,int,int,str], Dict[str, Any]] = defaultdict(lambda: {'total':0,'cpp_ok':0,'items':[]})`.
    groups: Dict[Tuple[int,int,int,str], Dict[str, Any]] = defaultdict(lambda: {'total':0,'cpp_ok':0,'items':[]})
    # Starts a loop iterating over a sequence.
    for item in results.get('results', []):
        # Executes the statement `m = item.get('metadata', {})`.
        m = item.get('metadata', {})
        # Executes the statement `key = (int(m.get('sf',0)), int(m.get('bw',0)), int(m.get('cr',0)), snr_bin(float(m.get('snr_db',0.0))))`.
        key = (int(m.get('sf',0)), int(m.get('bw',0)), int(m.get('cr',0)), snr_bin(float(m.get('snr_db',0.0))))
        # Executes the statement `g = groups[key]`.
        g = groups[key]
        # Executes the statement `g['total'] += 1`.
        g['total'] += 1
        # Begins a conditional branch to check a condition.
        if item.get('cpp_stream', {}).get('status') == 'success' and item.get('cpp_stream', {}).get('payload_hex'):
            # Executes the statement `g['cpp_ok'] += 1`.
            g['cpp_ok'] += 1
        # Executes the statement `g['items'].append(item)`.
        g['items'].append(item)
    # score by failures
    # Executes the statement `scored = []`.
    scored = []
    # Starts a loop iterating over a sequence.
    for key, g in groups.items():
        # Executes the statement `failures = g['total'] - g['cpp_ok']`.
        failures = g['total'] - g['cpp_ok']
        # Begins a conditional branch to check a condition.
        if failures > 0:
            # Executes the statement `scored.append((failures, key))`.
            scored.append((failures, key))
    # Executes the statement `scored.sort(reverse=True)`.
    scored.sort(reverse=True)
    # Begins a conditional branch to check a condition.
    if not scored:
        # Returns the computed value to the caller.
        return ((0,0,0,''), [])
    # Executes the statement `_, top_key = scored[0]`.
    _, top_key = scored[0]
    # Returns the computed value to the caller.
    return top_key, groups[top_key]['items']


# Defines the function run_retry.
def run_retry(binary: Path, vec: Path, meta: Meta) -> Dict[str, Any]:
    # Executes the statement `cmd = [`.
    cmd = [
        # Executes the statement `str(binary), '--sf', str(meta.sf), '--bw', str(meta.bw), '--fs', str(meta.fs),`.
        str(binary), '--sf', str(meta.sf), '--bw', str(meta.bw), '--fs', str(meta.fs),
        # Executes the statement `'--ldro', '1' if meta.ldro else '0', '--sync-word', str(meta.sync_word),`.
        '--ldro', '1' if meta.ldro else '0', '--sync-word', str(meta.sync_word),
        # Executes the statement `'--streaming', '--hdr-cfo-sweep', '--hdr-cfo-range', '300', '--hdr-cfo-step', '25', str(vec),`.
        '--streaming', '--hdr-cfo-sweep', '--hdr-cfo-range', '300', '--hdr-cfo-step', '25', str(vec),
    # Closes the previously opened list indexing or literal.
    ]
    # Begins a conditional branch to check a condition.
    if meta.impl:
        # Executes the statement `cmd.extend(['--implicit-header', '--payload-len', str(meta.payload_len), '--cr', str(meta.cr)])`.
        cmd.extend(['--implicit-header', '--payload-len', str(meta.payload_len), '--cr', str(meta.cr)])
        # Executes the statement `cmd.append('--has-crc' if meta.crc else '--no-crc')`.
        cmd.append('--has-crc' if meta.crc else '--no-crc')
    # Executes the statement `res = subprocess.run(cmd, capture_output=True, text=True, timeout=90)`.
    res = subprocess.run(cmd, capture_output=True, text=True, timeout=90)
    # Executes the statement `ok = res.returncode == 0`.
    ok = res.returncode == 0
    # Executes the statement `payload_hex = None`.
    payload_hex = None
    # Starts a loop iterating over a sequence.
    for line in res.stdout.splitlines():
        # Begins a conditional branch to check a condition.
        if line.startswith('payload_hex='):
            # Executes the statement `payload_hex = line.split('=',1)[1].strip()`.
            payload_hex = line.split('=',1)[1].strip()
            # Exits the nearest enclosing loop early.
            break
    # Returns the computed value to the caller.
    return {'ok': ok and bool(payload_hex), 'stdout': res.stdout, 'stderr': res.stderr, 'payload_hex': payload_hex}


# Defines the function main.
def main() -> None:
    # Executes the statement `ap = argparse.ArgumentParser(description='Retry top failure bin with wider CFO sweep')`.
    ap = argparse.ArgumentParser(description='Retry top failure bin with wider CFO sweep')
    # Executes the statement `ap.add_argument('--results', type=Path, default=ROOT / 'results' / 'streaming_compat_results_cfo200.json')`.
    ap.add_argument('--results', type=Path, default=ROOT / 'results' / 'streaming_compat_results_cfo200.json')
    # Executes the statement `ap.add_argument('--limit', type=int, default=20)`.
    ap.add_argument('--limit', type=int, default=20)
    # Executes the statement `args = ap.parse_args()`.
    args = ap.parse_args()

    # Executes the statement `binary = resolve_cpp()`.
    binary = resolve_cpp()
    # Begins a conditional branch to check a condition.
    if not binary:
        # Raises an exception to signal an error.
        raise SystemExit('decode_cli not found; build cpp_receiver first')

    # Executes the statement `data = json.loads(args.results.read_text())`.
    data = json.loads(args.results.read_text())
    # Executes the statement `top_key, items = pick_top_failure_bin(data)`.
    top_key, items = pick_top_failure_bin(data)
    # Begins a conditional branch to check a condition.
    if not items:
        # Outputs diagnostic or user-facing text.
        print('No failures to retry')
        # Returns control to the caller.
        return
    # Executes the statement `sf, bw, cr, binlab = top_key`.
    sf, bw, cr, binlab = top_key
    # Outputs diagnostic or user-facing text.
    print(f'Top failure bin: sf={sf} bw={bw} cr={cr} snr={binlab}')

    # Collect failed items only in this bin
    # Executes the statement `failed = []`.
    failed = []
    # Starts a loop iterating over a sequence.
    for it in items:
        # Executes the statement `cpp = it.get('cpp_stream', {})`.
        cpp = it.get('cpp_stream', {})
        # Begins a conditional branch to check a condition.
        if cpp.get('status') == 'success' and cpp.get('payload_hex'):
            # Skips to the next iteration of the loop.
            continue
        # Executes the statement `failed.append(it)`.
        failed.append(it)
    # Executes the statement `failed = failed[: args.limit]`.
    failed = failed[: args.limit]

    # Executes the statement `before_ok = 0`.
    before_ok = 0
    # Executes the statement `after_ok = 0`.
    after_ok = 0
    # Starts a loop iterating over a sequence.
    for it in failed:
        # Executes the statement `vec = Path(it['vector'])`.
        vec = Path(it['vector'])
        # Executes the statement `m = it['metadata']`.
        m = it['metadata']
        # Executes the statement `meta = Meta(sf=int(m['sf']), bw=int(m['bw']), fs=int(m.get('samp_rate') or m.get('sample_rate')), cr=int(m['cr']), crc=bool(m.get('crc', True)), impl=bool(m.get('impl_header') or m.get('implicit_header')), ldro=int(m.get('ldro_mode',0)), sync_word=int(m.get('sync_word',0x12)), payload_len=int(m.get('payload_len') or m.get('payload_length') or 0))`.
        meta = Meta(sf=int(m['sf']), bw=int(m['bw']), fs=int(m.get('samp_rate') or m.get('sample_rate')), cr=int(m['cr']), crc=bool(m.get('crc', True)), impl=bool(m.get('impl_header') or m.get('implicit_header')), ldro=int(m.get('ldro_mode',0)), sync_word=int(m.get('sync_word',0x12)), payload_len=int(m.get('payload_len') or m.get('payload_length') or 0))
        # Begins a conditional branch to check a condition.
        if it.get('cpp_stream', {}).get('status') == 'success' and it['cpp_stream'].get('payload_hex'):
            # Executes the statement `before_ok += 1`.
            before_ok += 1
        # Executes the statement `res = run_retry(binary, vec, meta)`.
        res = run_retry(binary, vec, meta)
        # Begins a conditional branch to check a condition.
        if res['ok']:
            # Executes the statement `after_ok += 1`.
            after_ok += 1
        # Print per-vector delta
        # Executes the statement `status = 'OK' if res['ok'] else 'FAIL'`.
        status = 'OK' if res['ok'] else 'FAIL'
        # Outputs diagnostic or user-facing text.
        print(f" - {vec.name}: {status}")
    # Executes the statement `improved = after_ok - before_ok`.
    improved = after_ok - before_ok
    # Outputs diagnostic or user-facing text.
    print(f"Delta: before_ok={before_ok} after_ok={after_ok} improved={improved} over {len(failed)} retried")

# Begins a conditional branch to check a condition.
if __name__ == '__main__':
    # Executes the statement `main()`.
    main()
