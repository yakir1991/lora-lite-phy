#!/usr/bin/env python3

"""Verify host_sim/lora_replay output against regenerated GNU Radio payload binaries.

Workflow:
1) Use tools/regenerate_gnuradio_payloads_from_summaries.py to create a directory of:
   - <base>.meta.json
   - <base>_gnuradio_payload.bin
2) This script runs lora_replay with the corresponding meta JSON and compares the
   dumped host payload bytes against the regenerated GNU payload bytes.

This is the strict compatibility check: payload bytes must match exactly, including
CRC-gated empties.

Exit code:
  0 - all payloads match
  1 - at least one mismatch
"""

from __future__ import annotations

import argparse
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List, Optional, Tuple


REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_REGEN_DIR = REPO_ROOT / "build" / "receiver_vs_gnuradio_regen_gnu"
DEFAULT_LORA_REPLAY = REPO_ROOT / "build" / "host_sim" / "lora_replay"
DEFAULT_STANDALONE_SUFFIX = "_standalone_payload.bin"


@dataclass(frozen=True)
class Case:
    base: str
    meta: Path
    ref_payload: Path


def discover_cases(regen_dir: Path) -> List[Case]:
    cases: List[Case] = []
    for ref_payload in sorted(regen_dir.glob("*_gnuradio_payload.bin")):
        base = ref_payload.name[: -len("_gnuradio_payload.bin")]
        meta = regen_dir / f"{base}.meta.json"
        if not meta.exists():
            raise FileNotFoundError(f"Missing meta for {base}: {meta}")
        cases.append(Case(base=base, meta=meta, ref_payload=ref_payload))
    return cases


def run_lora_replay(lora_replay: Path, capture: Path, meta: Path, out_payload: Path, timeout_s: float) -> Tuple[int, bytes]:
    cmd = [
        str(lora_replay),
        "--iq",
        str(capture),
        "--metadata",
        str(meta),
        "--dump-payload",
        str(out_payload),
    ]
    proc = subprocess.run(cmd, capture_output=True, timeout=timeout_s)
    return proc.returncode, (proc.stdout or b"") + (proc.stderr or b"")


def compare_bytes(ref_payload: Path, host_payload: Path) -> Tuple[bool, str]:
    ref = ref_payload.read_bytes()
    host = host_payload.read_bytes() if host_payload.exists() else b""

    if ref == host:
        return True, ""

    max_len = max(len(ref), len(host))
    for i in range(max_len):
        rb = ref[i] if i < len(ref) else None
        hb = host[i] if i < len(host) else None
        if rb != hb:
            return False, f"first_diff@{i}: ref={rb} host={hb} (ref_len={len(ref)} host_len={len(host)})"

    return False, f"length_mismatch ref_len={len(ref)} host_len={len(host)}"


def main(argv: Optional[Iterable[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--regen-dir", type=Path, default=DEFAULT_REGEN_DIR)
    parser.add_argument("--captures-dir", type=Path, default=REPO_ROOT / "build" / "receiver_vs_gnuradio")
    parser.add_argument("--lora-replay", type=Path, default=DEFAULT_LORA_REPLAY)
    parser.add_argument("--timeout-s", type=float, default=180.0)
    parser.add_argument("--max-cases", type=int, default=0)
    parser.add_argument(
        "--prefer-existing-standalone",
        action="store_true",
        help=(
            "If <base>_standalone_payload.bin exists under --captures-dir, compare against it "
            "instead of re-running lora_replay (much faster)."
        ),
    )
    parser.add_argument(
        "--rerun-on-mismatch",
        action="store_true",
        help=(
            "When --prefer-existing-standalone is set and the existing payload mismatches, "
            "re-run lora_replay for that case and compare again. This helps detect stale "
            "*_standalone_payload.bin artifacts."
        ),
    )
    parser.add_argument(
        "--standalone-suffix",
        type=str,
        default=DEFAULT_STANDALONE_SUFFIX,
        help="Filename suffix for existing host payloads (default: _standalone_payload.bin)",
    )
    args = parser.parse_args(list(argv) if argv is not None else None)

    regen_dir = args.regen_dir.resolve()
    captures_dir = args.captures_dir.resolve()
    lora_replay = args.lora_replay.resolve()

    if not lora_replay.exists():
        raise FileNotFoundError(f"lora_replay not found: {lora_replay}")

    cases = discover_cases(regen_dir)
    if args.max_cases and args.max_cases > 0:
        cases = cases[: args.max_cases]

    failures: List[str] = []
    reused = 0
    rerun_due_to_mismatch = 0
    rerun = 0

    for case in cases:
        capture = captures_dir / f"{case.base}.cf32"
        if not capture.exists():
            failures.append(f"{case.base}: capture missing: {capture}")
            continue

        existing_host_payload = captures_dir / f"{case.base}{args.standalone_suffix}"
        if args.prefer_existing_standalone and existing_host_payload.exists():
            ok, detail = compare_bytes(case.ref_payload, existing_host_payload)
            reused += 1
            if ok or not args.rerun_on_mismatch:
                if not ok:
                    failures.append(f"{case.base}: payload_mismatch {detail}")
                continue

            # Existing payload mismatched: re-run lora_replay for an up-to-date comparison.
            rerun_due_to_mismatch += 1

            with tempfile.TemporaryDirectory(prefix=f"verify_host_rerun_{case.base}_") as tmp:
                tmp_dir = Path(tmp)
                host_payload = tmp_dir / "host_payload.bin"
                host_payload.write_bytes(b"")

                rc, output = run_lora_replay(
                    lora_replay=lora_replay,
                    capture=capture,
                    meta=case.meta,
                    out_payload=host_payload,
                    timeout_s=float(args.timeout_s),
                )
                if rc != 0:
                    failures.append(
                        f"{case.base}: lora_replay rc={rc} (tail={output[-200:].decode(errors='ignore')!r})"
                    )
                    continue

                ok2, detail2 = compare_bytes(case.ref_payload, host_payload)
                if not ok2:
                    failures.append(f"{case.base}: payload_mismatch {detail2}")
                rerun += 1
            continue

        with tempfile.TemporaryDirectory(prefix=f"verify_host_{case.base}_") as tmp:
            tmp_dir = Path(tmp)
            host_payload = tmp_dir / "host_payload.bin"
            host_payload.write_bytes(b"")

            rc, output = run_lora_replay(
                lora_replay=lora_replay,
                capture=capture,
                meta=case.meta,
                out_payload=host_payload,
                timeout_s=float(args.timeout_s),
            )
            if rc != 0:
                failures.append(f"{case.base}: lora_replay rc={rc} (tail={output[-200:].decode(errors='ignore')!r})")
                continue

            ok, detail = compare_bytes(case.ref_payload, host_payload)
            if not ok:
                failures.append(f"{case.base}: payload_mismatch {detail}")
            rerun += 1

    if failures:
        for line in failures:
            print(line)
        print(
            f"FAIL ({len(failures)}/{len(cases)} cases) "
            f"[reused_existing={reused} rerun_due_to_mismatch={rerun_due_to_mismatch} rerun_lora_replay={rerun}]"
        )
        return 1

    print(
        f"OK ({len(cases)}/{len(cases)} cases) "
        f"[reused_existing={reused} rerun_due_to_mismatch={rerun_due_to_mismatch} rerun_lora_replay={rerun}]"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
