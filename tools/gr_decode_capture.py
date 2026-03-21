#!/usr/bin/env python3

"""
Decode a LoRa CF32 capture using GNU Radio gr-lora_sdr blocks and dump payload/CRC bytes.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import resource
import sys
import threading
import time
from pathlib import Path
def log_rss(label: str) -> None:
    usage = resource.getrusage(resource.RUSAGE_SELF)
    print(f"[mem] {label}: {usage.ru_maxrss} kB", flush=True)


def debug(msg: str) -> None:
    if os.environ.get("LORA_GR_DECODE_DEBUG"):
        print(f"[gr_decode_capture] {msg}", flush=True)


def _extend_lora_sdr_paths() -> None:
    repo_root = Path(__file__).resolve().parent.parent
    python_paths = []
    gnuradio_paths = []

    py_tag = f"python{sys.version_info.major}.{sys.version_info.minor}"

    # Support both local "install" (in-tree) and "install_sys" (system-style prefix)
    # layouts. The workspace's generate_and_verify.sh uses install_sys.
    # Prefer install_sys (matches system python3.10 in this repo) over install.
    install_prefixes = [
        repo_root / "gr_lora_sdr" / "install_sys",
        repo_root / "gr_lora_sdr" / "install",
    ]
    ld_paths: list[str] = []
    for prefix in install_prefixes:
        if not prefix.exists():
            continue

        # Python module locations.
        for candidate in sorted((prefix / "lib").glob("python*/site-packages")):
            if py_tag not in str(candidate):
                continue
            python_paths.append(str(candidate.resolve()))
            gnuradio_candidate = candidate / "gnuradio"
            if gnuradio_candidate.exists():
                gnuradio_paths.append(str(gnuradio_candidate.resolve()))

        # Debian-style dist-packages under install_sys/local.
        local_lib = prefix / "local" / "lib"
        if local_lib.exists():
            for candidate in sorted(local_lib.glob("python*/dist-packages")):
                if py_tag not in str(candidate):
                    continue
                python_paths.append(str(candidate.resolve()))
                gnuradio_candidate = candidate / "gnuradio"
                if gnuradio_candidate.exists():
                    gnuradio_paths.append(str(gnuradio_candidate.resolve()))

        # Shared library locations.
        for lib_dir in (
            prefix / "lib",
            prefix / "lib" / "x86_64-linux-gnu",
        ):
            if not lib_dir.exists():
                continue
            ld_paths.append(str(lib_dir.resolve()))

            # Preload the module library so dlopen can resolve it without LD_LIBRARY_PATH.
            for lib_path in sorted(lib_dir.glob("libgnuradio-lora_sdr.so*")):
                if not lib_path.is_file():
                    continue
                try:
                    mode = getattr(ctypes, "RTLD_GLOBAL", None)
                    if mode is None:
                        ctypes.CDLL(str(lib_path))
                    else:
                        ctypes.CDLL(str(lib_path), mode=mode)
                    break
                except OSError:
                    continue

    if ld_paths:
        existing_ld = os.environ.get("LD_LIBRARY_PATH")
        joined = ":".join(ld_paths)
        os.environ["LD_LIBRARY_PATH"] = (
            f"{joined}:{existing_ld}" if existing_ld else joined
        )

    python_src = repo_root / "gr_lora_sdr" / "python"
    if python_src.exists():
        python_paths.append(str(python_src.resolve()))
        gnuradio_candidate = python_src / "gnuradio"
        if gnuradio_candidate.exists():
            gnuradio_paths.append(str(gnuradio_candidate.resolve()))

    for path in reversed(python_paths):
        if path not in sys.path:
            sys.path.insert(0, path)

    gnuradio_mod = sys.modules.get("gnuradio")
    if gnuradio_mod is not None and hasattr(gnuradio_mod, "__path__"):
        for path in gnuradio_paths:
            if path not in gnuradio_mod.__path__:
                gnuradio_mod.__path__.append(path)


try:
    from gnuradio import blocks, gr
    import gnuradio.lora_sdr as lora_sdr
except ModuleNotFoundError:
    _extend_lora_sdr_paths()
    from gnuradio import blocks, gr
    import gnuradio.lora_sdr as lora_sdr

import pmt


def write_int_list(path: Path, values: list[int]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as handle:
        for value in values:
            handle.write(f"{value}\n")


def _hostsim_fft_stage_from_raw_symbol(symbol: int, *, sf: int, reduce_by_four: bool) -> int:
    """Match host_sim's notion of the FFT stage.

    host_sim (see --dump-normalized) computes:
      raw = symbol & ((1<<sf)-1)
      norm_full = (raw - 1) & ((1<<sf)-1)
      norm_ldro = norm_full >> 2   (when LDRO is enabled)

    For stage comparisons we want values in the same domain as host_sim's
    `norm_ldro` (or `norm_full` when not reducing).
    """
    mask_full = (1 << int(sf)) - 1
    raw = int(symbol) & mask_full
    norm_full = (raw - 1) & mask_full
    return int(norm_full >> 2) if reduce_by_four else int(norm_full)


def _looks_like_already_reduced(symbols: list[int], *, sf: int) -> bool:
    """Heuristic: some GNU Radio taps already emit LDRO-reduced symbols.

    When LDRO reduction is applied, symbols live in an (sf-2)-bit domain.
    """
    if not symbols:
        return False
    reduced_bits = max(int(sf) - 2, 1)
    max_reduced = (1 << reduced_bits) - 1
    return max(symbols) <= max_reduced


def _write_stage_reference(
    stage_root: Path,
    *,
    sf: int,
    ldro_enabled: bool,
    symbols: list[int],
    deinterleaver: list[int],
    hamming: list[int],
) -> None:
    base = stage_root
    if base.suffix == ".cf32":
        base = base.with_suffix("")

    # host_sim compares against `norm_ldro` for LDRO-enabled streams.
    # Depending on where we tap GNU Radio, `symbols` might already be in the
    # reduced domain. Avoid double-normalizing by detecting this case.
    reduce = bool(ldro_enabled)
    if reduce and _looks_like_already_reduced(symbols, sf=sf):
        reduced_bits = max(int(sf) - 2, 1)
        mask_reduced = (1 << reduced_bits) - 1
        fft_vals = [int(s) & mask_reduced for s in symbols]
    else:
        fft_vals = [_hostsim_fft_stage_from_raw_symbol(s, sf=sf, reduce_by_four=reduce) for s in symbols]
    gray_vals = [int(v) ^ (int(v) >> 1) for v in fft_vals]

    write_int_list(base.with_name(base.name + "_fft.txt"), fft_vals)
    write_int_list(base.with_name(base.name + "_gray.txt"), gray_vals)
    write_int_list(
        base.with_name(base.name + "_deinterleaver.txt"),
        [int(v) & 0xFF for v in deinterleaver],
    )
    write_int_list(
        base.with_name(base.name + "_hamming.txt"),
        [int(v) & 0xFF for v in hamming],
    )


def compute_crc(payload: bytes) -> int:
    # Match gr-lora_sdr's crc_verif:
    # - Compute CRC16-CCITT over the first (N-2) bytes.
    # - XOR with the last two payload bytes interpreted as MSB:LSB.
    if len(payload) < 2:
        return 0

    crc = 0x0000
    for byte in payload[:-2]:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    # XOR with last 2 payload bytes (MSB then LSB).
    crc ^= payload[-1] ^ (payload[-2] << 8)
    return crc & 0xFFFF


def build_flowgraph(
    input_path: Path,
    payload_path: Path,
    sf: int,
    bw: int,
    sample_rate: int,
    cr: int,
    payload_len: int,
    sync_word: int,
    preamble_len: int,
    implicit_header: bool,
    has_crc: bool,
    ldro: bool,
    dump_codewords: bool,
    dump_nibbles: bool,
    dump_symbols: bool,
    bypass_crc_verif: bool,
    tag_debug: bool,
    debug_max_items: int = 0,
) -> tuple[gr.top_block, dict[str, blocks.block], lora_sdr.frame_sync, blocks.file_sink]:
    tb = gr.top_block()
    payload_sink = blocks.file_sink(gr.sizeof_char, str(payload_path), False)
    sinks: dict[str, blocks.block] = {}

    # Cap how many payload bytes can reach the sink. This makes the flowgraph
    # terminate deterministically even if upstream blocks keep producing data.
    payload_head_count = int(payload_len) if int(payload_len) > 0 else (1 << 30)
    payload_head = blocks.head(gr.sizeof_char, payload_head_count)

    file_source = blocks.file_source(gr.sizeof_gr_complex, str(input_path), False)
    # Avoid GNU Radio scheduler failures where frame_sync requests slightly more than
    # one symbol worth of samples, but the upstream block can only ever provide 8191
    # items per call (common default buffer ceiling). Mirror tx_rx_simulation.py's
    # approach by ensuring at least ~one symbol (+ slack) is available.
    try:
        oversample = max(1, int(sample_rate / bw))
        samples_per_symbol = (1 << int(sf)) * oversample
        # Provide generous slack to satisfy internal filter/tap requirements.
        file_source.set_min_output_buffer(max(16384, int(samples_per_symbol * 2)))
    except Exception:
        pass
    frame_sync = lora_sdr.frame_sync(
        int(868.1e6), bw, sf, implicit_header, [sync_word], int(sample_rate / bw), preamble_len
    )
    # Match tx_rx_simulation.py receiver wiring.
    fft_demod = lora_sdr.fft_demod(False, False)
    gray_mapping = lora_sdr.gray_mapping(False)
    deinterleaver = lora_sdr.deinterleaver(False)
    hamming_dec = lora_sdr.hamming_dec(False)
    # Disable verbose header logging to keep batch runs fast and quiet.
    header_decoder = lora_sdr.header_decoder(
        implicit_header, cr, payload_len, has_crc, ldro, False
    )
    dewhitening = lora_sdr.dewhitening()
    # Enable crc_check output so we can gate invalid decodes in our harness.
    crc_verif = lora_sdr.crc_verif(1, True)
    # Collecting crc_check with a vector sink can grow without bound if the flowgraph
    # doesn't terminate naturally (e.g., CRC never passes so payload never reaches the file).
    # We only need the first frame's CRC flag for strict comparisons.
    crc_check_head = blocks.head(gr.sizeof_char, 1)
    crc_check_sink = blocks.vector_sink_b()
    sinks["crc_check"] = crc_check_sink

    tag_debug_after_hamming: blocks.block | None = None
    tag_debug_after_header: blocks.block | None = None
    if tag_debug:
        # Print only the LoRa stream metadata tags to keep output manageable.
        tag_debug_after_hamming = blocks.tag_debug(gr.sizeof_char, "after_hamming", "frame_info")
        tag_debug_after_header = blocks.tag_debug(gr.sizeof_char, "after_header", "frame_info")

    tb.msg_connect((header_decoder, "frame_info"), (frame_sync, "frame_info"))
    tb.connect(file_source, frame_sync, fft_demod)
    tb.connect(fft_demod, gray_mapping)
    if dump_symbols:
        symbol_sink = blocks.vector_sink_s()
        if int(debug_max_items) > 0:
            sym_head = blocks.head(gr.sizeof_short, int(debug_max_items))
            tb.connect(fft_demod, sym_head, symbol_sink)
        else:
            tb.connect(fft_demod, symbol_sink)
        sinks["symbols"] = symbol_sink
    tb.connect(gray_mapping, deinterleaver)
    if dump_codewords:
        codeword_sink = blocks.vector_sink_b()
        if int(debug_max_items) > 0:
            cw_head = blocks.head(gr.sizeof_char, int(debug_max_items))
            tb.connect(deinterleaver, cw_head, codeword_sink)
        else:
            tb.connect(deinterleaver, codeword_sink)
        sinks["codewords"] = codeword_sink
    tb.connect(deinterleaver, hamming_dec)
    if dump_nibbles:
        nibble_sink = blocks.vector_sink_b()
        if int(debug_max_items) > 0:
            nib_head = blocks.head(gr.sizeof_char, int(debug_max_items))
            tb.connect(hamming_dec, nib_head, nibble_sink)
        else:
            tb.connect(hamming_dec, nibble_sink)
        sinks["nibbles"] = nibble_sink

    tb.connect(hamming_dec, header_decoder)
    if tag_debug_after_hamming is not None:
        tb.connect(hamming_dec, tag_debug_after_hamming)

    if bypass_crc_verif:
        tb.connect(header_decoder, dewhitening, payload_head, payload_sink)
    else:
        tb.connect(header_decoder, dewhitening, crc_verif)
        tb.connect((crc_verif, 0), payload_head, payload_sink)
        tb.connect((crc_verif, 1), crc_check_head, crc_check_sink)

    if tag_debug_after_header is not None:
        tb.connect(header_decoder, tag_debug_after_header)

    return tb, sinks, frame_sync, payload_sink


def _coerce_sync_word(meta: dict) -> list[int]:
    value = meta.get("sync_word", 0x12)
    if isinstance(value, int):
        return [value]
    if isinstance(value, list) and all(isinstance(x, int) for x in value):
        return value
    return [0x12]


def _try_set_tag_debug_display(block: blocks.block) -> None:
    """Best-effort: some GNU Radio builds expose set_display(bool)."""
    try:
        getattr(block, "set_display")(True)
    except Exception:
        pass


def _coerce_ldro_mode(meta: dict) -> int:
    """Map our metadata to gr-lora_sdr's header_decoder ldro_mode.

    gr-lora_sdr expects an integer: 0=off, 1=on, 2=auto.
    """

    if "ldro_mode" in meta:
        try:
            return int(meta["ldro_mode"])
        except (TypeError, ValueError):
            return 2
    if "ldro" in meta:
        return 1 if bool(meta.get("ldro")) else 0
    return 2


def build_flowgraph_simple(
    input_path: Path,
    payload_path: Path,
    sf: int,
    bw: int,
    sample_rate: int,
    cr: int,
    payload_len: int,
    sync_word: list[int],
    implicit_header: bool,
    has_crc: bool,
    ldro_mode: int,
) -> tuple[gr.top_block, blocks.file_sink]:
    """Decode via the upstream hierarchical block to match reference wiring."""

    tb = gr.top_block()
    file_source = blocks.file_source(gr.sizeof_gr_complex, str(input_path), False)
    payload_sink = blocks.file_sink(gr.sizeof_char, str(payload_path), False)
    rx = lora_sdr.lora_sdr_lora_rx(
        center_freq=int(868.1e6),
        bw=bw,
        cr=cr,
        has_crc=has_crc,
        impl_head=implicit_header,
        pay_len=payload_len,
        samp_rate=sample_rate,
        sf=sf,
        sync_word=sync_word,
        soft_decoding=False,
        ldro_mode=ldro_mode,
        print_rx=[False, False],
    )
    tb.connect(file_source, rx, payload_sink)
    return tb, payload_sink


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True, help="Path to CF32 capture")
    parser.add_argument("--metadata", type=Path, required=True, help="Associated metadata JSON")
    parser.add_argument("--payload-out", type=Path, required=True, help="Output file for decoded payload bytes")
    parser.add_argument(
        "--timeout-s",
        type=float,
        help=(
            "Optional timeout for GNU Radio execution. If omitted, a timeout is computed from "
            "capture length and sample rate. Also configurable via LORA_GR_DECODE_TIMEOUT_S."
        ),
    )
    parser.add_argument("--crc-out", type=Path, help="Optional output file for CRC bytes")
    parser.add_argument(
        "--bypass-crc-verif",
        action="store_true",
        help=(
            "Debug only: in manual flowgraph, bypass crc_verif and write bytes directly after dewhitening. "
            "This helps diagnose cases where crc_verif never releases output because the expected CRC bytes never arrive."
        ),
    )
    parser.add_argument("--dump-codewords", type=Path, help="Optional file for raw deinterleaver codewords (uint8 per line)")
    parser.add_argument("--dump-nibbles", type=Path, help="Optional file for Hamming-decoded nibbles (uint8 per line)")
    parser.add_argument("--dump-symbols", type=Path, help="Optional file for hard-demod symbols (uint16 per line)")
    parser.add_argument("--dump-framesync", type=Path, help="Optional file for frame_sync output samples (CF32)")
    parser.add_argument("--dump-sync-offset", type=Path, help="Optional file to record GNU frame_sync start offsets (samples)")
    parser.add_argument(
        "--dump-stage-root",
        type=Path,
        help=(
            "Optional path/prefix for host_sim-style stage reference dumps. "
            "Writes <prefix>_{fft,gray,deinterleaver,hamming}.txt using GNU debug sinks."
        ),
    )
    parser.add_argument(
        "--tag-debug",
        action="store_true",
        help="Debug: print frame_info tags after hamming_dec/header_decoder",
    )
    parser.add_argument(
        "--debug-max-items",
        type=int,
        default=0,
        help=(
            "Debug only: cap how many items GNU vector sinks retain for stage dumps "
            "(symbols/codewords/nibbles). 0 means unlimited."
        ),
    )
    parser.add_argument(
        "--disable-early-stop",
        action="store_true",
        help=(
            "Debug only: do not stop the flowgraph early when payload_out reaches payload_len; "
            "run until timeout or natural termination."
        ),
    )
    parser.add_argument(
        "--force-manual",
        action="store_true",
        help="Force the manual flowgraph (more deterministic) even without dumps",
    )
    args = parser.parse_args()

    debug("args-parsed")

    args.payload_out.parent.mkdir(parents=True, exist_ok=True)

    debug(f"reading-metadata: {args.metadata}")
    meta = json.loads(args.metadata.read_text())
    debug("metadata-loaded")
    wants_debug = any(
        x is not None
        for x in (
            args.dump_codewords,
            args.dump_nibbles,
            args.dump_symbols,
            args.dump_framesync,
            args.dump_sync_offset,
            args.dump_stage_root,
        )
    )
    wants_debug = wants_debug or bool(args.bypass_crc_verif) or bool(args.tag_debug) or bool(args.force_manual)

    if wants_debug:
        debug("building-flowgraph: manual")
        tb, sinks, frame_sync, payload_sink = build_flowgraph(
            input_path=args.input,
            payload_path=args.payload_out,
            sf=meta["sf"],
            bw=meta["bw"],
            sample_rate=meta["sample_rate"],
            cr=meta["cr"],
            payload_len=meta["payload_len"],
            sync_word=_coerce_sync_word(meta)[0],
            preamble_len=meta.get("preamble_len", 8),
            implicit_header=meta.get("implicit_header", False),
            has_crc=meta.get("has_crc", True),
            ldro=bool(meta.get("ldro", False)),
                dump_codewords=(args.dump_codewords is not None) or (args.dump_stage_root is not None),
                dump_nibbles=(args.dump_nibbles is not None) or (args.dump_stage_root is not None),
                dump_symbols=(args.dump_symbols is not None) or (args.dump_stage_root is not None),
            bypass_crc_verif=bool(args.bypass_crc_verif),
               tag_debug=args.tag_debug,
            debug_max_items=int(args.debug_max_items),
        )
    else:
        if args.bypass_crc_verif or args.tag_debug:
            raise SystemExit("--bypass-crc-verif/--tag-debug require debug (manual) flowgraph")
        debug("building-flowgraph: hier")
        tb, payload_sink = build_flowgraph_simple(
            input_path=args.input,
            payload_path=args.payload_out,
            sf=meta["sf"],
            bw=meta["bw"],
            sample_rate=meta["sample_rate"],
            cr=meta["cr"],
            payload_len=meta["payload_len"],
            sync_word=_coerce_sync_word(meta),
            implicit_header=meta.get("implicit_header", False),
            has_crc=meta.get("has_crc", True),
            ldro_mode=_coerce_ldro_mode(meta),
        )
        sinks = {}
        frame_sync = None

    debug("flowgraph-built")
    log_rss("flowgraph-built")

    if os.environ.get("LORA_GR_DECODE_SKIP_RUN"):
        debug("skip-run: exiting before tb.run")
        return
    framesync_sink = None
    if args.dump_framesync:
        if frame_sync is None:
            raise SystemExit("--dump-framesync requires debug (manual) flowgraph")
        args.dump_framesync.parent.mkdir(parents=True, exist_ok=True)
        framesync_sink = blocks.file_sink(gr.sizeof_gr_complex, str(args.dump_framesync), False)
        tb.connect(frame_sync, framesync_sink)
    tag_sink = None
    if args.dump_sync_offset:
        tag_sink = blocks.vector_sink_c()
        if frame_sync is None:
            raise SystemExit("--dump-sync-offset requires debug (manual) flowgraph")
        tb.connect(frame_sync, tag_sink)
    def _compute_timeout_s() -> float:
        env_timeout = os.environ.get("LORA_GR_DECODE_TIMEOUT_S")
        if env_timeout:
            try:
                return float(env_timeout)
            except ValueError:
                debug(f"invalid LORA_GR_DECODE_TIMEOUT_S={env_timeout!r}; ignoring")

        if args.timeout_s is not None:
            return float(args.timeout_s)

        try:
            file_bytes = args.input.stat().st_size
            samples = file_bytes / 8.0  # CF32 complex: 2x float32 per sample
            sr = float(meta["sample_rate"])
            expected_s = samples / sr if sr > 0 else 0.0
            # Add generous slack to avoid false timeouts when blocks are slow.
            # We also have an early-stop condition once expected payload bytes are observed.
            return max(10.0, expected_s * 20.0 + 5.0)
        except Exception as exc:
            debug(f"timeout auto-compute failed: {exc}; using default")
            return 10.0

    timeout_s = _compute_timeout_s()

    # Ensure we never reuse stale payload bytes across runs.
    args.payload_out.parent.mkdir(parents=True, exist_ok=True)
    args.payload_out.write_bytes(b"")

    expected_payload_bytes = None
    try:
        expected_payload_bytes = int(meta.get("payload_len", 0))
        if expected_payload_bytes <= 0:
            expected_payload_bytes = None
    except Exception:
        expected_payload_bytes = None

    debug(f"tb.run: begin (timeout_s={timeout_s:.3f})")
    start_time = time.time()
    wait_done = threading.Event()
    wait_exc: list[BaseException] = []

    def _waiter() -> None:
        try:
            tb.wait()
        except BaseException as exc:  # noqa: BLE001
            wait_exc.append(exc)
        finally:
            wait_done.set()

    tb.start()
    waiter = threading.Thread(target=_waiter, name="gr_wait", daemon=True)
    waiter.start()

    # If the flowgraph doesn't naturally terminate (common with some GR topologies),
    # stop as soon as we've observed enough output bytes.
    deadline = time.time() + float(timeout_s)
    while True:
        if wait_done.is_set():
            break
        if (not args.disable_early_stop) and expected_payload_bytes is not None:
            try:
                if args.payload_out.exists() and args.payload_out.stat().st_size >= expected_payload_bytes:
                    debug("tb.run: got expected payload bytes; stopping flowgraph")
                    break
            except OSError:
                pass
        if time.time() >= deadline:
            debug("tb.run: timeout; stopping flowgraph")
            break
        time.sleep(0.05)

    tb.stop()
    wait_done.wait(5.0)

    # Ensure resources are always released.
    tb.stop()
    tb.wait()
    elapsed_s = time.time() - start_time
    debug(f"tb.run: end (elapsed_s={elapsed_s:.3f})")

    if wait_exc:
        raise wait_exc[0]
    payload_sink.close()
    if framesync_sink is not None:
        framesync_sink.close()
    log_rss("after-run")

    # Read only what we need to avoid OOM if the flowgraph emits excessive bytes.
    try:
        payload_size = args.payload_out.stat().st_size if args.payload_out.exists() else 0
    except OSError:
        payload_size = 0

    try:
        expected_len_int = int(meta.get("payload_len", 0))
    except Exception:
        expected_len_int = 0

    read_limit = 4096
    if expected_len_int > 0:
        read_limit = expected_len_int

    payload_data = b""
    if payload_size > 0:
        with args.payload_out.open("rb") as f:
            payload_data = f.read(min(payload_size, read_limit))

    log_rss("after-payload-read")

    # If crc_verif provided a CRC check stream, use it to suppress invalid decodes.
    # This aligns comparisons with the common expectation that CRC-failing frames
    # should not be treated as valid payload output.
    if not args.bypass_crc_verif and meta.get("has_crc", True) and "crc_check" in sinks:
        try:
            crc_stream = list(sinks["crc_check"].data())
            # crc_verif emits one boolean per decoded frame. For strict comparisons
            # we care about the first decoded frame (host_sim processes a single frame).
            crc_ok = bool(crc_stream) and int(crc_stream[0]) == 1
        except Exception:
            crc_ok = False

        if not crc_ok:
            args.payload_out.write_bytes(b"")
            payload_data = b""

    # Some captures contain multiple repeated frames (or GNU decodes multiple frames
    # before the scheduler terminates). For strict host-vs-GNU comparisons we want a
    # single frame payload.
    if payload_data and expected_len_int > 0:
        # If more than one frame was emitted, truncate on disk and in-memory.
        if payload_size > expected_len_int:
            payload_data = payload_data[:expected_len_int]
            args.payload_out.write_bytes(payload_data)

    if args.crc_out and meta.get("has_crc", True) and payload_data:
        payload_len = meta["payload_len"]
        crc = compute_crc(payload_data[:payload_len])
        args.crc_out.write_bytes(bytes([crc & 0xFF, (crc >> 8) & 0xFF]))

    if args.dump_codewords and "codewords" in sinks:
        write_int_list(args.dump_codewords, list(sinks["codewords"].data()))
    if args.dump_nibbles and "nibbles" in sinks:
        write_int_list(args.dump_nibbles, list(sinks["nibbles"].data()))
    if args.dump_symbols and "symbols" in sinks:
        write_int_list(args.dump_symbols, list(sinks["symbols"].data()))
    if args.dump_stage_root is not None:
        _write_stage_reference(
            args.dump_stage_root,
            sf=int(meta["sf"]),
            ldro_enabled=bool(meta.get("ldro", False)),
            symbols=[int(v) for v in list(sinks.get("symbols").data())] if "symbols" in sinks else [],
            deinterleaver=[int(v) for v in list(sinks.get("codewords").data())] if "codewords" in sinks else [],
            hamming=[int(v) for v in list(sinks.get("nibbles").data())] if "nibbles" in sinks else [],
        )
    if args.dump_sync_offset and tag_sink is not None:
        offsets: list[int] = []
        for tag in tag_sink.tags():
            if tag.key == pmt.intern("frame_info"):
                info = pmt.to_python(tag.value)
                if isinstance(info, dict) and info.get("is_header", False):
                    offsets.append(int(tag.offset))
        if offsets:
            write_int_list(args.dump_sync_offset, offsets)


if __name__ == "__main__":
    main()
