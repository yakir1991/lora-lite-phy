#!/usr/bin/env python3
"""Generate reference chirps OR full transmitted LoRa frames (golden vectors).

The C++ modulator block in :mod:`gnuradio.lora_sdr` generates reference
upchirp and downchirp waveforms through ``build_ref_chirps``. This script
reproduces the same computation in Python so that a user can export the
reference vectors for further processing, testing or comparison.

The script stores the upchirp (and optionally the downchirp) to disk and can
print the first few samples for quick inspection.
"""

from __future__ import annotations

import argparse
import math
import json
from pathlib import Path
from typing import Iterable, Tuple, List

import numpy as np

# Defer GNU Radio imports until frame generation is requested
gr = None  # type: ignore
blocks = None  # type: ignore
pmt = None  # type: ignore


def _infer_os_factor(samp_rate: float, bandwidth: float) -> int:
    """Derive the integer oversampling factor used by the modulator."""

    if bandwidth <= 0:
        raise ValueError("Bandwidth must be strictly positive")

    ratio = samp_rate / bandwidth
    rounded = int(round(ratio))
    if not math.isclose(ratio, rounded, rel_tol=0, abs_tol=1e-9):
        raise ValueError(
            "The sampling rate must be an integer multiple of the bandwidth "
            f"(got ratio={ratio!r})"
        )
    if rounded <= 0:
        raise ValueError("The derived oversampling factor must be positive")
    return rounded


def build_reference_chirps(sf: int, os_factor: int) -> Tuple[np.ndarray, np.ndarray]:
    """Return the reference upchirp and downchirp used by the TX chain."""

    if sf <= 0:
        raise ValueError("The spreading factor must be positive")
    if os_factor <= 0:
        raise ValueError("The oversampling factor must be positive")

    samples_per_symbol = (1 << sf) * os_factor
    n = np.arange(samples_per_symbol, dtype=np.float64)
    n_os = float(os_factor)
    symbol_bins = float(1 << sf)

    phase = 2.0 * np.pi * ((n * n) / (2.0 * symbol_bins * n_os * n_os) - 0.5 * n / n_os)
    upchirp = np.exp(1j * phase).astype(np.complex64)
    downchirp = np.conj(upchirp)
    return upchirp, downchirp


def save_chirps(
    upchirp: np.ndarray,
    downchirp: np.ndarray,
    output: Path,
    fmt: str,
    include_down: bool,
) -> Iterable[Path]:
    """Persist the computed chirps using the requested format."""

    output = output.expanduser().resolve()
    produced = []

    if fmt == "npz":
        target = output
        if target.suffix != ".npz":
            target = target.with_suffix(".npz")
        if include_down:
            np.savez(target, upchirp=upchirp, downchirp=downchirp)
        else:
            np.savez(target, upchirp=upchirp)
        produced.append(target)
    elif fmt == "npy":
        base = output
        if base.suffix:
            base = base.with_suffix("")
        up_path = base.with_name(base.name + "_up.npy")
        np.save(up_path, upchirp)
        produced.append(up_path)
        if include_down:
            down_path = base.with_name(base.name + "_down.npy")
            np.save(down_path, downchirp)
            produced.append(down_path)
    elif fmt == "txt":
        base = output
        if base.suffix:
            base = base.with_suffix("")
        up_path = base.with_name(base.name + "_up.txt")
        np.savetxt(
            up_path,
            np.column_stack((upchirp.real, upchirp.imag)),
            fmt="%.12e",
            header="real\timag",
        )
        produced.append(up_path)
        if include_down:
            down_path = base.with_name(base.name + "_down.txt")
            np.savetxt(
                down_path,
                np.column_stack((downchirp.real, downchirp.imag)),
                fmt="%.12e",
                header="real\timag",
            )
            produced.append(down_path)
    else:
        raise ValueError(f"Unsupported export format: {fmt}")

    return produced


def format_complex(value: complex) -> str:
    return f"{value.real:+.6f} {value.imag:+.6f}j"


def _parse_int_list(spec: str) -> List[int]:
    spec = spec.strip()
    if not spec:
        return []
    parts: List[int] = []
    for chunk in spec.split(","):
        chunk = chunk.strip()
        if not chunk:
            continue
        if "-" in chunk:
            a, b = chunk.split("-", 1)
            parts.extend(range(int(a), int(b) + 1))
        else:
            parts.append(int(chunk))
    return parts


def _generate_frame(
    *,
    sf: int,
    bw: int,
    samp_rate: int,
    cr: int,
    has_crc: bool,
    impl_head: bool,
    ldro_mode: int,
    payload: bytes,
    sync_word: int,
    frame_zero_padd: int,
    module_path: str | None,
) -> np.ndarray:
    # Import GNU Radio modules lazily here
    import sys, importlib
    global gr, blocks, pmt
    if gr is None:
        try:
            from gnuradio import gr as _gr, blocks as _blocks  # type: ignore
            import pmt as _pmt  # type: ignore
            gr, blocks, pmt = _gr, _blocks, _pmt
        except Exception as e:  # pragma: no cover
            raise RuntimeError(f"GNU Radio core import failed: {e}")
    if module_path:
        import os as _os
        mp_abs = _os.path.abspath(module_path)
        if mp_abs not in sys.path:
            sys.path.insert(0, mp_abs)
    try:
        lora_sdr = importlib.import_module("gnuradio.lora_sdr")
    except Exception as e:  # pragma: no cover
        raise RuntimeError(f"Failed importing gnuradio.lora_sdr: {e}")

    class _TxTop(gr.top_block):
        def __init__(self):
            super().__init__("tx_frame_export")
            self.tx = lora_sdr.lora_sdr_lora_tx(
                bw=bw,
                cr=cr,
                has_crc=has_crc,
                impl_head=impl_head,
                samp_rate=samp_rate,
                sf=sf,
                ldro_mode=ldro_mode,
                frame_zero_padd=frame_zero_padd,
                sync_word=[sync_word],
            )
            self.snk = blocks.vector_sink_c()
            # One-shot message source to inject payload deterministically
            class _OneShotSrc(gr.basic_block):
                def __init__(self, payload: bytes):
                    gr.basic_block.__init__(self, name="oneshot_msg_src", in_sig=None, out_sig=None)
                    self.message_port_register_out(pmt.intern("out"))
                    self._pl = payload
                    self._sent = False
                def start(self):  # called by scheduler once graph activates
                    if not self._sent:
                        # TX chain expects a PMT symbol (string) like examples do
                        self.message_port_pub(
                            pmt.intern("out"), pmt.intern(self._pl.decode('latin-1'))
                        )
                        self._sent = True
                    return gr.basic_block.start(self)
            # Initial dummy source (will be replaced in run_and_fetch)
            self.src = _OneShotSrc(b"")
            # Wire message path src->tx
            self.msg_connect(self.src, "out", self.tx, "in")
            # Stream path tx->sink
            self.connect(self.tx, self.snk)

        def run_and_fetch(self, data: bytes):
            # Disconnect old source message connection only
            try:
                self.msg_disconnect(self.src, "out", self.tx, "in")
            except Exception:
                pass
            # Replace source
            class _OneShotSrc(gr.basic_block):
                def __init__(self, payload: bytes):
                    gr.basic_block.__init__(self, name="oneshot_msg_src", in_sig=None, out_sig=None)
                    self.message_port_register_out(pmt.intern("out"))
                    self._pl = payload
                    self._sent = False
                def start(self):  # publish once
                    if not self._sent:
                        self.message_port_pub(pmt.intern("out"), pmt.intern(self._pl.decode('latin-1')))
                        self._sent = True
                    return gr.basic_block.start(self)
            self.src = _OneShotSrc(data)
            self.msg_connect(self.src, "out", self.tx, "in")
            # Clear prior sink data by recreating sink
            self.disconnect(self.tx, self.snk)
            self.snk = blocks.vector_sink_c()
            self.connect(self.tx, self.snk)
            # Execute graph with watchdog to avoid indefinite hang
            import time
            timeout_s = 5.0
            idle_threshold_intervals = 10  # number of consecutive intervals with no new samples
            interval = 0.05
            last_len = -1
            idle_count = 0
            start_t = time.time()
            self.start()
            try:
                while True:
                    cur_len = len(self.snk.data())
                    if cur_len == last_len:
                        idle_count += 1
                    else:
                        idle_count = 0
                        last_len = cur_len
                    if idle_count >= idle_threshold_intervals:
                        break  # stable -> done
                    if (time.time() - start_t) > timeout_s:
                        print(f"[warn] generation timeout reached after {timeout_s}s; stopping early (samples={cur_len})")
                        break
                    time.sleep(interval)
            finally:
                self.stop()
                self.wait()
            return np.array(self.snk.data(), dtype=np.complex64)

    tb = _TxTop()
    return tb.run_and_fetch(payload)


def main() -> None:
    parser = argparse.ArgumentParser(
        description=(
            "Export reference LoRa chirps OR emit full TX frames (golden vectors) for parity tests."
        ),
    )
    parser.add_argument("--sf", type=int, default=7, help="Spreading factor (default: 7)")
    parser.add_argument(
        "--bw",
        type=float,
        default=125000,
        help="Transmission bandwidth in Hz (used when deriving the oversampling factor)",
    )
    parser.add_argument(
        "--samp-rate",
        type=float,
        default=None,
        help="Sampling rate in Hz. Required when --os-factor is not provided.",
    )
    parser.add_argument(
        "--os-factor",
        type=int,
        default=None,
        help="Explicit oversampling factor (samples per symbol bin). Overrides --samp-rate/--bw.",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="Destination file or prefix. Defaults to reference_chirps_sf{sf}_os{os}.npz",
    )
    parser.add_argument(
        "--format",
        choices=("npz", "npy", "txt"),
        default="npz",
        help="File format used to save the chirps (default: npz)",
    )
    parser.add_argument(
        "--no-downchirp",
        action="store_true",
        help="Only export the reference upchirp",
    )
    parser.add_argument(
        "--preview",
        action="store_true",
        help="Print the first few samples of each chirp to stdout",
    )
    # Frame generation options
    parser.add_argument(
        "--emit-frame",
        action="store_true",
        help="Generate a full transmitted frame IQ vector instead of only chirps",
    )
    parser.add_argument(
        "--payload",
        type=str,
        default="HELLO_WORLD",
        help="ASCII payload (or hex: prefix with 0x...)",
    )
    parser.add_argument(
        "--cr", type=str, default="1", help="Coding rate (or list/range when --batch)"
    )
    parser.add_argument(
        "--sf-list",
        type=str,
        default=None,
        help="List/range of SF values when doing batch frame export (e.g. 7,8,9 or 7-9)",
    )
    parser.add_argument(
        "--cr-list",
        type=str,
        default=None,
        help="List/range of CR values (1-4) for batch frame export",
    )
    parser.add_argument(
        "--ldro-mode", type=int, default=2, choices=[0, 1, 2], help="LDRO mode (0=off,1=on,2=auto)"
    )
    parser.add_argument(
        "--ldro-list",
        type=str,
        default=None,
        help="List/range of LDRO modes when batching (e.g. 0,2 or 0-2)",
    )
    parser.add_argument(
        "--no-crc", action="store_true", help="Disable CRC for frame generation"
    )
    parser.add_argument(
        "--crc-both",
        action="store_true",
        help="When set, generate both CRC-enabled and CRC-disabled variants (ignores --no-crc for batching)",
    )
    parser.add_argument(
        "--impl-header", action="store_true", help="Use implicit header (default explicit)"
    )
    parser.add_argument(
        "--both-header",
        action="store_true",
        help="When batching, emit both explicit and implicit header variants",
    )
    parser.add_argument(
        "--batch", action="store_true", help="Batch over sf-list and cr-list to produce many golden vectors"
    )
    parser.add_argument(
        "--out-dir", type=Path, default=Path("golden_vectors"), help="Directory for emitted frames"
    )
    parser.add_argument(
        "--module-path", type=str, default=None, help="Extra path to locate gnuradio.lora_sdr Python bindings"
    )

    args = parser.parse_args()

    if args.os_factor is not None:
        os_factor = args.os_factor
    else:
        if args.samp_rate is None:
            raise ValueError("Either --os-factor or --samp-rate must be specified")
        os_factor = _infer_os_factor(args.samp_rate, args.bw)

    if not args.emit_frame:
        upchirp, downchirp = build_reference_chirps(args.sf, os_factor)
    else:
        # Frame logic (possibly batch)
        sfs = [args.sf]
        if args.batch and args.sf_list:
            sfs = _parse_int_list(args.sf_list)
        crs = [int(args.cr)]
        if args.batch and args.cr_list:
            crs = _parse_int_list(args.cr_list)
        ldros = [args.ldro_mode]
        if args.batch and args.ldro_list:
            ldros = _parse_int_list(args.ldro_list)
        crc_opts = [not args.no_crc]
        if args.batch and args.crc_both:
            crc_opts = [True, False]
        header_opts = [args.impl_header]
        if args.batch and args.both_header:
            header_opts = [False, True]
        payload: bytes
        if args.payload.startswith("0x"):
            hexstr = args.payload[2:]
            payload = bytes.fromhex(hexstr)
        else:
            payload = args.payload.encode("utf-8")
        out_dir = args.out_dir.expanduser().resolve()
        out_dir.mkdir(parents=True, exist_ok=True)
        generated = []
        for sf in sfs:
            for cr in crs:
                for ldro in ldros:
                    for has_crc in crc_opts:
                        for impl in header_opts:
                            iq = _generate_frame(
                                sf=sf,
                                bw=int(args.bw),
                                samp_rate=int(args.samp_rate) if args.samp_rate else int(args.bw),
                                cr=cr,
                                has_crc=has_crc,
                                impl_head=impl,
                                ldro_mode=ldro,
                                payload=payload,
                                sync_word=0x12,
                                frame_zero_padd=(1 << sf),
                                module_path=args.module_path,
                            )
                            stem = (
                                f"tx_sf{sf}_bw{int(args.bw)}_cr{cr}_crc{int(has_crc)}_"
                                f"impl{int(impl)}_ldro{ldro}_pay{len(payload)}"
                            )
                            bin_path = out_dir / f"{stem}.cf32"
                            iq.astype(np.complex64).tofile(bin_path)
                            meta = {
                                "sf": sf,
                                "bw": int(args.bw),
                                "cr": cr,
                                "crc": has_crc,
                                "impl_header": impl,
                                "ldro_mode": ldro,
                                "samp_rate": int(args.samp_rate) if args.samp_rate else int(args.bw),
                                "payload_len": len(payload),
                                "payload_hex": payload.hex(),
                                "filename": bin_path.name,
                            }
                            (out_dir / f"{stem}.json").write_text(json.dumps(meta, indent=2))
                            generated.append(bin_path.name)
                            print(f"[frame] wrote {bin_path.name} samples={len(iq)}")
        print(f"Generated {len(generated)} frame vectors in {out_dir}")
        return

    if args.output is None:
        default_name = f"reference_chirps_sf{args.sf}_os{os_factor}"
        suffix = ".npz" if args.format == "npz" else ""
        output = Path(default_name + suffix)
    else:
        output = args.output

    saved_files = save_chirps(upchirp, downchirp, output, args.format, not args.no_downchirp)

    print(
        f"Generated reference chirps with {len(upchirp)} samples per symbol "
        f"(SF={args.sf}, oversampling={os_factor})."
    )
    for path in saved_files:
        print(f"Saved {path}")

    if args.preview:
        preview_len = min(8, len(upchirp))
        print("First samples of the upchirp:")
        for idx in range(preview_len):
            print(f"  up[{idx}] = {format_complex(upchirp[idx])}")
        if not args.no_downchirp:
            print("First samples of the downchirp:")
            for idx in range(preview_len):
                print(f"  down[{idx}] = {format_complex(downchirp[idx])}")


if __name__ == "__main__":
    main()
