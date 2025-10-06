"""Unified LoRa Lite PHY tooling helpers.

This module centralises the functionality that previously lived across
multiple standalone scripts under ``tools/`` and ``scripts/`` so that a single
CLI can orchestrate end-to-end analysis:

* Generate payload truth tables using the Python ``sdr_lora`` reference.
* Generate payload truth tables using the GNU Radio offline decoder.
* Compare C++, Python, and GNU Radio receivers.
* Benchmark receiver runtimes.
* Execute the GNU Radio vs C++ compatibility sweep.

Each helper returns structured dictionaries making it easy to surface
results through higher level tooling (e.g. ``scripts.lora_cli``).
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple


ROOT = Path(__file__).resolve().parents[1]
GR_SCRIPT = ROOT / "external" / "gr_lora_sdr" / "scripts" / "decode_offline_recording.py"
PY_TRUTH_DEFAULT = ROOT / "results" / "python_truth.json"
GNUR_TRUTH_DEFAULT = ROOT / "results" / "gnur_truth.json"
CPP_BINARY_CANDIDATES: Tuple[Path, ...] = (
    ROOT / "cpp_receiver" / "build" / "decode_cli",
    ROOT / "cpp_receiver" / "build" / "Release" / "decode_cli",
    ROOT / "cpp_receiver" / "build" / "Debug" / "decode_cli",
    ROOT / "cpp_receiver" / "build" / "decode_cli.exe",
)

DEFAULT_VECTOR_ROOTS: Tuple[Path, ...] = (
    Path("golden_vectors/new_batch"),
    Path("golden_vectors/extended_batch"),
    Path("golden_vectors/extended_batch_crc_off"),
    Path("golden_vectors/extended_batch_impl"),
    Path("golden_vectors/demo"),
    Path("golden_vectors/demo_batch"),
    Path("golden_vectors/custom"),
)


# ---------------------------------------------------------------------------
# Helpers shared across routines
# ---------------------------------------------------------------------------


def _resolve_root_paths(roots: Optional[Sequence[str | Path]], *, default: Sequence[Path]) -> List[Path]:
    if roots:
        paths = [Path(r) for r in roots]
    else:
        paths = list(default)
    resolved: List[Path] = []
    for path in paths:
        p = (ROOT / path).resolve()
        if p.exists():
            resolved.append(p)
    return resolved


def _iter_vector_sidecars(roots: Iterable[Path]) -> Iterable[Tuple[Path, Path]]:
    for root in roots:
        for json_path in sorted(root.glob("*.json")):
            cf32 = json_path.with_suffix(".cf32")
            if cf32.exists():
                yield cf32, json_path


def _iter_vectors(roots: Iterable[Path]) -> Iterable[Path]:
    for root in roots:
        for cf32 in sorted(root.glob("*.cf32")):
            yield cf32


def _load_meta(cf32: Path) -> Dict[str, Any]:
    meta_path = cf32.with_suffix(".json")
    return json.loads(meta_path.read_text())


def _resolve_cpp_binary() -> Optional[Path]:
    for candidate in CPP_BINARY_CANDIDATES:
        if candidate.exists():
            return candidate
    return None


def _ldro_flag(meta: Dict[str, Any]) -> str:
    mode = int(meta.get("ldro_mode", 0) or 0)
    sf = int(meta.get("sf", 7))
    if mode == 1:
        return "1"
    if mode == 2 and sf >= 11:
        return "1"
    return "0"


def _subprocess_run(cmd: Sequence[str], *, timeout: Optional[int] = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        list(cmd),
        capture_output=True,
        text=True,
        timeout=timeout,
        errors="replace",
    )


# ---------------------------------------------------------------------------
# Python truth table generation
# ---------------------------------------------------------------------------


def run_python_truth(
    roots: Optional[Sequence[str | Path]] = None,
    *,
    output: Path = PY_TRUTH_DEFAULT,
    fast: bool = True,
    keep_existing: bool = True,
    timeout: int = 180,
) -> Dict[str, Any]:
    """Generate payload truth via ``scripts.sdr_lora_cli``.

    Returns a dictionary with keys ``written`` (count of newly written entries)
    and ``total`` (total entries in the output file).
    """

    search_roots = _resolve_root_paths(roots, default=DEFAULT_VECTOR_ROOTS)

    existing: Dict[str, Any] = {}
    if keep_existing and output.exists():
        try:
            existing = json.loads(output.read_text())
        except Exception:
            existing = {}

    truth: Dict[str, Any] = {}
    for cf32_path, json_path in _iter_vector_sidecars(search_roots):
        rel = str(cf32_path.relative_to(ROOT))
        cmd: List[str] = [
            sys.executable,
            "-m",
            "scripts.sdr_lora_cli",
            "decode",
            str(cf32_path),
            "-v",
        ]
        if fast:
            cmd.append("--fast")
        try:
            proc = _subprocess_run(cmd, timeout=timeout)
        except subprocess.TimeoutExpired:
            truth[rel] = {"status": "timeout", "stderr": ""}
            continue

        if proc.returncode != 0:
            truth[rel] = {
                "status": "error",
                "returncode": proc.returncode,
                "stderr": proc.stderr.strip(),
            }
            continue

        try:
            data = json.loads(proc.stdout)
        except json.JSONDecodeError as exc:  # pragma: no cover - defensive
            truth[rel] = {
                "status": "error",
                "returncode": proc.returncode,
                "stderr": f"JSON decode failed: {exc}\nRaw: {proc.stdout[:200]}",
            }
            continue

        entry: Dict[str, Any] = {
            "status": "ok",
            "sf": data.get("sf"),
            "bw": data.get("bw"),
            "fs": data.get("fs"),
            "expected_hex": (data.get("expected") or "").lower(),
            "payload_hex": None,
            "count": 0,
        }
        found = data.get("found") or []
        entry["count"] = len(found)
        if found:
            first = found[0]
            entry["payload_hex"] = (first.get("hex") or "").lower()
            entry["hdr_ok"] = int(first.get("hdr_ok", 0))
            entry["crc_ok"] = int(first.get("crc_ok", 0))
            entry["cr"] = int(first.get("cr", 0))
            entry["ih"] = int(first.get("ih", 0))
        truth[rel] = entry

    merged = existing if keep_existing else {}
    merged.update(truth)

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(merged, indent=2))
    return {"written": len(truth), "total": len(merged), "output": str(output)}


# ---------------------------------------------------------------------------
# GNU Radio truth table generation
# ---------------------------------------------------------------------------


def run_gnur_truth(
    roots: Optional[Sequence[str | Path]] = None,
    *,
    output: Path = GNUR_TRUTH_DEFAULT,
    timeout: int = 180,
    limit: Optional[int] = None,
    conda_env: str = "gr310",
) -> Dict[str, Any]:
    """Generate GNU Radio truth tables via ``decode_offline_recording.py``."""

    search_roots = _resolve_root_paths(roots, default=DEFAULT_VECTOR_ROOTS)

    existing: Dict[str, Any] = {}
    if output.exists():
        try:
            existing = json.loads(output.read_text())
        except Exception:
            existing = {}

    results: Dict[str, Any] = {}
    vectors = list(_iter_vectors(search_roots))
    if limit is not None:
        vectors = vectors[:limit]

    for cf32 in vectors:
        rel = str(cf32.relative_to(ROOT))
        meta = _load_meta(cf32)
        samp_rate = int(meta.get("samp_rate") or meta.get("sample_rate") or meta.get("fs") or meta.get("bw", 0))
        cmd: List[str] = [
            "conda",
            "run",
            "-n",
            conda_env,
            "python",
            str(GR_SCRIPT),
            str(cf32),
            "--sf",
            str(meta.get("sf")),
            "--bw",
            str(meta.get("bw")),
            "--samp-rate",
            str(samp_rate),
            "--cr",
            str(meta.get("cr", 1)),
            "--ldro-mode",
            str(meta.get("ldro_mode", 2)),
            "--format",
            "cf32",
        ]
        cmd.append("--has-crc" if meta.get("crc", True) else "--no-crc")
        cmd.append("--impl-header" if meta.get("impl_header", False) else "--explicit-header")
        if "payload_len" in meta:
            cmd.extend(["--pay-len", str(meta.get("payload_len"))])
        if "sync_word" in meta:
            try:
                cmd.extend(["--sync-word", hex(int(meta["sync_word"]))])
            except Exception:
                pass

        try:
            proc = _subprocess_run(cmd, timeout=timeout)
        except subprocess.TimeoutExpired:
            results[rel] = {"status": "timeout", "stderr": ""}
            continue

        if proc.returncode != 0:
            results[rel] = {
                "status": "error",
                "returncode": proc.returncode,
                "stdout": proc.stdout,
                "stderr": proc.stderr,
            }
            continue

        payloads: List[str] = []
        crc_state: List[str] = []
        for line in proc.stdout.splitlines():
            line = line.strip()
            if line.startswith("Frame") and "CRC" in line:
                crc_state.append(line.split("CRC", 1)[1].strip())
            if line.startswith("Hex:"):
                hex_tokens = line.split(":", 1)[1].strip().split()
                payloads.append("".join(hex_tokens).lower())

        results[rel] = {
            "status": "ok",
            "payloads": payloads,
            "crc": crc_state,
            "stdout": proc.stdout,
        }

    existing.update(results)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(existing, indent=2))
    return {"written": len(results), "total": len(existing), "output": str(output)}


# ---------------------------------------------------------------------------
# Receiver comparison (C++ vs Python vs GNU Radio)
# ---------------------------------------------------------------------------


def run_receiver_comparison(
    roots: Optional[Sequence[str | Path]] = None,
    *,
    timeout: int = 45,
    fallback_skip: bool = False,
    limit: Optional[int] = None,
    output: Path = ROOT / "results" / "receiver_comparison.json",
    python_truth: Path = PY_TRUTH_DEFAULT,
    gnur_truth: Path = GNUR_TRUTH_DEFAULT,
) -> Dict[str, Any]:
    """Cross-check receivers and emit summary counts."""

    def load_truth(path: Path) -> Dict[str, Any]:
        if not path.exists():
            return {}
        try:
            return json.loads(path.read_text())
        except Exception:
            return {}

    py_truth = load_truth(python_truth)
    gnur_truth = load_truth(gnur_truth)

    roots_resolved = _resolve_root_paths(roots, default=DEFAULT_VECTOR_ROOTS)
    vectors = list(_iter_vectors(roots_resolved))
    if limit is not None:
        vectors = vectors[:limit]

    cpp_binary = _resolve_cpp_binary()
    if not cpp_binary:
        raise FileNotFoundError("decode_cli binary not found; build cpp_receiver first")

    counts: Dict[str, int] = {}
    entries: Dict[str, Any] = {}

    for cf32 in vectors:
        rel = str(cf32.relative_to(ROOT))
        try:
            meta = _load_meta(cf32)
        except Exception as exc:
            entries[rel] = {"status": "meta_error", "error": repr(exc)}
            counts["meta_error"] = counts.get("meta_error", 0) + 1
            continue

        def make_cmd(skip_syncword: bool = False) -> List[str]:
            cmd = [
                str(cpp_binary),
                "--sf",
                str(meta.get("sf")),
                "--bw",
                str(meta.get("bw")),
                "--fs",
                str(meta.get("samp_rate") or meta.get("fs") or 0),
                "--ldro",
                _ldro_flag(meta),
            ]
            if meta.get("impl_header"):
                cmd += [
                    "--implicit-header",
                    "--payload-len",
                    str(meta.get("payload_len", 0)),
                    "--cr",
                    str(meta.get("cr", 1)),
                    "--has-crc" if meta.get("crc", True) else "--no-crc",
                ]
            if "sync_word" in meta:
                try:
                    cmd += ["--sync-word", hex(int(meta["sync_word"]))]
                except Exception:
                    pass
            if skip_syncword:
                cmd.append("--skip-syncword")
            cmd.append(str(cf32))
            return cmd

        def run_cpp(cmd: List[str]) -> Tuple[bool, str, Dict[str, Any]]:
            try:
                proc = _subprocess_run(cmd, timeout=timeout)
            except subprocess.TimeoutExpired:
                return False, "", {"status": "timeout", "timeout_sec": timeout}
            if proc.returncode != 0:
                return False, "", {
                    "status": "error",
                    "ret": proc.returncode,
                    "stderr": proc.stderr[-400:],
                    "stdout": proc.stdout[-400:],
                }
            payload_hex = ""
            for line in proc.stdout.splitlines():
                if line.startswith("payload_hex="):
                    payload_hex = line.split("=", 1)[1].strip().lower()
                    break
            return True, payload_hex, {"status": "ok"}

        cpp_ok, cpp_hex, cpp_diag = run_cpp(make_cmd(False))
        if not cpp_ok and fallback_skip:
            cpp_ok2, cpp_hex2, cpp_diag2 = run_cpp(make_cmd(True))
            if cpp_ok2:
                cpp_ok, cpp_hex, cpp_diag = cpp_ok2, cpp_hex2, {"status": "ok", "used_skip_syncword": True}
            else:
                cpp_diag = {"first": cpp_diag, "second": cpp_diag2}

        py_info = py_truth.get(rel)
        gnur_info = gnur_truth.get(rel)

        py_hex = ((py_info or {}).get("payload_hex") or "").lower()
        gnur_payloads = [p.lower() for p in ((gnur_info or {}).get("payloads") or [])] if (gnur_info or {}).get("status") == "ok" else []

        py_ok = bool(py_hex)
        gnur_ok = bool(gnur_payloads)
        cpp_match_py = cpp_hex == py_hex if (py_ok and cpp_ok) else False
        cpp_match_gnur = cpp_hex in gnur_payloads if (gnur_ok and cpp_ok) else False

        if py_ok and cpp_ok and gnur_ok and cpp_match_py and cpp_match_gnur:
            category = "all_match"
        elif py_ok and cpp_ok and cpp_match_py and not gnur_ok:
            category = "cpp_python_only"
        elif cpp_ok and gnur_ok and cpp_match_gnur and not py_ok:
            category = "cpp_gnur_only"
        elif py_ok and gnur_ok and (py_hex in gnur_payloads) and not cpp_ok:
            category = "python_gnur_only"
        elif cpp_ok and py_ok and not cpp_match_py:
            category = "cpp_vs_python_mismatch"
        elif cpp_ok and gnur_ok and not cpp_match_gnur:
            category = "cpp_vs_gnur_mismatch"
        elif py_ok and gnur_ok and (py_hex not in gnur_payloads):
            category = "python_vs_gnur_mismatch"
        elif cpp_ok and not (py_ok or gnur_ok):
            category = "cpp_only"
        elif py_ok and not (cpp_ok or gnur_ok):
            category = "python_only"
        elif gnur_ok and not (cpp_ok or py_ok):
            category = "gnur_only"
        else:
            category = "all_fail"

        counts[category] = counts.get(category, 0) + 1
        entries[rel] = {
            "sf": meta.get("sf"),
            "cr": meta.get("cr"),
            "ldro_mode": meta.get("ldro_mode"),
            "impl_header": bool(meta.get("impl_header", False)),
            "python": py_info or {"status": "missing"},
            "gnuradio": gnur_info or {"status": "missing"},
            "cpp_ok": cpp_ok,
            "cpp_payload_hex": cpp_hex,
            "cpp_diag": cpp_diag,
            "category": category,
        }

    output.parent.mkdir(parents=True, exist_ok=True)
    summary = {"counts": counts, "entries": entries}
    output.write_text(json.dumps(summary, indent=2))
    return summary


# ---------------------------------------------------------------------------
# Runtime benchmarking
# ---------------------------------------------------------------------------


@dataclass
class BenchmarkResult:
    status: str
    avg_sec: Optional[float]
    std_sec: Optional[float]
    avg_user_sec: Optional[float]
    std_user_sec: Optional[float]
    avg_sys_sec: Optional[float]
    std_sys_sec: Optional[float]
    avg_maxrss_kb: Optional[float]
    std_maxrss_kb: Optional[float]
    stdout: Optional[str]
    returncode: Optional[int] = None
    stderr: Optional[str] = None


def _time_command(cmd: List[str], runs: int) -> BenchmarkResult:
    def _have_time_cmd() -> Optional[str]:
        for cand in ("/usr/bin/time", shutil.which("time")):
            if cand and os.path.exists(cand):
                return cand
        return None

    def _run_once(time_cmd: Optional[str]) -> Dict[str, Any]:
        if time_cmd:
            fmt = '{"wall":%e,"user":%U,"sys":%S,"max_kb":%M}'
            with tempfile.TemporaryDirectory() as tmp:
                out_path = Path(tmp) / "time.json"
                proc = subprocess.run([time_cmd, "-f", fmt, "-o", str(out_path), "--", *cmd], capture_output=True, text=True)
                try:
                    timing = json.loads(out_path.read_text())
                except Exception:
                    timing = {"wall": None, "user": None, "sys": None, "max_kb": None}
                return {
                    "returncode": proc.returncode,
                    "stdout": proc.stdout,
                    "stderr": proc.stderr,
                    "timing": timing,
                }
        start = time.perf_counter()
        proc = subprocess.run(cmd, capture_output=True, text=True)
        elapsed = time.perf_counter() - start
        return {
            "returncode": proc.returncode,
            "stdout": proc.stdout,
            "stderr": proc.stderr,
            "timing": {"wall": elapsed, "user": None, "sys": None, "max_kb": None},
        }

    time_cmd = _have_time_cmd()
    walls: List[float] = []
    users: List[float] = []
    syss: List[float] = []
    rss: List[int] = []
    last_stdout: Optional[str] = None
    last_stderr: Optional[str] = None

    for _ in range(runs):
        res = _run_once(time_cmd)
        last_stdout = res["stdout"]
        last_stderr = res["stderr"]
        if res["returncode"] != 0:
            return BenchmarkResult(
                status="error",
                avg_sec=None,
                std_sec=None,
                avg_user_sec=None,
                std_user_sec=None,
                avg_sys_sec=None,
                std_sys_sec=None,
                avg_maxrss_kb=None,
                std_maxrss_kb=None,
                stdout=last_stdout,
                returncode=res["returncode"],
                stderr=last_stderr,
            )
        timing = res["timing"]
        if timing.get("wall") is not None:
            walls.append(float(timing["wall"]))
        if timing.get("user") is not None:
            users.append(float(timing["user"]))
        if timing.get("sys") is not None:
            syss.append(float(timing["sys"]))
        if timing.get("max_kb") is not None:
            try:
                rss.append(int(timing["max_kb"]))
            except Exception:
                pass

    def _avg_std(values: List[float]) -> Tuple[Optional[float], Optional[float]]:
        if not values:
            return None, None
        avg = sum(values) / len(values)
        std = (sum((v - avg) ** 2 for v in values) / len(values)) ** 0.5
        return avg, std

    wall_avg, wall_std = _avg_std(walls)
    user_avg, user_std = _avg_std(users)
    sys_avg, sys_std = _avg_std(syss)
    rss_avg, rss_std = _avg_std([float(x) for x in rss]) if rss else (None, None)

    return BenchmarkResult(
        status="ok",
        avg_sec=wall_avg,
        std_sec=wall_std,
        avg_user_sec=user_avg,
        std_user_sec=user_std,
        avg_sys_sec=sys_avg,
        std_sys_sec=sys_std,
        avg_maxrss_kb=rss_avg,
        std_maxrss_kb=rss_std,
        stdout=last_stdout,
    )


def benchmark_receivers(
    cf32: Path,
    *,
    runs: int = 3,
    include_python: bool = True,
    include_cpp: bool = True,
    include_gnur: bool = False,
    fast: bool = True,
    conda_env: str = "gr310",
    meta_override: Optional[Dict[str, Any]] = None,
) -> Dict[str, BenchmarkResult]:
    """Benchmark different receivers on a single vector."""

    results: Dict[str, BenchmarkResult] = {}
    meta = meta_override or _load_meta(cf32)

    if include_cpp:
        cpp_binary = _resolve_cpp_binary()
        if not cpp_binary:
            raise FileNotFoundError("decode_cli binary not found; build cpp_receiver first")
        cmd = [
            str(cpp_binary),
            "--sf",
            str(meta["sf"]),
            "--bw",
            str(meta["bw"]),
            "--fs",
            str(meta.get("samp_rate") or meta.get("fs") or 0),
            "--ldro",
            _ldro_flag(meta),
            str(cf32),
        ]
        results["cpp"] = _time_command(cmd, runs)

    if include_python:
        cmd = [
            sys.executable,
            "-m",
            "scripts.sdr_lora_cli",
            "decode",
            str(cf32),
        ]
        if fast:
            cmd.append("--fast")
        results["python"] = _time_command(cmd, runs)

    if include_gnur:
        cmd = [
            "conda",
            "run",
            "-n",
            conda_env,
            "python",
            str(GR_SCRIPT),
            str(cf32),
            "--sf",
            str(meta["sf"]),
            "--bw",
            str(meta["bw"]),
            "--samp-rate",
            str(meta.get("samp_rate") or meta.get("fs") or meta["bw"]),
            "--cr",
            str(meta.get("cr", 1)),
            "--ldro-mode",
            str(meta.get("ldro_mode", 2)),
            "--format",
            "cf32",
        ]
        cmd.append("--has-crc" if meta.get("crc", True) else "--no-crc")
        cmd.append("--impl-header" if meta.get("impl_header", False) else "--explicit-header")
        results["gnuradio"] = _time_command(cmd, runs)

    return results


# ---------------------------------------------------------------------------
# Compatibility sweep wrapper
# ---------------------------------------------------------------------------


def run_gnur_cpp_compat(
    vectors_dir: Path,
    *,
    limit: Optional[int] = None,
    output: Path = ROOT / "gnu_radio_compat_results.json",
) -> Dict[str, Any]:
    """Invoke the GNU Radio vs C++ compatibility sweep programmatically."""

    from tests.test_gnu_radio_compat import main as compat_main

    argv: List[str] = [
        "test_gnu_radio_compat.py",
        "--vectors-dir",
        str(vectors_dir),
        "--output",
        str(output),
    ]
    if limit is not None:
        argv.extend(["--limit", str(limit)])

    old_argv = sys.argv
    old_cwd = Path.cwd()
    try:
        sys.argv = argv
        os.chdir(ROOT)
        compat_main()
    finally:
        sys.argv = old_argv
        os.chdir(old_cwd)

    return json.loads(output.read_text())


__all__ = [
    "run_python_truth",
    "run_gnur_truth",
    "run_receiver_comparison",
    "benchmark_receivers",
    "run_gnur_cpp_compat",
    "BenchmarkResult",
]


