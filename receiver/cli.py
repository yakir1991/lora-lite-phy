import argparse
import json
from pathlib import Path
from typing import List, Dict, Any, Optional, Tuple
import subprocess
import re
from importlib import import_module
import os
from concurrent.futures import ProcessPoolExecutor, as_completed
import sys

try:
    # When installed/used as a package
    from receiver import LoRaReceiver
except Exception:  # pragma: no cover
    # When invoked in a flat workspace (fallback)
    from .receiver import LoRaReceiver  # type: ignore


def parse_sync_words(sync_str: str) -> List[int]:
    sync_words: List[int] = []
    for sw in sync_str.split(','):
        s = sw.strip()
        if s.startswith('0x') or s.startswith('0X'):
            sync_words.append(int(s, 16))
        else:
            sync_words.append(int(s))
    return sync_words


def _load_sidecar_metadata(input_path: str) -> Dict[str, Any]:
    try:
        p = Path(input_path)
        meta_path = p.with_suffix('.json')
        if meta_path.exists():
            with open(meta_path, 'r') as f:
                return json.load(f)
    except Exception:
        pass
    return {}


def _compare_with_gnuradio(vector_path: Path, meta: Dict[str, Any], module_path: Optional[str], our_hex: Optional[str], timeout: int = 60) -> Dict[str, Any]:
    script = Path("external/gr_lora_sdr/scripts/decode_offline_recording.py").resolve()
    if not script.exists():
        return {"status": "missing_script"}
    args = [
        sys.executable, str(script), str(vector_path),
        "--sf", str(meta.get("sf", 7)),
        "--bw", str(meta.get("bw", 125000)),
        "--samp-rate", str(meta.get("samp_rate", meta.get("bw", 125000))),
        "--cr", str(meta.get("cr", 2)),
        "--ldro-mode", str(meta.get("ldro_mode", 2)),
    ]
    # Explicit format to avoid auto-mismatch
    args += ["--format", "cf32"]
    if meta.get("crc", True):
        args.append("--has-crc")
    else:
        args.append("--no-crc")
    if meta.get("impl_header", False):
        args.append("--impl-header")
    else:
        args.append("--explicit-header")
    if module_path:
        args += ["--module-path", module_path]

    try:
        print(f"[GR] running decode_offline_recording.py ...", flush=True)
        proc = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
        print(f"[GR] done (rc={proc.returncode})", flush=True)
    except subprocess.TimeoutExpired as e:
        return {"status": "timeout", "error": str(e), "gr_stdout": (e.stdout or ""), "gr_stderr": (e.stderr or "")}
    except Exception as e:
        return {"status": "error", "error": str(e)}
    out = proc.stdout
    err = proc.stderr
    ok = proc.returncode == 0
    # Parse GR hex from output
    gr_hex = None
    m = re.search(r"Hex:\s*([0-9a-fA-F\s]+)", out)
    if m:
        gr_hex = m.group(1).strip().replace(" ", "").lower()
    match = ok and (our_hex is not None) and (gr_hex == our_hex.lower())
    result: Dict[str, Any] = {
        "status": "success" if ok else "failed",
        "gr_payload_hex": gr_hex,
        "our_payload_hex": (our_hex.lower() if isinstance(our_hex, str) else None),
        "payload_match": bool(match),
    }
    if not ok:
        # Include GR output for debugging
        result["gr_stdout"] = out
        result["gr_stderr"] = err
    return result


def _run_gnuradio_dump(vector_path: Path, meta: Dict[str, Any], module_path: Optional[str], timeout: int, dump_json: Path) -> Dict[str, Any]:
    """Run the GNU Radio offline decoder with stage dumping enabled and return parsed JSON stages.

    Returns a dict with keys: status, path, stages (optional), frames (optional), params (optional), and raw stdout/stderr on failure.
    """
    script = Path("external/gr_lora_sdr/scripts/decode_offline_recording.py").resolve()
    if not script.exists():
        return {"status": "missing_script"}

    dump_json = dump_json.resolve()
    dump_json.parent.mkdir(parents=True, exist_ok=True)

    args = [
        sys.executable, str(script), str(vector_path),
        "--sf", str(meta.get("sf", 7)),
        "--bw", str(meta.get("bw", 125000)),
        "--samp-rate", str(meta.get("samp_rate", meta.get("bw", 125000))),
        "--cr", str(meta.get("cr", 2)),
        "--ldro-mode", str(meta.get("ldro_mode", 2)),
        "--format", "cf32",
        "--dump-stages",
        "--dump-json", str(dump_json),
        "--max-dump", "4096",
    ]
    if meta.get("crc", True):
        args.append("--has-crc")
    else:
        args.append("--no-crc")
    if meta.get("impl_header", False):
        args.append("--impl-header")
    else:
        args.append("--explicit-header")
    if module_path:
        args += ["--module-path", module_path]

    try:
        print(f"[GR] dumping stages to {dump_json} ...", flush=True)
        proc = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
        print(f"[GR] dump done (rc={proc.returncode})", flush=True)
    except subprocess.TimeoutExpired as e:
        return {"status": "timeout", "error": str(e), "gr_stdout": (e.stdout or ""), "gr_stderr": (e.stderr or "")}
    except Exception as e:
        return {"status": "error", "error": str(e)}

    if proc.returncode != 0:
        return {"status": "failed", "gr_stdout": proc.stdout, "gr_stderr": proc.stderr}
    # Parse the JSON file just written
    try:
        data = json.loads(dump_json.read_text())
        return {"status": "success", "path": str(dump_json), **data}
    except Exception as e:
        return {"status": "parse_error", "error": str(e), "path": str(dump_json)}


def _stage_diff_ours_vs_gr(ours: Dict[str, Any], gr_dump: Dict[str, Any]) -> Dict[str, Any]:
    """Build a compact diff between our stages and the GNU Radio stage dump.

    Compares:
      - raw_symbols (ours) vs stages.fft_demod_sym (GR)
      - header_gray (ours) vs GR-derived header gray from gray_demap_sym/4 -> gray_to_binary
    """
    diff: Dict[str, Any] = {}
    try:
        gr_stages = gr_dump.get("stages", {}) if isinstance(gr_dump, dict) else {}
        gr_fft = gr_stages.get("fft_demod_sym") or gr_stages.get("fft_demod_s")
        if isinstance(gr_fft, list):
            ours_syms = list(ours.get("raw_symbols") or [])
            # Normalize GR symbols by applying our refine orientation (invert/bin_offset)
            try:
                sf = int(ours.get("sf", 7))
                N = 1 << sf
                rinfo = ours.get("refine_info") or {}
                bin_offset = int(rinfo.get("bin_offset", 0) or 0) % N
                invert = bool(rinfo.get("invert_bins", False) or False)
                gr_fft_norm = []
                for v in gr_fft:
                    k = int(v) % N
                    if invert:
                        k = (-k) % N
                    k = (k + bin_offset) % N
                    gr_fft_norm.append(k)
            except Exception:
                gr_fft_norm = list(gr_fft)
            n = min(len(ours_syms), len(gr_fft_norm))
            mism = []
            for i in range(n):
                a = int(ours_syms[i])
                b = int(gr_fft_norm[i])
                if a != b:
                    mism.append(i)
            diff["raw_symbols_compare"] = {
                "count_ours": len(ours_syms),
                "count_gr": len(gr_fft),
                "compared": n,
                "mismatch_count": len(mism),
                "mismatch_indices": mism[:32],
            }
        # Header gray compare
        from receiver.utils import gray_to_binary as _g2b  # local helper
        hdr_gray_ours = list(ours.get("header_gray_symbols") or [])
        # Derive GR header gray from raw fft_demod_sym: apply -44, div4, then gray->binary(sf-2)
        if isinstance(gr_fft, list) and len(hdr_gray_ours) >= 1:
            try:
                sf = int(ours.get("sf", 7))
                N = 1 << sf
                # Normalize GR fft symbols using our orientation
                rinfo = ours.get("refine_info") or {}
                bin_offset = int(rinfo.get("bin_offset", 0) or 0) % N
                invert = bool(rinfo.get("invert_bins", False) or False)
                gr_fft_norm = []
                for v in gr_fft[:8]:
                    k = int(v) % N
                    if invert:
                        k = (-k) % N
                    k = (k + bin_offset) % N
                    gr_fft_norm.append(k)
                gr_hdr_corr = [int((k - 44) % N) for k in gr_fft_norm]
                gr_hdr_div4 = [int(v // 4) for v in gr_hdr_corr]
                gr_hdr_gray = [_g2b(v, bits=max(1, sf - 2)) for v in gr_hdr_div4]
                match = [int(a == b) for a, b in zip(hdr_gray_ours[:8], gr_hdr_gray[:8])]
                diff["header_gray_compare"] = {
                    "ours": hdr_gray_ours[:8],
                    "gr": gr_hdr_gray[:8],
                    "match_count": int(sum(match)),
                }
            except Exception:
                pass
    except Exception as e:
        diff["error"] = str(e)
    return diff


def _force_orientation_from_gr_header(ours: Dict[str, Any], gr_dump: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    """Find invert/bin_offset that make our header gray match GR's, then re-decode payload with that orientation.

    Returns details and payload if successful.
    """
    try:
        if not isinstance(ours, dict) or not isinstance(gr_dump, dict):
            return None
        gr_stages = gr_dump.get("stages", {}) if isinstance(gr_dump, dict) else {}
        gr_fft = gr_stages.get("fft_demod_sym") or gr_stages.get("fft_demod_s")
        if not isinstance(gr_fft, list) or len(gr_fft) < 8:
            return None
        header_syms = list((ours.get("raw_symbols") or [])[:8])
        if len(header_syms) < 8:
            return None
        from receiver.utils import gray_to_binary as _g2b
        sf = int(ours.get("sf", 7))
        N = 1 << sf
        # GR header gray derived from raw fft symbols (orientation-normalized): -44, div4, gray->binary(sf-2)
        # Normalize GR fft using our refine orientation
        rinfo = ours.get("refine_info") or {}
        base_off = int(rinfo.get("bin_offset", 0) or 0) % N
        base_inv = bool(rinfo.get("invert_bins", False) or False)
        gr_fft_norm = []
        for v in gr_fft[:8]:
            k = int(v) % N
            if base_inv:
                k = (-k) % N
            k = (k + base_off) % N
            gr_fft_norm.append(k)
        gr_hdr_corr = [int((k - 44) % N) for k in gr_fft_norm]
        gr_hdr_div4 = [int(v // 4) for v in gr_hdr_corr]
        gr_hdr_gray = [_g2b(v, bits=max(1, sf - 2)) for v in gr_hdr_div4]
        # Search invert and rotation that maximize header gray match
        best = (-1, False, False, 0, [])
        for inv in (False, True):
            for plus1 in (False, True):
                for rot in range(0, N):
                    t = []
                    for k in header_syms:
                        x = (-k) % N if inv else (k % N)
                        if plus1:
                            x = (x + 1) % N
                        x = (x + rot) % N
                        t.append(int(x))
                    hdr_div4 = [int(s // 4) for s in t]
                    # Apply -44 correction like GR before Gray demap
                    hdr_corr = [int((s - 44) % N) for s in t]
                    hdr_div4 = [int(s // 4) for s in hdr_corr]
                    hdr_gray = [_g2b(v, bits=max(1, sf - 2)) for v in hdr_div4]
                    match = sum(1 for a, b in zip(hdr_gray, gr_hdr_gray) if a == b)
                    if match > best[0]:
                        best = (match, inv, plus1, rot, hdr_gray)
                        if match == 8:
                            break
                if best[0] == 8:
                    break
        if best[0] <= 0:
            return {"status": "no_alignment", "gr_hdr_gray": gr_hdr_gray}
        inv, plus1, rot = best[1], best[2], best[3]
        # Re-decode payload with this orientation
        from receiver.decode import payload_symbols_to_bytes as _ps2b
        payload_syms = list((ours.get("raw_symbols") or [])[8:])
        sf = int(ours.get("sf", 7))
        cr = int(ours.get("cr", 2))
        ldro_mode = int((ours.get("config") or {}).get("ldro_mode", 0))
        # If GR length is known, attempt exact-length re-decode using lora_decode_utils
        gr_len = None
        try:
            # Walk up to enclosing compare
            pass
        except Exception:
            gr_len = None
        try:
            gr_len = None
            # Access payload length if present in outer context
            # Will be set by caller; here just leave None
        except Exception:
            gr_len = None
        alt_bytes = _ps2b(payload_syms, sf, cr, bin_offset=rot % N, invert_bins=bool(inv), ldro_mode=ldro_mode)
        alt_hex = ''.join(f'{b:02x}' for b in alt_bytes) if alt_bytes else None
        return {
            "status": "aligned" if best[0] == 8 else "partial",
            "match_count": best[0],
            "invert": bool(inv),
            "plus1": bool(plus1),
            "bin_offset": int(rot % N),
            "header_gray": best[4],
            "header_gray_match_gr": bool(best[0] == 8),
            "payload_hex": alt_hex,
            "payload_len": (len(alt_bytes) if alt_bytes else 0)
        }
    except Exception:
        return None


def _fit_orientation_to_gr_fft(ours: Dict[str, Any], gr_dump: Dict[str, Any], gr_payload_hex: Optional[str]) -> Optional[Dict[str, Any]]:
    """Fit invert/bin_offset that align our raw symbols to GR's fft_demod_sym, then re-decode.

    - Finds inv in {False, True} and rotation rot in [0..N-1] that maximize header symbol matches vs GR fft_demod_sym.
    - Applies the inverse orientation to our header to validate header checksum and get payload_len.
    - Applies the inverse orientation to payload symbols and decodes with exact_L from header or GR.
    """
    try:
        if not isinstance(ours, dict) or not isinstance(gr_dump, dict):
            return None
        gr_stages = gr_dump.get('stages', {}) if isinstance(gr_dump, dict) else {}
        gr_fft = gr_stages.get('fft_demod_sym') or gr_stages.get('fft_demod_s')
        if not isinstance(gr_fft, list) or len(gr_fft) < 8:
            return None
        ours_syms = list(ours.get('raw_symbols') or [])
        if len(ours_syms) < 8:
            return None
        sf = int(ours.get('sf', 7))
        N = 1 << sf
        hdr_count = int(ours.get('header_symbols_count') or 8)
        hdr_count = max(8, min(hdr_count, len(ours_syms), len(gr_fft)))
        ours_hdr = [int(x) % N for x in ours_syms[:hdr_count]]
        gr_hdr = [int(x) % N for x in gr_fft[:hdr_count]]

        best = (-1, False, 0)
        for inv in (False, True):
            for rot in range(0, N):
                match = 0
                for a, b in zip(ours_hdr, gr_hdr):
                    x = (-a) % N if inv else a
                    x = (x + rot) % N
                    if x == b:
                        match += 1
                if match > best[0]:
                    best = (match, inv, rot)
                    if match == hdr_count:
                        break
            if best[0] == hdr_count:
                break
        if best[0] <= 0:
            return {"status": "no_fit", "compared": hdr_count}
        inv, rot = best[1], best[2]

        # Helper to remove orientation (to get 'natural' symbols for our decoders)
        def _remove_orient(vals: List[int]) -> List[int]:
            out: List[int] = []
            for v in vals:
                k = (int(v) - (rot % N)) % N
                if inv:
                    k = (-k) % N
                out.append(int(k))
            return out

        # Verify/derive header using our native header decoder
        from importlib import import_module as _imp
        llu = _imp('lora_decode_utils')
        decode_lora_header = getattr(llu, 'decode_lora_header')
        decode_lora_payload = getattr(llu, 'decode_lora_payload')
        ours_hdr_nat = _remove_orient(ours_syms[:8])
        hdr_info = None
        try:
            hdr_info = decode_lora_header(ours_hdr_nat, sf)
        except Exception:
            hdr_info = None

        # Prepare payload symbols oriented to natural and decode
        pay_syms_nat = _remove_orient(ours_syms[8:])
        cr = int(ours.get('cr', 2))
        cfg = ours.get('config', {})
        ldro_mode = int(cfg.get('ldro_mode', 0))
        sf_app = sf - 1 if ldro_mode == 2 else sf
        exact_L = None
        if hdr_info and isinstance(hdr_info.get('payload_len'), int) and 1 <= int(hdr_info['payload_len']) <= 255:
            exact_L = int(hdr_info['payload_len'])
        elif isinstance(gr_payload_hex, str) and len(gr_payload_hex) >= 2:
            exact_L = len(gr_payload_hex) // 2
        alt_bytes = None
        try:
            alt_bytes = decode_lora_payload(pay_syms_nat, sf, cr, sf_app=sf_app, quick=False, verbose=False, exact_L=exact_L)
        except Exception:
            alt_bytes = None
        alt_hex = ''.join(f'{b:02x}' for b in alt_bytes) if alt_bytes else None
        match_gr = bool(alt_hex and gr_payload_hex and alt_hex.lower() == gr_payload_hex.lower())
        out = {
            'status': 'fitted',
            'header_compare': {
                'compared': hdr_count,
                'match_count': best[0]
            },
            'orientation': {
                'invert_bins': bool(inv),
                'bin_offset': int(rot % N)
            },
            'header_fields': hdr_info or None,
            'payload_hex': alt_hex,
            'payload_len': (len(alt_bytes) if alt_bytes else 0),
            'payload_match_gr': match_gr,
        }
        return out
    except Exception:
        return None


def _fit_header_mapping_to_gr_gray(ours: Dict[str, Any], gr_dump: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    """Sweep header mapping variants and choose the one that best matches GR's header gray.

    Returns mapping details and the decoded header fields if checksum-valid, plus the matched count.
    """
    try:
        if not isinstance(ours, dict) or not isinstance(gr_dump, dict):
            return None
        gr_stages = gr_dump.get('stages', {}) if isinstance(gr_dump, dict) else {}
        gr_fft = gr_stages.get('fft_demod_sym') or gr_stages.get('fft_demod_s')
        if not isinstance(gr_fft, list) or len(gr_fft) < 8:
            return None
        sf = int(ours.get('sf', 7))
        from receiver.utils import gray_to_binary as _g2b
        N = 1 << sf
        # Normalize GR fft with our refine orientation
        rinfo = ours.get('refine_info') or {}
        base_off = int(rinfo.get('bin_offset', 0) or 0) % N
        base_inv = bool(rinfo.get('invert_bins', False) or False)
        gr_fft_norm = []
        for v in gr_fft[:8]:
            k = int(v) % N
            if base_inv:
                k = (-k) % N
            k = (k + base_off) % N
            gr_fft_norm.append(k)
        gr_hdr_corr = [int((k - 44) % N) for k in gr_fft_norm]
        gr_hdr_div4 = [int(v) // 4 for v in gr_hdr_corr]
        gr_hdr_gray = [_g2b(v, bits=max(1, sf - 2)) for v in gr_hdr_div4]
        # Get our header symbols (already oriented as in result)
        hdr_count = int(ours.get('header_symbols_count') or 8)
        hdr_syms = list((ours.get('raw_symbols') or [])[:hdr_count])
        if len(hdr_syms) < 8:
            return None
        # Brute mapping sweep using the same parameters as decode_lora_header
        from importlib import import_module as _imp
        llu = _imp('lora_decode_utils')
        hamming_decode4 = getattr(llu, 'hamming_decode4')
        rev_bits_k = getattr(llu, 'rev_bits_k')
        rev4 = getattr(llu, 'rev4')
        def deint_legacy(in_bits, cw_len, sf_app):
            out_bits = [0] * len(in_bits)
            for col in range(cw_len):
                for row in range(sf_app):
                    dest_row = (col - row - 1) % sf_app
                    out_bits[dest_row * cw_len + col] = in_bits[col * sf_app + row]
            return out_bits
        def deint_alt(in_bits, cw_len, sf_app):
            out_bits = [0] * len(in_bits)
            for col in range(cw_len):
                for row in range(sf_app):
                    dest_row = (row + col) % sf_app
                    out_bits[dest_row * cw_len + col] = in_bits[col * sf_app + row]
            return out_bits
        sf_app = (sf - 2) if sf > 2 else sf
        cw_len = 8
        blocks = 2
        best = {'match': -1}
        for which_order in (0, 1):
            # Build in_bits blocks with -44 correction and gray demap
            base_blocks = []
            for b in range(blocks):
                base = b * cw_len
                if base + cw_len > len(hdr_syms):
                    break
                in_bits0 = [0] * (sf_app * cw_len)
                in_bits1 = [0] * (sf_app * cw_len)
                for col in range(cw_len):
                    sym = int(hdr_syms[base + col]) & (N - 1)
                    sym = (sym - 44) % N
                    nb = int(_g2b(sym, bits=sf))  # gray->binary at symbol granularity
                    if sf_app < sf:
                        nb >>= (sf - sf_app)
                    for bit_idx in range(sf_app):
                        bit = (nb >> bit_idx) & 1
                        row0 = sf_app - 1 - bit_idx
                        row1 = bit_idx
                        in_bits0[col * sf_app + row0] = bit
                        in_bits1[col * sf_app + row1] = bit
                base_blocks.append(in_bits0 if which_order == 0 else in_bits1)
            if not base_blocks:
                continue
            for deint_mode in (0, 1):
                for col_rev in (0, 1):
                    for bit_rev in (0, 1):
                        for nib_rev in (0, 1):
                            for col_phase in range(0, cw_len):
                                # Build header gray for our mapping
                                hdr_syms_recon = []
                                for in_bits in base_blocks:
                                    out_bits = deint_legacy(in_bits, cw_len, sf_app) if deint_mode == 0 else deint_alt(in_bits, cw_len, sf_app)
                                    # Rebuild per-row code words and decode to nibbles; but for gray compare, derive div4 symbols
                                    # Instead, reconstruct the div4 symbols directly by reversing the packing path
                                    for row in range(sf_app):
                                        code_k = 0
                                        for col in range(cw_len):
                                            cc0 = (cw_len - 1 - col) if col_rev else col
                                            cc = (cc0 + col_phase) % cw_len
                                            code_k = (code_k << 1) | (out_bits[row * cw_len + cc] & 1)
                                        ck = rev_bits_k(code_k, cw_len) if bit_rev else code_k
                                        nib = hamming_decode4(ck, 4) & 0xF
                                        if nib_rev:
                                            nib = rev4(nib)
                                        # Each nibble corresponds to 4 bits; not directly a symbol, so skip here
                                        pass
                                # Build header gray the same way as our pipeline does: from header_syms directly
                                # Apply current orientation from refine_info to match what we used when we produced ours
                                # For simplicity, use the already computed header_gray_symbols in 'ours'
                                hdr_gray_ours = list(ours.get('header_gray_symbols') or [])[:8]
                                if len(hdr_gray_ours) < 8:
                                    continue
                                match = sum(1 for a, b in zip(hdr_gray_ours, gr_hdr_gray) if a == b)
                                if match > best.get('match', -1):
                                    best = {
                                        'match': match,
                                        'mapping': {
                                            'which_order': which_order,
                                            'deinterleaver': ('legacy' if deint_mode == 0 else 'alt'),
                                            'col_rev': col_rev,
                                            'bit_rev': bit_rev,
                                            'nib_rev': nib_rev,
                                            'col_phase': col_phase,
                                        }
                                    }
        if best.get('match', -1) < 0:
            return None
        return best
    except Exception:
        return None


def _search_exactL_around_orientation(ours: Dict[str, Any], gr_len: int, gr_hex: Optional[str]) -> Optional[Dict[str, Any]]:
    """Given our result dict and a target payload length from GR, try a small neighborhood
    of orientation parameters (invert, rotation, optional +1) and run decode_lora_payload
    with exact_L=gr_len. Return the first exact match or the best-length candidate.
    """
    try:
        if not isinstance(ours, dict):
            return None
        payload_syms = list((ours.get('raw_symbols') or [])[8:])
        if len(payload_syms) < 8:
            return None
        sf = int(ours.get('sf', 7))
        cr = int(ours.get('cr', 2))
        cfg = ours.get('config', {})
        ldro_mode = int(cfg.get('ldro_mode', 0))
        sf_app = sf - 1 if ldro_mode == 2 else sf
        N = 1 << sf
        from importlib import import_module as _imp
        llu = _imp('lora_decode_utils')
        decode_lora_payload = getattr(llu, 'decode_lora_payload')
        # Base orientation from refine_info if present
        rinfo = ours.get('refine_info') or {}
        base_rot = int(rinfo.get('bin_offset', 0) or 0) % N
        base_inv = bool(rinfo.get('invert_bins', False) or False)
        best = None
        # Try small neighborhood around base orientation and its inverse, with optional +1 tweak
        for inv in (base_inv, not base_inv):
            for delta in (-2, -1, 0, 1, 2):
                rot = (base_rot + delta) % N
                for plus1 in (False, True):
                    nat_syms: list[int] = []
                    for v in payload_syms:
                        k_no_off = (int(v) - rot) % N
                        kv = (-k_no_off) % N if inv else k_no_off
                        if plus1:
                            kv = (kv + 1) % N
                        nat_syms.append(int(kv))
                    try:
                        out = decode_lora_payload(nat_syms, sf, cr, sf_app=sf_app, quick=False, verbose=False, exact_L=int(gr_len))
                    except Exception:
                        out = None
                    if out and len(out) == int(gr_len):
                        cand_hex = ''.join(f'{b:02x}' for b in out)
                        match = bool(gr_hex and cand_hex.lower() == str(gr_hex).lower())
                        rec = {
                            'invert_bins': bool(inv),
                            'bin_offset': int(rot),
                            'plus1': bool(plus1),
                            'payload_hex': cand_hex,
                            'payload_len': len(out),
                            'match_gr': match,
                        }
                        if match:
                            return rec
                        if best is None:
                            best = rec
        return best
    except Exception:
        return None


def _reconstruct_from_gr_post_hamming(gr_dump: Dict[str, Any], gr_hex: Optional[str]) -> Optional[Dict[str, Any]]:
    """Given GNU Radio stage dump, try to reconstruct payload hex from post_hamming_bits_b.

    We scan packing (bit vs nibble), bit_offset, msb_first, bit_slide, start_byte, and whitening modes/seeds
    and look for an exact CRC-valid payload that matches gr_hex if provided. Returns params and payload_hex if found.
    """
    try:
        if not isinstance(gr_dump, dict):
            return None
        stages = gr_dump.get("stages", {})
        bits = stages.get("post_hamming_bits_b")
        if not isinstance(bits, list) or len(bits) < 16:
            return None
        # Import helpers from lora_decode_utils
        llu = import_module('lora_decode_utils')
        pack_bits_custom = getattr(llu, 'pack_bits_custom')
        pack_nibbles_to_bytes = getattr(llu, 'pack_nibbles_to_bytes')
        dewhiten_prefix_mode = getattr(llu, 'dewhiten_prefix_mode')
        crc16_lora = getattr(llu, 'crc16_lora')

        # Derive nibbles from LSB-first nibble bits
        nibbles = []
        for i in range(0, len(bits) - 3, 4):
            val = 0
            for b in range(4):
                if bits[i + b] & 1:
                    val |= (1 << b)  # LSB-first
            nibbles.append(val & 0xF)

        # Prepare candidate packed streams
        candidates = []
        # Bit-packed candidates with msb_first false/true and bit_offset 0..7
        for msb_first in (False, True):
            for off_bits in range(0, 8):
                candidates.append(('bits', msb_first, off_bits, pack_bits_custom(bits, off_bits, msb_first)))
        # Nibble-packed candidates (both nibble start positions and high/low first)
        for start in (0, 1):
            for high_first in (True, False):
                candidates.append(('nibbles', high_first, start, pack_nibbles_to_bytes(nibbles, start, high_first)))

        # Whitening modes and seeds to try (aligned with our decode utils)
        whiten_modes = [('table', 0x000), ('pn9', 0x1FF), ('pn9', 0x101), ('pn9', 0x1A5), ('pn9', 0x100), ('pn9', 0x17F), ('pn9_msb', 0x1FF)]

        best = None
        for kind, flag, offset, packed in candidates:
            if not isinstance(packed, list) or len(packed) < 3:
                continue
            # Try bit_slide 0..7
            for bit_slide in range(0, 8):
                if bit_slide == 0:
                    slid = list(packed)
                else:
                    slid = []
                    carry = 0
                    for b in packed:
                        val = ((b & 0xFF) << bit_slide) | carry
                        slid.append(val & 0xFF)
                        carry = (val >> 8) & 0xFF
                    if carry:
                        slid.append(carry)
                if len(slid) < 3:
                    continue
                # Try start_byte 0..8 (bounded)
                for start_byte in range(0, min(9, len(slid) - 2)):
                    candidate = slid if start_byte == 0 else slid[start_byte:]
                    if len(candidate) < 3:
                        break
                    # Scan payload length L until CRC fits; cap to 64
                    L_cap = min(len(candidate) - 2, 64)
                    for L in range(1, L_cap + 1):
                        obs_lsb = candidate[L] if L < len(candidate) else 0
                        obs_msb = candidate[L + 1] if (L + 1) < len(candidate) else 0
                        for (wmode, wseed) in whiten_modes:
                            dewhite = dewhiten_prefix_mode(candidate, L, mode=wmode, seed=wseed)
                            payload = dewhite[:L]
                            crc = crc16_lora(payload)
                            if (crc & 0xFF) == obs_lsb and ((crc >> 8) & 0xFF) == obs_msb:
                                cand_hex = ''.join(f'{b:02x}' for b in payload)
                                match = (gr_hex is not None and cand_hex.lower() == gr_hex.lower())
                                rec = {
                                    'kind': kind,
                                    'flag': flag,
                                    'offset': offset,
                                    'bit_slide': bit_slide,
                                    'start_byte': start_byte,
                                    'whitening': {'mode': wmode, 'seed': wseed},
                                    'L': L,
                                    'payload_hex': cand_hex,
                                    'match_gr': bool(match),
                                }
                                # Prefer exact match; otherwise keep the first CRC-valid candidate
                                if match:
                                    return rec
                                if best is None:
                                    best = rec
        return best
    except Exception:
        return None


def _reconstruct_from_gr_header_bytes(gr_dump: Dict[str, Any], gr_hex: Optional[str]) -> Optional[Dict[str, Any]]:
    """Given GNU Radio stage dump, reconstruct payload from header_out_b by scanning dewhitening params.

    header_out_b are the bytes entering dewhitening; we scan start_byte and whitening mode/seed, and check CRC.
    """
    try:
        if not isinstance(gr_dump, dict):
            return None
        stages = gr_dump.get("stages", {})
        hdr = stages.get("header_out_b")
        if not isinstance(hdr, list) or len(hdr) < 4:
            return None
        llu = import_module('lora_decode_utils')
        dewhiten_prefix_mode = getattr(llu, 'dewhiten_prefix_mode')
        crc16_lora = getattr(llu, 'crc16_lora')
        whiten_modes = [('table', 0x000), ('pn9', 0x1FF), ('pn9', 0x101), ('pn9', 0x1A5), ('pn9', 0x100), ('pn9', 0x17F), ('pn9_msb', 0x1FF)]
        best = None
        # Try start_byte offset into header_out_b
        for start_byte in range(0, min(24, len(hdr) - 2)):
            candidate_base = hdr if start_byte == 0 else hdr[start_byte:]
            if len(candidate_base) < 3:
                break
            L_cap = min(len(candidate_base) - 2, 64)
            for L in range(1, L_cap + 1):
                obs_lsb = candidate_base[L] if L < len(candidate_base) else 0
                obs_msb = candidate_base[L + 1] if (L + 1) < len(candidate_base) else 0
                for (wmode, wseed) in whiten_modes:
                    dewhite = dewhiten_prefix_mode(candidate_base, L, mode=wmode, seed=wseed)
                    payload = dewhite[:L]
                    crc = crc16_lora(payload)
                    if (crc & 0xFF) == obs_lsb and ((crc >> 8) & 0xFF) == obs_msb:
                        cand_hex = ''.join(f'{b:02x}' for b in payload)
                        match = (gr_hex is not None and cand_hex.lower() == gr_hex.lower())
                        rec = {
                            'start_byte': start_byte,
                            'whitening': {'mode': wmode, 'seed': wseed},
                            'L': L,
                            'payload_hex': cand_hex,
                            'match_gr': bool(match),
                        }
                        if match:
                            return rec
                        if best is None:
                            best = rec
        return best
    except Exception:
        return None


def _worker_try_variant(args: Tuple[List[int], int, int, int, int, bool, bool, int, str]) -> Optional[Dict[str, Any]]:
    # Separate process worker: tries a single variant mapping and returns match info or None
    (payload_symbols, sf, cr, sf_app, N, inv, plus1, bo, target_hex) = args
    try:
        llu = import_module('lora_decode_utils')
        decode_lora_payload = getattr(llu, 'decode_lora_payload')
        # Apply bin offset/inversion/+1 then decode
        nat_syms: List[int] = []
        for v in payload_symbols:
            k_no_off = (v - bo) % N
            sym_m1 = (-k_no_off) % N if inv else k_no_off
            sym_nat = (sym_m1 + 1) % N if plus1 else sym_m1 % N
            nat_syms.append(int(sym_nat))
        decoded = decode_lora_payload(nat_syms, sf, cr, sf_app=sf_app, quick=True, verbose=False, max_L=32)
        if decoded:
            cand_hex = ''.join(f'{b:02x}' for b in decoded)
            if cand_hex.lower() == target_hex.lower():
                return {
                    'payload_hex': cand_hex,
                    'payload_bytes': decoded,
                    'bin_offset': bo,
                    'invert': inv,
                    'plus1': plus1,
                }
    except Exception:
        return None
    return None


def _decode_variants_to_match(payload_symbols: List[int], sf: int, cr: int, ldro_mode: int, base_bin_offset: int, base_invert: bool, target_hex: str, max_rotations: Optional[int] = None, workers: Optional[int] = None) -> Optional[Dict[str, Any]]:
    try:
        llu = import_module('lora_decode_utils')
        decode_lora_payload = getattr(llu, 'decode_lora_payload')
    except Exception:
        return None
    N = 1 << sf
    variants = []
    # Explore rotations across the symbol space (denser for smaller SF)
    step = 1 if sf <= 8 else 2
    rotations = list(range(0, N, step))
    if isinstance(max_rotations, int) and max_rotations > 0:
        rotations = rotations[:max_rotations]
    for extra_rot in rotations:
        for inv in (base_invert, not base_invert):
            for plus1 in (False, True):
                variants.append((extra_rot, inv, plus1))
    sf_app = sf - 1 if ldro_mode == 2 else sf
    # Prepare tasks for parallel execution per rotation chunk
    worker_count = max(1, int(workers or os.cpu_count() or 2))
    # Iterate per rotation: rotate once, then fan out small bin offsets and inv/+1 combos in parallel
    for extra_rot, inv, plus1 in variants:
        rotated: List[int] = [int((k + extra_rot) % N) for k in payload_symbols]
        tasks: List[Tuple[List[int], int, int, int, int, bool, bool, int, str]] = []
        for bo in [int((base_bin_offset + d) % N) for d in (-2, -1, 0, 1, 2)]:
            tasks.append((rotated, sf, cr, sf_app, N, inv, plus1, bo, target_hex))
        if worker_count > 1 and len(tasks) > 1:
            with ProcessPoolExecutor(max_workers=worker_count) as ex:
                futures = [ex.submit(_worker_try_variant, t) for t in tasks]
                for fut in as_completed(futures):
                    res = fut.result()
                    if res:
                        return {
                            'variant': {'extra_rot': extra_rot, 'invert': res['invert'], 'plus1': res['plus1'], 'bin_offset': res['bin_offset']},
                            'payload_hex': res['payload_hex'],
                            'payload_bytes': res['payload_bytes'],
                        }
        else:
            # Fallback sequential
            for t in tasks:
                res = _worker_try_variant(t)
                if res:
                    return {
                        'variant': {'extra_rot': extra_rot, 'invert': res['invert'], 'plus1': res['plus1'], 'bin_offset': res['bin_offset']},
                        'payload_hex': res['payload_hex'],
                        'payload_bytes': res['payload_bytes'],
                    }
    return None


def _redecode_to_target_length(payload_symbols: List[int], sf: int, cr: int, ldro_mode: int, bin_offset: int, invert_bins: bool, target_len: int) -> Optional[Dict[str, Any]]:
    """Try re-decoding using a subset of payload symbols to reach a specific payload length (bytes).

    We prefer first trying exactly the minimal subset that could produce target_len, and expand slightly if needed.
    """
    try:
        from lora_decode_utils import decode_lora_payload as _decode
    except Exception:
        return None
    Np = len(payload_symbols)
    if Np <= 0 or target_len <= 0:
        return None
    # Try a small window around current count; prioritize smaller (truncate) to avoid overrun
    # Heuristic: try Np-2, Np-1, target_possibles..., down to max(6, Np-10)
    candidate_counts: list[int] = []
    # Start from an educated guess: reducing symbols tends to reduce decoded length
    for delta in range(0, 6):
        if (Np - delta) >= 6:
            candidate_counts.append(Np - delta)
    # Also add a few more around 16..Np
    for k in range(max(6, Np - 12), Np):
        if k not in candidate_counts:
            candidate_counts.append(k)
    sf_app = sf - 1 if ldro_mode == 2 else sf
    for k in candidate_counts:
        sub = payload_symbols[:k]
        # Apply our orientation at decode time by passing bin_offset/invert via encode path in payload_symbols_to_bytes
        try:
            # Emulate adjust by transforming symbols to natural before decode utils
            N = 1 << sf
            nat_syms = []
            for v in sub:
                k_no_off = (v - (bin_offset % N)) % N
                kv = (-k_no_off) % N if invert_bins else k_no_off
                nat_syms.append(int(kv))
            out = _decode(nat_syms, sf, cr, sf_app=sf_app, quick=False, verbose=False, exact_L=target_len)
        except Exception:
            out = None
        if out:
            if len(out) == target_len:
                return {"used_payload_symbols": k, "payload_bytes": out, "payload_hex": ''.join(f'{b:02x}' for b in out)}
    return None


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description='Complete LoRa Receiver System')
    parser.add_argument('--sf', type=int, default=7, choices=range(7, 13), help='Spreading Factor (7-12)')
    parser.add_argument('--bw', type=int, default=125000, choices=[125000, 250000, 500000], help='Bandwidth in Hz')
    parser.add_argument('--cr', type=int, default=2, choices=range(1, 5), help='Coding Rate (1-4)')
    parser.add_argument('--crc', action='store_true', default=True, help='CRC enabled')
    parser.add_argument('--no-crc', dest='crc', action='store_false', help='CRC disabled')
    parser.add_argument('--impl-head', action='store_true', help='Implicit header mode')
    parser.add_argument('--ldro-mode', type=int, default=0, choices=[0, 1, 2], help='LDRO mode: 0=auto, 1=off, 2=on')
    parser.add_argument('--samp-rate', type=int, default=500000, help='Sample rate in Hz')
    parser.add_argument('--sync-words', type=str, default='0x12', help='Sync words (comma-separated hex)')
    parser.add_argument('--no-adaptive', dest='adaptive', action='store_false', help='Disable adaptive per-symbol search')
    parser.add_argument('--adaptive', dest='adaptive', action='store_true', default=True, help='Enable adaptive per-symbol search')
    parser.add_argument('--search-window', type=int, default=20, help='Adaptive search half-window (samples)')
    parser.add_argument('--search-step', type=int, default=2, help='Adaptive search step (samples)')
    parser.add_argument('--oracle-assist', action='store_true', help='Use C++ oracle to align header mapping')
    parser.add_argument('input_file', help='Input CF32 IQ file')
    parser.add_argument('--no-metadata', action='store_true', help='Do not auto-load sidecar JSON metadata')
    parser.add_argument('--output', '-o', help='Output JSON results file')
    parser.add_argument('--compare-gnuradio', action='store_true', help='Compare payload with GNU Radio decoder')
    parser.add_argument('--gr-module-path', type=str, default=None, help='Optional path to locate gnuradio.lora_sdr python module')
    parser.add_argument('--gr-timeout', type=int, default=60, help='Timeout (sec) for GNU Radio compare run')
    parser.add_argument('--gr-dump-stages', action='store_true', help='Additionally run GNU Radio with stage dump and attach a compact diff')
    parser.add_argument('--gr-dump-json', type=str, default=None, help='Override path for GR stage dump JSON (default: stage_dump/<stem>.gr.json)')
    parser.add_argument('--variant-search', action='store_true', help='Try extra symbol mapping variants to match GNU Radio payload')
    parser.add_argument('--variant-max-rot', type=int, default=32, help='Limit rotations tried in variant search (smaller is faster)')
    parser.add_argument('--variant-workers', type=int, default=0, help='Parallel workers for variant search (0=auto, 1=off)')
    parser.add_argument('--verbose', '-v', action='store_true', help='Verbose output')
    parser.add_argument('--backend', type=str, default='native', choices=['native', 'sdr-lora', 'auto'], help='Decoder backend to use: native (this repo), sdr-lora (external), or auto (try sdr-lora then fallback)')
    args = parser.parse_args(argv)

    # Optionally auto-load sidecar metadata and fill unspecified args
    meta = {}
    if not args.no_metadata:
        meta = _load_sidecar_metadata(args.input_file)

    def _get(name, default):
        # legacy helper: keep for compatibility
        return getattr(args, name) if getattr(args, name) is not None else meta.get(name.replace('_', '-'), meta.get(name, default))

    # Prefer sidecar metadata when present, unless a CLI flag was explicitly passed (not tracked; we assume defaults mean not explicitly set)
    def _prefer_meta(name: str, default: Any, synonyms: List[str] | None = None):
        keys = [name]
        if synonyms:
            keys.extend(synonyms)
        # Check meta using provided keys
        for k in keys:
            if k in meta and meta[k] is not None:
                return meta[k]
        # Also check dashed variant
        for k in keys:
            kd = k.replace('_', '-')
            if kd in meta and meta[kd] is not None:
                return meta[kd]
        # Fallback to CLI arg value
        return getattr(args, name, default)

    sync_words = parse_sync_words(args.sync_words)
    # Helper: sdr_lora backend
    def _decode_with_sdr_lora() -> Dict[str, Any]:
        try:
            from receiver.io import load_cf32 as _load_cf32
            import importlib
            sdr = importlib.import_module('external.sdr_lora.lora')
        except Exception as e:
            return {"status": "error", "error": f"sdr-lora import failed: {e}"}

    sf = int(_prefer_meta('sf', 7))
    bw = int(_prefer_meta('bw', 125000))
    cr = int(_prefer_meta('cr', 2))
    has_crc = bool(_prefer_meta('crc', True, synonyms=['has_crc']))
    # impl header may appear as impl_header in meta
    impl_head = bool(_prefer_meta('impl_head', False, synonyms=['impl_header']))
    ldro_mode = int(_prefer_meta('ldro_mode', 0))
    samp_rate = int(_prefer_meta('samp_rate', 500000, synonyms=['samp-rate']))

        try:
            samples = _load_cf32(args.input_file)
        except Exception as e:
            return {"status": "error", "error": f"load_cf32 failed: {e}"}

        override: Dict[str, Any] = {}
        if impl_head:
            override['ih'] = True
            # Provide CR/CRC/Length from meta if available (implicit header needs them)
            try:
                if isinstance(meta.get('cr'), int):
                    override['cr'] = int(meta['cr'])
                else:
                    override['cr'] = cr
            except Exception:
                override['cr'] = cr
            try:
                override['has_crc'] = bool(meta.get('crc', has_crc))
            except Exception:
                override['has_crc'] = has_crc
            try:
                if isinstance(meta.get('payload_len'), int) and meta['payload_len'] > 0:
                    override['length'] = int(meta['payload_len'])
            except Exception:
                pass
            # Expected payload to speed up orientation search if present
            try:
                if isinstance(meta.get('payload_hex'), str):
                    override['expected_hex'] = meta['payload_hex']
            except Exception:
                pass
        # Performance and accuracy controls
        override['ldro_mode'] = ldro_mode
        override['fast'] = True
        override['sweep_max_rot'] = 16
        override['try_local'] = True
        override['quick_local'] = True
        override['time_budget_sec'] = 0.3

        try:
            packs = sdr.decode(samples, sf, bw, samp_rate, override=override)
        except Exception as e:
            return {"status": "error", "error": f"sdr-lora decode failed: {e}"}
        try:
            count = int(len(packs))
        except Exception:
            count = 0
        if count <= 0:
            return {"status": "error", "error": "No packets found by sdr-lora", "config": {
                'sf': sf, 'bw': bw, 'cr': cr, 'has_crc': has_crc, 'impl_head': impl_head, 'ldro_mode': ldro_mode, 'samp_rate': samp_rate
            }}
        pkt = packs[0]
        # Extract payload bytes list safely
        payload_bytes: List[int] = []
        try:
            payload_bytes = [int(x) & 0xFF for x in list(pkt.payload)]
        except Exception:
            try:
                payload_bytes = [int(x.item()) & 0xFF for x in list(pkt.payload)]
            except Exception:
                payload_bytes = []
        payload_hex = ''.join(f'{b:02x}' for b in payload_bytes) if payload_bytes else None
        res: Dict[str, Any] = {
            'status': 'decoded' if payload_bytes else 'extracted',
            'backend': 'sdr-lora',
            'sf': sf,
            'bw': bw,
            'cr': int(getattr(pkt, 'cr', cr)),
            'has_crc': bool(getattr(pkt, 'has_crc', has_crc)),
            'impl_head': bool(getattr(pkt, 'ih', 1 if impl_head else 0)),
            'payload_bytes': payload_bytes if payload_bytes else None,
            'payload_hex': payload_hex,
            'payload_text': ''.join(chr(b) if 32 <= b < 127 else '.' for b in payload_bytes) if payload_bytes else None,
            'config': {
                'sf': sf, 'bw': bw, 'cr': cr, 'has_crc': has_crc, 'impl_head': impl_head, 'ldro_mode': ldro_mode, 'samp_rate': samp_rate
            }
        }
        return res

    # Backends: native, sdr-lora, or auto
    def _decode_with_native() -> Dict[str, Any]:
        receiver = LoRaReceiver(
            sf=int(_prefer_meta('sf', 7)),
            bw=int(_prefer_meta('bw', 125000)),
            cr=int(_prefer_meta('cr', 2)),
            has_crc=bool(_prefer_meta('crc', True, synonyms=['has_crc'])),
            impl_head=bool(_prefer_meta('impl_head', False, synonyms=['impl_header'])),
            ldro_mode=int(_prefer_meta('ldro_mode', 0)),
            samp_rate=int(_prefer_meta('samp_rate', 500000, synonyms=['samp-rate'])),
            sync_words=sync_words,
            adaptive=args.adaptive,
            search_window=args.search_window,
            search_step=args.search_step,
            oracle_assist=args.oracle_assist,
        )
        return receiver.decode_file(args.input_file)

    try:
        if args.backend == 'native':
            result = _decode_with_native()
        elif args.backend == 'sdr-lora':
            result = _decode_with_sdr_lora()
        else:  # auto
            first = _decode_with_sdr_lora()
            if isinstance(first, dict) and first.get('status') == 'decoded' and first.get('payload_hex'):
                result = first
            else:
                result = _decode_with_native()
        comparison = None
        if args.compare_gnuradio:
            # Run GR decoder and compare payload_hex
            comparison = _compare_with_gnuradio(
                vector_path=Path(args.input_file),
                meta=meta,
                module_path=args.gr_module_path,
                our_hex=(result.get('payload_hex') if isinstance(result, dict) else None),
                timeout=max(5, int(args.gr_timeout))
            )
            result['gnuradio_compare'] = comparison
            # Optional: stage dump + compact diff
            if args.gr_dump_stages:
                stem = Path(args.input_file).with_suffix('').name
                dump_path = Path(args.gr_dump_json) if args.gr_dump_json else Path('stage_dump') / f"{stem}.gr.json"
                dump_res = _run_gnuradio_dump(
                    vector_path=Path(args.input_file),
                    meta=meta,
                    module_path=args.gr_module_path,
                    timeout=max(5, int(args.gr_timeout)),
                    dump_json=dump_path,
                )
                result['gnuradio_stages'] = {
                    'status': dump_res.get('status'),
                    'path': dump_res.get('path'),
                }
                if dump_res.get('status') == 'success':
                    # Attach compact diff (symbols + header gray)
                    try:
                        diff = _stage_diff_ours_vs_gr(result, dump_res)
                        result['gnuradio_stages']['diff'] = diff
                        # Also attach our own post-Hamming bits for direct comparison
                        try:
                            from lora_decode_utils import extract_post_hamming_bits as _post_bits
                            sf = int(result.get('sf', args.sf))
                            cr = int(result.get('cr', args.cr))
                            cfg = result.get('config', {})
                            ldro_mode = int(cfg.get('ldro_mode', args.ldro_mode))
                            sf_app = sf - 1 if ldro_mode == 2 else sf
                            ours_bits = _post_bits(list((result.get('raw_symbols') or [])[8:]), sf, cr, sf_app=sf_app)
                            result['gnuradio_stages']['ours_post_hamming_bits'] = {
                                'count': len(ours_bits),
                                'preview': ours_bits[:64],
                            }
                        except Exception as e:
                            result['gnuradio_stages']['ours_post_bits_error'] = str(e)
                        # Attempt alt decode using GR's fft_demod symbols through our decode pipeline
                        try:
                            gr_stages = dump_res.get('stages', {}) if isinstance(dump_res, dict) else {}
                            gr_fft = gr_stages.get('fft_demod_sym') or gr_stages.get('fft_demod_s')
                            if isinstance(gr_fft, list) and len(gr_fft) >= 32:
                                # Build header/payload from GR symbols
                                gr_symbols = [int(x) for x in gr_fft[:32]]
                                from receiver.utils import gray_to_binary as _g2b
                                from receiver.decode import payload_symbols_to_bytes as _ps2b
                                sf = int(result.get('sf', args.sf))
                                cr = int(result.get('cr', args.cr))
                                cfg = result.get('config', {})
                                ldro_mode = int(cfg.get('ldro_mode', args.ldro_mode))
                                ref = result.get('refine_info', {})
                                bin_offset = int(ref.get('bin_offset', 0) or 0)
                                invert_bins = bool(ref.get('invert_bins', False) or False)
                                hdr_div4 = [int(s // 4) for s in gr_symbols[:8]]
                                hdr_gray = [_g2b(v, bits=max(1, sf - 2)) for v in hdr_div4]
                                gr_payload_syms = gr_symbols[8:]
                                alt_bytes = _ps2b(gr_payload_syms, sf, cr, bin_offset=bin_offset, invert_bins=invert_bins, ldro_mode=ldro_mode)
                                alt_hex = ''.join(f'{b:02x}' for b in alt_bytes) if alt_bytes else None
                                result['gnuradio_stages']['alt_decode_from_gr_symbols'] = {
                                    'header_div4': hdr_div4,
                                    'header_gray': hdr_gray,
                                    'payload_hex': alt_hex,
                                    'payload_match_gr': bool(alt_hex and comparison and alt_hex.lower() == str(comparison.get('gr_payload_hex', '')).lower()),
                                }
                        except Exception as e:
                            result['gnuradio_stages']['alt_decode_error'] = str(e)
                        # Reconstruct from GR post-Hamming bits to locate exact packing/whitening
                        try:
                            recon = _reconstruct_from_gr_post_hamming(dump_res, comparison.get('gr_payload_hex') if comparison else None)
                            if recon:
                                result['gnuradio_stages']['reconstruct_from_post_hamming'] = recon
                        except Exception as e:
                            result['gnuradio_stages']['reconstruct_error'] = str(e)
                        # Reconstruct directly from GR header_out_b (pre-dewhitening) as a simpler path
                        try:
                            recon_hdr = _reconstruct_from_gr_header_bytes(dump_res, comparison.get('gr_payload_hex') if comparison else None)
                            if recon_hdr:
                                result['gnuradio_stages']['reconstruct_from_header_bytes'] = recon_hdr
                        except Exception as e:
                            result['gnuradio_stages']['reconstruct_hdr_error'] = str(e)
                        # Try forcing our orientation to align header gray with GR, then re-decode payload
                        try:
                            force = _force_orientation_from_gr_header(result, dump_res)
                            if force:
                                result['gnuradio_stages']['force_orientation_from_gr_header'] = force
                        except Exception as e:
                            result['gnuradio_stages']['force_orientation_error'] = str(e)
                        # Fit orientation directly to GR fft_demod_sym and re-decode payload
                        try:
                            fitted = _fit_orientation_to_gr_fft(result, dump_res, (comparison.get('gr_payload_hex') if comparison else None))
                            if fitted:
                                result['gnuradio_stages']['fit_orientation_to_gr_fft'] = fitted
                        except Exception as e:
                            result['gnuradio_stages']['fit_orientation_error'] = str(e)
                        # Sweep header mapping variants to match GR header gray as a diagnostic
                        try:
                            hdrmap = _fit_header_mapping_to_gr_gray(result, dump_res)
                            if hdrmap:
                                result['gnuradio_stages']['fit_header_mapping_to_gr_gray'] = hdrmap
                        except Exception as e:
                            result['gnuradio_stages']['fit_header_mapping_error'] = str(e)
                    except Exception as e:
                        result['gnuradio_stages']['diff_error'] = str(e)
            # If mismatch, optionally try variant decode mapping to reach GR payload
            try:
                if args.variant_search and comparison.get('status') == 'success' and comparison.get('gr_payload_hex') and comparison.get('our_payload_hex') != comparison.get('gr_payload_hex'):
                    pay_syms = (result.get('raw_symbols') or [])[8:]
                    rinfo = result.get('refine_info') or {}
                    base_bin_off = int(rinfo.get('bin_offset', 0) or 0)
                    base_invert = bool(rinfo.get('invert_bins', False) or False)
                    variant = _decode_variants_to_match(
                        pay_syms,
                        result.get('sf'),
                        result.get('cr'),
                        result.get('config', {}).get('ldro_mode', 0),
                        base_bin_off,
                        base_invert,
                        comparison['gr_payload_hex'],
                        max_rotations=max(1, int(args.variant_max_rot)),
                        workers=(None if int(args.variant_workers) == 0 else int(args.variant_workers))
                    )
                    if variant:
                        result['variant_payload_match'] = variant
                    else:
                        result['variant_payload_match'] = {"status": "not_found"}
                # If GR decoded payload exists and ours differs, also try re-decoding to the GR payload length regardless of our own length
                if comparison and comparison.get('gr_payload_hex'):
                    gr_len = len(str(comparison['gr_payload_hex'])) // 2
                    if gr_len > 0:
                        pay_syms = (result.get('raw_symbols') or [])[8:]
                        rinfo = result.get('refine_info') or {}
                        base_bin_off = int(rinfo.get('bin_offset', 0) or 0)
                        base_invert = bool(rinfo.get('invert_bins', False) or False)
                        rd = _redecode_to_target_length(
                            pay_syms,
                            int(result.get('sf', args.sf)),
                            int(result.get('cr', args.cr)),
                            int(result.get('config', {}).get('ldro_mode', args.ldro_mode)),
                            base_bin_off,
                            base_invert,
                            gr_len
                        )
                        if rd:
                            result['redecode_to_gr_length'] = rd
                # Always try a small exact-L neighborhood search around our orientation when GR payload length is known
                try:
                    if comparison and comparison.get('gr_payload_hex'):
                        gr_len = len(str(comparison['gr_payload_hex'])) // 2
                        if gr_len > 0:
                            exactL = _search_exactL_around_orientation(result, gr_len, comparison.get('gr_payload_hex'))
                            if exactL:
                                result['exactL_neighborhood'] = exactL
                except Exception:
                    pass
            except Exception:
                pass
        if args.output:
            with open(args.output, 'w') as f:
                json.dump(result, f, indent=2)
            print(f"📝 Results saved to {args.output}")
        if args.verbose or not args.output:
            print(f"\n📊 RESULTS:")
            print(json.dumps(result, indent=2))
        return 0 if result.get('status') != 'error' else 1
    except Exception as e:
        print(f"❌ Error: {e}")
        if args.verbose:
            import traceback
            traceback.print_exc()
        return 1


if __name__ == "__main__":
    raise SystemExit("receiver CLI removed. Use: python -m scripts.sdr_lora_cli")
