#!/usr/bin/env python3
"""Simple TCP IQ stream emulator for exercising the streaming receiver."""
from __future__ import annotations

import argparse
import os
import socket
import struct
import time
from pathlib import Path

import numpy as np


def load_cf32(path: Path) -> np.ndarray:
    raw = np.fromfile(path, dtype=np.float32)
    if raw.size % 2 != 0:
        raise ValueError(f"{path}: odd number of float32 entries")
    return raw.reshape(-1, 2)


def stream_vectors(
    host: str,
    port: int,
    unix_path: str | None,
    vectors: list[Path],
    chunk: int,
    sleep_s: float,
    loop: bool,
) -> None:
    payloads = [load_cf32(vec) for vec in vectors]
    if unix_path:
        path = Path(unix_path)
        if path.exists():
            path.unlink()
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(unix_path)
        server.listen(1)
        print(f"[emulator] listening on unix://{unix_path} (chunk={chunk} samples, sleep={sleep_s:.6f}s)")
    else:
        server = socket.create_server((host, port), reuse_port=False)
        print(f"[emulator] listening on {host}:{port} (chunk={chunk} samples, sleep={sleep_s:.6f}s)")

    try:
        with server:
            while True:
                conn, addr = server.accept()
                print(f"[emulator] client connected from {addr}")
                with conn:
                    try:
                        while True:
                            for payload, path in zip(payloads, vectors, strict=False):
                                arr = payload
                                total_samples = arr.shape[0]
                                sent_samples = 0
                                while sent_samples < total_samples:
                                    take = min(chunk, total_samples - sent_samples)
                                    chunk_view = arr[sent_samples : sent_samples + take]
                                    conn.sendall(struct.pack("!I", take))
                                    conn.sendall(chunk_view.astype(np.float32).tobytes())
                                    sent_samples += take
                                    if sleep_s > 0.0:
                                        time.sleep(sleep_s)
                                conn.sendall(struct.pack("!I", 0))
                                print(f"[emulator] sent frame {path.name} ({total_samples} samples)")
                            if not loop:
                                break
                    except (BrokenPipeError, ConnectionResetError):
                        print("[emulator] client disconnected")
                if not loop:
                    break
    finally:
        if unix_path:
            try:
                Path(unix_path).unlink()
            except FileNotFoundError:
                pass


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("vectors", nargs="+", type=Path, help="Input .cf32 vectors")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--chunk", type=int, default=2048, help="Samples per packet")
    parser.add_argument("--sleep-ms", type=float, default=0.0, help="Sleep between packets in milliseconds")
    parser.add_argument("--loop", action="store_true", help="Loop vectors indefinitely")
    parser.add_argument("--unix-socket", type=str, help="Unix domain socket path (overrides host/port)")
    args = parser.parse_args()

    stream_vectors(
        args.host,
        args.port,
        args.unix_socket,
        args.vectors,
        max(1, args.chunk),
        max(0.0, args.sleep_ms / 1000.0),
        args.loop,
    )


if __name__ == "__main__":
    main()
