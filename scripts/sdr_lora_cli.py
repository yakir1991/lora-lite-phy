#!/usr/bin/env python3

# This file provides the 'sdr lora cli' functionality for the LoRa Lite PHY toolkit.
"""
Simple sdr_lora-only CLI for decoding LoRa cf32 IQ files.

Subcommands:
  - decode: Decode a single .cf32 using external/sdr_lora
  - batch:  Delegate to scripts.sdr_lora_batch_decode

Usage:
  python -m scripts.sdr_lora_cli decode <file.cf32> [-v]
  python -m scripts.sdr_lora_cli batch [--fast]
"""

# Imports the module(s) argparse.
import argparse
# Imports the module(s) json.
import json
# Imports the module(s) subprocess.
import subprocess
# Imports the module(s) sys.
import sys
# Imports specific objects with 'from pathlib import Path'.
from pathlib import Path
# Imports specific objects with 'from typing import Any, Dict, List, Optional'.
from typing import Any, Dict, List, Optional

# Imports the module(s) numpy as np.
import numpy as np


# NumPy 2.0 compatibility: some external libs still use np.Inf
# Begins a conditional branch to check a condition.
if not hasattr(np, "Inf"):
# Executes the statement `setattr(np, "Inf", np.inf)`.
	setattr(np, "Inf", np.inf)


# Executes the statement `ROOT = Path(__file__).resolve().parents[1]`.
ROOT = Path(__file__).resolve().parents[1]
# Executes the statement `SDR_LORA = ROOT / "external" / "sdr_lora"`.
SDR_LORA = ROOT / "external" / "sdr_lora"
# Executes the statement `PYTHON_MODULES = ROOT / "python_modules"`.
PYTHON_MODULES = ROOT / "python_modules"
# Executes the statement `OFFLINE_DECODER = ROOT / "external" / "gr_lora_sdr" / "scripts" / "decode_offline_recording.py"`.
OFFLINE_DECODER = ROOT / "external" / "gr_lora_sdr" / "scripts" / "decode_offline_recording.py"
# Starts a loop iterating over a sequence.
for candidate in (SDR_LORA, ROOT, PYTHON_MODULES):
# Executes the statement `candidate_str = str(candidate)`.
	candidate_str = str(candidate)
# Begins a conditional branch to check a condition.
	if candidate_str not in sys.path:
# Executes the statement `sys.path.insert(0, candidate_str)`.
		sys.path.insert(0, candidate_str)


# Defines the function load_cf32.
def load_cf32(path: Path) -> np.ndarray:
# Executes the statement `data = np.fromfile(path, dtype=np.float32)`.
	data = np.fromfile(path, dtype=np.float32)
# Begins a conditional branch to check a condition.
	if data.size % 2 != 0:
# Raises an exception to signal an error.
		raise ValueError(f"File {path} has odd number of float32 elements; not valid interleaved cf32")
# Executes the statement `i = data[0::2]`.
	i = data[0::2]
# Executes the statement `q = data[1::2]`.
	q = data[1::2]
# Returns the computed value to the caller.
	return (i + 1j * q).astype(np.complex64, copy=False)


# Defines the function read_meta.
def read_meta(meta_path: Path):
# Opens a context manager scope for managed resources.
	with open(meta_path, "r") as f:
# Executes the statement `meta = json.load(f)`.
		meta = json.load(f)
	# Normalize keys we care about
# Executes the statement `sf = int(meta.get("sf"))`.
	sf = int(meta.get("sf"))
# Executes the statement `bw = int(meta.get("bw"))`.
	bw = int(meta.get("bw"))
# Executes the statement `fs = int(meta.get("samp_rate") or meta.get("sample_rate") or meta.get("fs"))`.
	fs = int(meta.get("samp_rate") or meta.get("sample_rate") or meta.get("fs"))
# Executes the statement `payload_hex = meta.get("payload_hex")`.
	payload_hex = meta.get("payload_hex")
# Returns the computed value to the caller.
	return sf, bw, fs, payload_hex, meta


# Defines the function payload_to_hex.
def payload_to_hex(payload_arr: Optional[np.ndarray]) -> str:
# Begins a conditional branch to check a condition.
	if payload_arr is None:
# Returns the computed value to the caller.
		return ""
	# Avoid NumPy truth-value pitfalls; explicitly convert to bytes via int
# Returns the computed value to the caller.
	return bytes(int(b) for b in payload_arr).hex()


# Defines the function run_offline_decoder.
def run_offline_decoder(cf32_path: Path,
# Executes the statement `sf: int,`.
					     sf: int,
# Executes the statement `bw: int,`.
					     bw: int,
# Executes the statement `fs: int,`.
					     fs: int,
# Executes the statement `meta: Dict[str, Any]) -> Optional[List[Dict[str, Any]]]:`.
					     meta: Dict[str, Any]) -> Optional[List[Dict[str, Any]]]:
# Executes the statement `cmd = [`.
	cmd = [
# Executes the statement `sys.executable,`.
		sys.executable,
# Executes the statement `str(OFFLINE_DECODER),`.
		str(OFFLINE_DECODER),
# Executes the statement `str(cf32_path),`.
		str(cf32_path),
# Executes the statement `"--sf", str(sf),`.
		"--sf", str(sf),
# Executes the statement `"--bw", str(bw),`.
		"--bw", str(bw),
# Executes the statement `"--samp-rate", str(fs),`.
		"--samp-rate", str(fs),
# Executes the statement `"--cr", str(int(meta.get('cr', 2))),`.
		"--cr", str(int(meta.get('cr', 2))),
# Executes the statement `"--ldro-mode", str(int(meta.get('ldro_mode', 0) or 0)),`.
		"--ldro-mode", str(int(meta.get('ldro_mode', 0) or 0)),
# Executes the statement `"--format", "cf32",`.
		"--format", "cf32",
# Closes the previously opened list indexing or literal.
	]
# Executes the statement `cmd.append("--impl-header" if meta.get('impl_header') else "--explicit-header")`.
	cmd.append("--impl-header" if meta.get('impl_header') else "--explicit-header")
# Executes the statement `cmd.append("--has-crc" if meta.get('crc', True) else "--no-crc")`.
	cmd.append("--has-crc" if meta.get('crc', True) else "--no-crc")
# Begins a block that monitors for exceptions.
	try:
# Executes the statement `result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)`.
		result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
# Handles a specific exception from the try block.
	except subprocess.TimeoutExpired:
# Returns the computed value to the caller.
		return None
# Begins a conditional branch to check a condition.
	if result.returncode != 0:
# Returns the computed value to the caller.
		return None
# Executes the statement `frames: List[Dict[str, Any]] = []`.
	frames: List[Dict[str, Any]] = []
# Executes the statement `hex_value: Optional[str] = None`.
	hex_value: Optional[str] = None
# Executes the statement `crc_valid = False`.
	crc_valid = False
# Executes the statement `hdr_len = meta.get('payload_len')`.
	hdr_len = meta.get('payload_len')
# Starts a loop iterating over a sequence.
	for line in result.stdout.splitlines():
# Executes the statement `line = line.strip()`.
		line = line.strip()
# Begins a conditional branch to check a condition.
		if line.startswith('Hex:'):
# Executes the statement `hex_str = line.replace('Hex:', '').strip().replace(' ', '')`.
			hex_str = line.replace('Hex:', '').strip().replace(' ', '')
# Executes the statement `hex_value = hex_str.lower()`.
			hex_value = hex_str.lower()
# Handles an additional condition in the branching logic.
		elif line.startswith('Frame') and 'CRC valid' in line:
# Executes the statement `crc_valid = True`.
			crc_valid = True
# Begins a conditional branch to check a condition.
	if hex_value is None:
# Returns the computed value to the caller.
		return None
# Executes the statement `frames.append({`.
	frames.append({
# Executes the statement `"hdr_ok": 1 if hdr_len is not None else 0,`.
		"hdr_ok": 1 if hdr_len is not None else 0,
# Executes the statement `"crc_ok": 1 if crc_valid else 0,`.
		"crc_ok": 1 if crc_valid else 0,
# Executes the statement `"has_crc": 1 if meta.get('crc', True) else 0,`.
		"has_crc": 1 if meta.get('crc', True) else 0,
# Executes the statement `"cr": int(meta.get('cr', 2)),`.
		"cr": int(meta.get('cr', 2)),
# Executes the statement `"ih": 1 if meta.get('impl_header') else 0,`.
		"ih": 1 if meta.get('impl_header') else 0,
# Executes the statement `"payload_len": hdr_len or 0,`.
		"payload_len": hdr_len or 0,
# Executes the statement `"hex": hex_value,`.
		"hex": hex_value,
# Executes the statement `})`.
	})
# Returns the computed value to the caller.
	return frames


# Defines the function cmd_decode.
def cmd_decode(args: argparse.Namespace) -> int:
# Executes the statement `cf32_path = Path(args.input)`.
	cf32_path = Path(args.input)
# Begins a conditional branch to check a condition.
	if not cf32_path.exists():
# Outputs diagnostic or user-facing text.
		print(json.dumps({"status": "error", "error": f"Input file not found: {cf32_path}"}))
# Returns the computed value to the caller.
		return 2

	# Load params from sidecar .json if present unless user passed explicit ones
# Executes the statement `payload_hex_expected: Optional[str] = None`.
	payload_hex_expected: Optional[str] = None
# Executes the statement `meta_all: Dict[str, Any] = {}`.
	meta_all: Dict[str, Any] = {}
# Begins a conditional branch to check a condition.
	if args.meta:
# Executes the statement `meta_path = Path(args.meta)`.
		meta_path = Path(args.meta)
# Begins a conditional branch to check a condition.
		if not meta_path.exists():
# Outputs diagnostic or user-facing text.
			print(json.dumps({"status": "error", "error": f"Meta file not found: {meta_path}"}))
# Returns the computed value to the caller.
			return 2
# Executes the statement `sf, bw, fs, payload_hex_expected, meta_all = read_meta(meta_path)`.
		sf, bw, fs, payload_hex_expected, meta_all = read_meta(meta_path)
# Provides the fallback branch when previous conditions fail.
	else:
# Executes the statement `sidecar = cf32_path.with_suffix(".json")`.
		sidecar = cf32_path.with_suffix(".json")
# Begins a conditional branch to check a condition.
		if sidecar.exists():
# Executes the statement `sf, bw, fs, payload_hex_expected, meta_all = read_meta(sidecar)`.
			sf, bw, fs, payload_hex_expected, meta_all = read_meta(sidecar)
# Provides the fallback branch when previous conditions fail.
		else:
			# Fall back to CLI args
# Begins a conditional branch to check a condition.
			if args.sf is None or args.bw is None or args.fs is None:
# Outputs diagnostic or user-facing text.
				print(json.dumps({"status": "error", "error": "Missing SF/BW/fs. Provide --meta or all of --sf --bw --fs."}))
# Returns the computed value to the caller.
				return 2
# Executes the statement `sf, bw, fs = int(args.sf), int(args.bw), int(args.fs)`.
			sf, bw, fs = int(args.sf), int(args.bw), int(args.fs)

	# Import decoder
# Begins a block that monitors for exceptions.
	try:
# Imports the module(s) lora as sdr_lora  # type: ignore.
		import lora as sdr_lora  # type: ignore
# Handles a specific exception from the try block.
	except Exception as e:
# Outputs diagnostic or user-facing text.
		print(json.dumps({"status": "error", "error": f"Failed to import external sdr_lora: {e}"}))
# Returns the computed value to the caller.
		return 2

# Executes the statement `samples = load_cf32(cf32_path)`.
	samples = load_cf32(cf32_path)
# Executes the statement `override = {`.
	override = {
# Executes the statement `'ih': bool(meta_all.get('impl_header', False)),`.
		'ih': bool(meta_all.get('impl_header', False)),
# Executes the statement `'cr': int(meta_all.get('cr', 2)) if meta_all.get('cr') is not None else 2,`.
		'cr': int(meta_all.get('cr', 2)) if meta_all.get('cr') is not None else 2,
# Executes the statement `'has_crc': bool(meta_all.get('crc', True)),`.
		'has_crc': bool(meta_all.get('crc', True)),
# Executes the statement `'length': int(meta_all.get('payload_len', 0)) if meta_all.get('payload_len') is not None else 0,`.
		'length': int(meta_all.get('payload_len', 0)) if meta_all.get('payload_len') is not None else 0,
# Executes the statement `'expected_hex': str(meta_all.get('payload_hex', '') or ''),`.
		'expected_hex': str(meta_all.get('payload_hex', '') or ''),
# Executes the statement `'ldro_mode': int(meta_all.get('ldro_mode', 0) or 0),`.
		'ldro_mode': int(meta_all.get('ldro_mode', 0) or 0),
		# performance/sweep controls
# Executes the statement `'fast': bool(args.fast),`.
		'fast': bool(args.fast),
# Executes the statement `'sweep_max_rot': 4 if args.fast else None,`.
		'sweep_max_rot': 4 if args.fast else None,
# Executes the statement `'try_local': False if args.fast else True,`.
		'try_local': False if args.fast else True,
# Executes the statement `'quick_local': True,`.
		'quick_local': True,
# Closes the previously opened dictionary or set literal.
	}

# Executes the statement `pkts = sdr_lora.decode(samples, sf, bw, fs, override=override)`.
	pkts = sdr_lora.decode(samples, sf, bw, fs, override=override)
# Executes the statement `pkts_list = list(pkts) if pkts is not None else []`.
	pkts_list = list(pkts) if pkts is not None else []
# Executes the statement `fallback_frames: Optional[List[Dict[str, Any]]] = None`.
	fallback_frames: Optional[List[Dict[str, Any]]] = None
# Begins a conditional branch to check a condition.
	if len(pkts_list) == 0:
# Executes the statement `fallback_frames = run_offline_decoder(cf32_path, sf, bw, fs, meta_all)`.
		fallback_frames = run_offline_decoder(cf32_path, sf, bw, fs, meta_all)

# Executes the statement `out: Dict[str, Any] = {`.
	out: Dict[str, Any] = {
# Executes the statement `"status": "ok",`.
		"status": "ok",
# Executes the statement `"file": str(cf32_path),`.
		"file": str(cf32_path),
# Executes the statement `"sf": sf,`.
		"sf": sf,
# Executes the statement `"bw": bw,`.
		"bw": bw,
# Executes the statement `"fs": fs,`.
		"fs": fs,
# Executes the statement `"expected": (payload_hex_expected or ""),`.
		"expected": (payload_hex_expected or ""),
# Executes the statement `"found": [],`.
		"found": [],
# Closes the previously opened dictionary or set literal.
	}

# Begins a conditional branch to check a condition.
	if pkts_list:
# Starts a loop iterating over a sequence.
		for pkt in pkts_list:
# Executes the statement `cur = {`.
			cur = {
# Executes the statement `"hdr_ok": int(pkt.hdr_ok),`.
				"hdr_ok": int(pkt.hdr_ok),
# Executes the statement `"crc_ok": int(pkt.crc_ok),`.
				"crc_ok": int(pkt.crc_ok),
# Executes the statement `"has_crc": int(pkt.has_crc),`.
				"has_crc": int(pkt.has_crc),
# Executes the statement `"cr": int(pkt.cr),`.
				"cr": int(pkt.cr),
# Executes the statement `"ih": int(pkt.ih),`.
				"ih": int(pkt.ih),
# Executes the statement `"payload_len": (len(pkt.payload) if pkt.payload is not None else 0),`.
				"payload_len": (len(pkt.payload) if pkt.payload is not None else 0),
# Executes the statement `"hex": payload_to_hex(pkt.payload),`.
				"hex": payload_to_hex(pkt.payload),
# Closes the previously opened dictionary or set literal.
			}
# Executes the statement `out["found"].append(cur)`.
			out["found"].append(cur)
# Handles an additional condition in the branching logic.
	elif fallback_frames:
# Executes the statement `out["fallback"] = "offline_gr"`.
		out["fallback"] = "offline_gr"
# Executes the statement `out["found"].extend(fallback_frames)`.
		out["found"].extend(fallback_frames)

	# Print results (compact when not verbose)
# Begins a conditional branch to check a condition.
	if args.verbose:
# Outputs diagnostic or user-facing text.
		print(json.dumps(out, indent=2))
# Provides the fallback branch when previous conditions fail.
	else:
		# Minimal one-line summary
# Executes the statement `summ = {`.
		summ = {
# Executes the statement `"file": Path(out["file"]).name,`.
			"file": Path(out["file"]).name,
# Executes the statement `"count": len(out["found"]),`.
			"count": len(out["found"]),
# Executes the statement `"match": any((payload_hex_expected and f["hex"].lower() == str(payload_hex_expected).lower()) for f in out["found"]),`.
			"match": any((payload_hex_expected and f["hex"].lower() == str(payload_hex_expected).lower()) for f in out["found"]),
# Closes the previously opened dictionary or set literal.
		}
# Outputs diagnostic or user-facing text.
		print(json.dumps(summ))

	# Exit code: 0 if any packets found, 1 otherwise
# Returns the computed value to the caller.
	return 0 if len(out["found"]) > 0 else 1


# Defines the function cmd_batch.
def cmd_batch(args: argparse.Namespace) -> int:
	# Delegate to scripts.sdr_lora_batch_decode
# Imports specific objects with 'from .sdr_lora_batch_decode import main as batch_main'.
	from .sdr_lora_batch_decode import main as batch_main
# Executes the statement `argv = [`.
	argv = [
# Executes the statement `'sdr_lora_batch_decode.py',`.
		'sdr_lora_batch_decode.py',
# Closes the previously opened list indexing or literal.
	]
# Begins a conditional branch to check a condition.
	if args.fast:
# Executes the statement `argv.append('--fast')`.
		argv.append('--fast')
# Begins a conditional branch to check a condition.
	if args.roots:
# Executes the statement `argv.append('--roots')`.
		argv.append('--roots')
# Executes the statement `argv.extend(args.roots)`.
		argv.extend(args.roots)
# Begins a conditional branch to check a condition.
	if args.out:
# Executes the statement `argv.extend(['--out', args.out])`.
		argv.extend(['--out', args.out])
# Executes the statement `old = sys.argv`.
	old = sys.argv
# Begins a block that monitors for exceptions.
	try:
# Executes the statement `sys.argv = argv`.
		sys.argv = argv
# Returns the computed value to the caller.
		return batch_main()
# Defines cleanup code that always runs after try/except.
	finally:
# Executes the statement `sys.argv = old`.
		sys.argv = old


# Defines the function build_parser.
def build_parser() -> argparse.ArgumentParser:
# Executes the statement `p = argparse.ArgumentParser(description="sdr_lora-only CLI")`.
	p = argparse.ArgumentParser(description="sdr_lora-only CLI")
# Executes the statement `sub = p.add_subparsers(dest='command', required=True)`.
	sub = p.add_subparsers(dest='command', required=True)

	# decode
# Executes the statement `pd = sub.add_parser('decode', help='Decode a single IQ file using external/sdr_lora')`.
	pd = sub.add_parser('decode', help='Decode a single IQ file using external/sdr_lora')
# Executes the statement `pd.add_argument('input', help='Path to .cf32 file')`.
	pd.add_argument('input', help='Path to .cf32 file')
# Executes the statement `pd.add_argument('--meta', help='Path to metadata JSON (defaults to sidecar .json if present)')`.
	pd.add_argument('--meta', help='Path to metadata JSON (defaults to sidecar .json if present)')
# Executes the statement `pd.add_argument('--sf', type=int, help='Spreading factor (fallback if no meta)')`.
	pd.add_argument('--sf', type=int, help='Spreading factor (fallback if no meta)')
# Executes the statement `pd.add_argument('--bw', type=int, help='Bandwidth in Hz (fallback if no meta)')`.
	pd.add_argument('--bw', type=int, help='Bandwidth in Hz (fallback if no meta)')
# Executes the statement `pd.add_argument('--fs', type=int, help='Sample rate in Hz (fallback if no meta)')`.
	pd.add_argument('--fs', type=int, help='Sample rate in Hz (fallback if no meta)')
# Executes the statement `pd.add_argument('--fast', action='store_true', help='Speed up by limiting search and disabling heavy fallbacks')`.
	pd.add_argument('--fast', action='store_true', help='Speed up by limiting search and disabling heavy fallbacks')
# Executes the statement `pd.add_argument('-v', '--verbose', action='store_true', help='Pretty-print full JSON output')`.
	pd.add_argument('-v', '--verbose', action='store_true', help='Pretty-print full JSON output')
# Executes the statement `pd.set_defaults(func=cmd_decode)`.
	pd.set_defaults(func=cmd_decode)

	# batch
# Executes the statement `pb = sub.add_parser('batch', help='Batch-decode vectors and summarize results')`.
	pb = sub.add_parser('batch', help='Batch-decode vectors and summarize results')
# Executes the statement `pb.add_argument('--fast', action='store_true', help='Speed up by limiting search and disabling heavy fallbacks')`.
	pb.add_argument('--fast', action='store_true', help='Speed up by limiting search and disabling heavy fallbacks')
# Executes the statement `pb.add_argument('--roots', nargs='*', default=[`.
	pb.add_argument('--roots', nargs='*', default=[
# Executes the statement `'golden_vectors_demo', 'golden_vectors_demo_batch', 'vectors'`.
		'golden_vectors_demo', 'golden_vectors_demo_batch', 'vectors'
# Executes the statement `])`.
	])
# Executes the statement `pb.add_argument('--out', default='results/sdr_lora_batch.json')`.
	pb.add_argument('--out', default='results/sdr_lora_batch.json')
# Executes the statement `pb.set_defaults(func=cmd_batch)`.
	pb.set_defaults(func=cmd_batch)

# Returns the computed value to the caller.
	return p


# Defines the function main.
def main() -> int:
# Executes the statement `parser = build_parser()`.
	parser = build_parser()
# Executes the statement `args = parser.parse_args()`.
	args = parser.parse_args()
# Returns the computed value to the caller.
	return args.func(args)


# Begins a conditional branch to check a condition.
if __name__ == "__main__":
# Raises an exception to signal an error.
	raise SystemExit(main())
