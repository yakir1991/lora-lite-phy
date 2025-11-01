#!/usr/bin/env python3
"""
Connect to the socket emulator, capture a single frame of samples, dump them to
disk, and optionally invoke streaming_harness on the captured IQ.
"""

from __future__ import annotations

import argparse
import json
import struct
import subprocess
import sys
from pathlib import Path
from typing import Optional


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--unix", type=Path, help="Unix domain socket path")
    group.add_argument("--host", help="TCP host (default 127.0.0.1)")
    parser.add_argument("--port", type=int, default=9000, help="TCP port (default 9000)")
    parser.add_argument("--meta", type=Path, required=True, help="Metadata JSON for the vector")
    parser.add_argument("--output", type=Path, help="Output cf32 file path")
    parser.add_argument("--run-harness", action="store_true", help="Invoke streaming_harness on the captured IQ")
    parser.add_argument("--chunk-dir", type=Path, help="Optional directory to dump raw chunk files for debugging")
    parser.add_argument("--silent", action="store_true", help="Do not print harness stdout")
    return parser.parse_args()


def read_metadata(meta_path: Path) -> dict:
    return json.loads(meta_path.read_text())


def connect_socket(args: argparse.Namespace):
    if args.unix:
        import socket

        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.connect(str(args.unix))
        return sock

    host = args.host or "127.0.0.1"
    import socket

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((host, args.port))
    return sock


def read_exact(sock, size: int) -> Optional[bytes]:
    view = memoryview(bytearray(size))
    remaining = size
    offset = 0
    while remaining:
        n = sock.recv_into(view[offset:], remaining)
        if n == 0:
            return None
        offset += n
        remaining -= n
    return bytes(view)


def capture_frame(sock, chunk_dir: Optional[Path]) -> bytes:
    raw = bytearray()
    chunk_idx = 0
    while True:
        raw_len = read_exact(sock, 4)
        if raw_len is None:
            break
        (sample_count,) = struct.unpack("!I", raw_len)
        if sample_count == 0:
            break
        raw_data = read_exact(sock, sample_count * 8)
        if raw_data is None:
            break
        raw.extend(raw_data)
        if chunk_dir:
            chunk_dir.mkdir(parents=True, exist_ok=True)
            chunk_path = chunk_dir / f"chunk_{chunk_idx:03d}.cf32"
            with chunk_path.open("wb") as f:
                f.write(raw_data)
        chunk_idx += 1
    return bytes(raw)


def dump_cf32(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def run_harness(meta: dict, iq_path: Path, silent: bool) -> subprocess.CompletedProcess[str]:
    harness = Path("cpp_receiver/build/streaming_harness")
    if not harness.exists():
        raise FileNotFoundError("streaming_harness binary is missing; build it first.")

    samp_rate = meta.get("samp_rate") or meta.get("sample_rate") or meta.get("fs") or meta.get("bw") or 0
    if samp_rate == 0:
        raise ValueError("Metadata missing sample rate (samp_rate/sample_rate/fs/bw).")

    cmd = [
        str(harness),
        "--sf", str(meta.get("sf")),
        "--bw", str(meta.get("bw")),
        "--fs", str(samp_rate),
        "--cr", str(meta.get("cr", 1)),
        str(iq_path),
    ]
    ldro_mode = meta.get("ldro_mode")
    if ldro_mode is not None:
        try:
            cmd.extend(["--ldro-mode", str(int(ldro_mode))])
        except Exception:
            if meta.get("ldro"):
                cmd.extend(["--ldro"])
    elif meta.get("ldro"):
        cmd.extend(["--ldro"])
    result = subprocess.run(cmd, capture_output=True, text=True)
    if not silent:
        if result.stdout:
            sys.stdout.write(result.stdout)
        if result.stderr:
            sys.stderr.write(result.stderr)
    return result


def main() -> None:
    args = parse_args()
    meta = read_metadata(args.meta)
    sock = connect_socket(args)
    try:
        raw_data = capture_frame(sock, args.chunk_dir)
    finally:
        sock.close()

    if not raw_data:
        print("[capture] no samples received", file=sys.stderr)
        sys.exit(1)

    output = args.output
    if not output:
        output = Path("results/live_capture.cf32")
    dump_cf32(output, raw_data)
    print(f"[capture] wrote {output} ({len(raw_data)//8} complex samples)")

    if args.run_harness:
        result = run_harness(meta, output, args.silent)
        if result.returncode != 0:
            sys.exit(result.returncode)


if __name__ == "__main__":
    main()
