#!/usr/bin/env python3

"""
Simple sdr_lora-only CLI for decoding LoRa cf32 IQ files.

Subcommands:
  - decode: Decode a single .cf32 using external/sdr_lora
  - batch:  Delegate to scripts.sdr_lora_batch_decode

Usage:
  python -m scripts.sdr_lora_cli decode <file.cf32> [-v]
  python -m scripts.sdr_lora_cli batch [--fast]
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional

import numpy as np


# NumPy 2.0 compatibility: some external libs still use np.Inf
if not hasattr(np, "Inf"):
	setattr(np, "Inf", np.inf)


ROOT = Path(__file__).resolve().parents[1]
SDR_LORA = ROOT / "external" / "sdr_lora"
PYTHON_MODULES = ROOT / "python_modules"
OFFLINE_DECODER = ROOT / "external" / "gr_lora_sdr" / "scripts" / "decode_offline_recording.py"
for candidate in (SDR_LORA, ROOT, PYTHON_MODULES):
	candidate_str = str(candidate)
	if candidate_str not in sys.path:
		sys.path.insert(0, candidate_str)


def load_cf32(path: Path) -> np.ndarray:
	data = np.fromfile(path, dtype=np.float32)
	if data.size % 2 != 0:
		raise ValueError(f"File {path} has odd number of float32 elements; not valid interleaved cf32")
	i = data[0::2]
	q = data[1::2]
	return (i + 1j * q).astype(np.complex64, copy=False)


def read_meta(meta_path: Path):
	with open(meta_path, "r") as f:
		meta = json.load(f)
	# Normalize keys we care about
	sf = int(meta.get("sf"))
	bw = int(meta.get("bw"))
	fs = int(meta.get("samp_rate") or meta.get("sample_rate") or meta.get("fs"))
	payload_hex = meta.get("payload_hex")
	return sf, bw, fs, payload_hex, meta


def payload_to_hex(payload_arr: Optional[np.ndarray]) -> str:
	if payload_arr is None:
		return ""
	# Avoid NumPy truth-value pitfalls; explicitly convert to bytes via int
	return bytes(int(b) for b in payload_arr).hex()


def run_offline_decoder(cf32_path: Path,
					     sf: int,
					     bw: int,
					     fs: int,
					     meta: Dict[str, Any]) -> Optional[List[Dict[str, Any]]]:
	cmd = [
		sys.executable,
		str(OFFLINE_DECODER),
		str(cf32_path),
		"--sf", str(sf),
		"--bw", str(bw),
		"--samp-rate", str(fs),
		"--cr", str(int(meta.get('cr', 2))),
		"--ldro-mode", str(int(meta.get('ldro_mode', 0) or 0)),
		"--format", "cf32",
	]
	cmd.append("--impl-header" if meta.get('impl_header') else "--explicit-header")
	cmd.append("--has-crc" if meta.get('crc', True) else "--no-crc")
	try:
		result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
	except subprocess.TimeoutExpired:
		return None
	if result.returncode != 0:
		return None
	frames: List[Dict[str, Any]] = []
	hex_value: Optional[str] = None
	crc_valid = False
	hdr_len = meta.get('payload_len')
	for line in result.stdout.splitlines():
		line = line.strip()
		if line.startswith('Hex:'):
			hex_str = line.replace('Hex:', '').strip().replace(' ', '')
			hex_value = hex_str.lower()
		elif line.startswith('Frame') and 'CRC valid' in line:
			crc_valid = True
	if hex_value is None:
		return None
	frames.append({
		"hdr_ok": 1 if hdr_len is not None else 0,
		"crc_ok": 1 if crc_valid else 0,
		"has_crc": 1 if meta.get('crc', True) else 0,
		"cr": int(meta.get('cr', 2)),
		"ih": 1 if meta.get('impl_header') else 0,
		"payload_len": hdr_len or 0,
		"hex": hex_value,
	})
	return frames


def cmd_decode(args: argparse.Namespace) -> int:
	cf32_path = Path(args.input)
	if not cf32_path.exists():
		print(json.dumps({"status": "error", "error": f"Input file not found: {cf32_path}"}))
		return 2

	# Load params from sidecar .json if present unless user passed explicit ones
	payload_hex_expected: Optional[str] = None
	meta_all: Dict[str, Any] = {}
	if args.meta:
		meta_path = Path(args.meta)
		if not meta_path.exists():
			print(json.dumps({"status": "error", "error": f"Meta file not found: {meta_path}"}))
			return 2
		sf, bw, fs, payload_hex_expected, meta_all = read_meta(meta_path)
	else:
		sidecar = cf32_path.with_suffix(".json")
		if sidecar.exists():
			sf, bw, fs, payload_hex_expected, meta_all = read_meta(sidecar)
		else:
			# Fall back to CLI args
			if args.sf is None or args.bw is None or args.fs is None:
				print(json.dumps({"status": "error", "error": "Missing SF/BW/fs. Provide --meta or all of --sf --bw --fs."}))
				return 2
			sf, bw, fs = int(args.sf), int(args.bw), int(args.fs)

	# Import decoder
	try:
		import lora as sdr_lora  # type: ignore
	except Exception as e:
		print(json.dumps({"status": "error", "error": f"Failed to import external sdr_lora: {e}"}))
		return 2

	samples = load_cf32(cf32_path)
	override = {
		'ih': bool(meta_all.get('impl_header', False)),
		'cr': int(meta_all.get('cr', 2)) if meta_all.get('cr') is not None else 2,
		'has_crc': bool(meta_all.get('crc', True)),
		'length': int(meta_all.get('payload_len', 0)) if meta_all.get('payload_len') is not None else 0,
		'expected_hex': str(meta_all.get('payload_hex', '') or ''),
		'ldro_mode': int(meta_all.get('ldro_mode', 0) or 0),
		# performance/sweep controls
		'fast': bool(args.fast),
		'sweep_max_rot': 4 if args.fast else None,
		'try_local': False if args.fast else True,
		'quick_local': True,
	}

	pkts = sdr_lora.decode(samples, sf, bw, fs, override=override)
	pkts_list = list(pkts) if pkts is not None else []
	fallback_frames: Optional[List[Dict[str, Any]]] = None
	if len(pkts_list) == 0:
		fallback_frames = run_offline_decoder(cf32_path, sf, bw, fs, meta_all)

	out: Dict[str, Any] = {
		"status": "ok",
		"file": str(cf32_path),
		"sf": sf,
		"bw": bw,
		"fs": fs,
		"expected": (payload_hex_expected or ""),
		"found": [],
	}

	if pkts_list:
		for pkt in pkts_list:
			cur = {
				"hdr_ok": int(pkt.hdr_ok),
				"crc_ok": int(pkt.crc_ok),
				"has_crc": int(pkt.has_crc),
				"cr": int(pkt.cr),
				"ih": int(pkt.ih),
				"payload_len": (len(pkt.payload) if pkt.payload is not None else 0),
				"hex": payload_to_hex(pkt.payload),
			}
			out["found"].append(cur)
	elif fallback_frames:
		out["fallback"] = "offline_gr"
		out["found"].extend(fallback_frames)

	# Print results (compact when not verbose)
	if args.verbose:
		print(json.dumps(out, indent=2))
	else:
		# Minimal one-line summary
		summ = {
			"file": Path(out["file"]).name,
			"count": len(out["found"]),
			"match": any((payload_hex_expected and f["hex"].lower() == str(payload_hex_expected).lower()) for f in out["found"]),
		}
		print(json.dumps(summ))

	# Exit code: 0 if any packets found, 1 otherwise
	return 0 if len(out["found"]) > 0 else 1


def cmd_batch(args: argparse.Namespace) -> int:
	# Delegate to scripts.sdr_lora_batch_decode
	from .sdr_lora_batch_decode import main as batch_main
	argv = [
		'sdr_lora_batch_decode.py',
	]
	if args.fast:
		argv.append('--fast')
	if args.roots:
		argv.append('--roots')
		argv.extend(args.roots)
	if args.out:
		argv.extend(['--out', args.out])
	old = sys.argv
	try:
		sys.argv = argv
		return batch_main()
	finally:
		sys.argv = old


def build_parser() -> argparse.ArgumentParser:
	p = argparse.ArgumentParser(description="sdr_lora-only CLI")
	sub = p.add_subparsers(dest='command', required=True)

	# decode
	pd = sub.add_parser('decode', help='Decode a single IQ file using external/sdr_lora')
	pd.add_argument('input', help='Path to .cf32 file')
	pd.add_argument('--meta', help='Path to metadata JSON (defaults to sidecar .json if present)')
	pd.add_argument('--sf', type=int, help='Spreading factor (fallback if no meta)')
	pd.add_argument('--bw', type=int, help='Bandwidth in Hz (fallback if no meta)')
	pd.add_argument('--fs', type=int, help='Sample rate in Hz (fallback if no meta)')
	pd.add_argument('--fast', action='store_true', help='Speed up by limiting search and disabling heavy fallbacks')
	pd.add_argument('-v', '--verbose', action='store_true', help='Pretty-print full JSON output')
	pd.set_defaults(func=cmd_decode)

	# batch
	pb = sub.add_parser('batch', help='Batch-decode vectors and summarize results')
	pb.add_argument('--fast', action='store_true', help='Speed up by limiting search and disabling heavy fallbacks')
	pb.add_argument('--roots', nargs='*', default=[
		'golden_vectors_demo', 'golden_vectors_demo_batch', 'vectors'
	])
	pb.add_argument('--out', default='results/sdr_lora_batch.json')
	pb.set_defaults(func=cmd_batch)

	return p


def main() -> int:
	parser = build_parser()
	args = parser.parse_args()
	return args.func(args)


if __name__ == "__main__":
	raise SystemExit(main())
