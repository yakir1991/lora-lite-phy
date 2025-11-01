from __future__ import annotations

import subprocess
import sys
import time
import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Protocol, Sequence, Tuple


REPO_ROOT = Path(__file__).resolve().parents[2]
GR_SCRIPT = REPO_ROOT / "external/gr_lora_sdr/scripts/decode_offline_recording.py"
CPP_CANDIDATES: Tuple[Path, ...] = (
    REPO_ROOT / "cpp_receiver/build/decode_cli",
    REPO_ROOT / "cpp_receiver/build/Release/decode_cli",
    REPO_ROOT / "cpp_receiver/build/Debug/decode_cli",
    REPO_ROOT / "cpp_receiver/build/decode_cli.exe",
)


@dataclass
class CompareOptions:
    """Options shared by comparison backends."""

    streaming_chunk: int = 4096
    hdr_cfo_sweep: bool = False
    hdr_cfo_range: Optional[float] = None
    hdr_cfo_step: Optional[float] = None
    dump_header_iq_dir: Optional[Path] = None
    dump_header_iq_payload_syms: int = 128
    dump_header_iq_always: bool = False
    timeout_gr: float = 90.0
    timeout_cpp: float = 60.0
    enable_auto_cfo_sweep: bool = True


@dataclass
class DecodeResult:
    status: str
    payload_hex: Optional[str] = None
    stdout: str = ""
    stderr: str = ""
    command: Optional[str] = None
    duration_s: Optional[float] = None
    frames: List[Dict[str, Any]] = field(default_factory=list)
    extra: Dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> Dict[str, Any]:
        return {
            "status": self.status,
            "payload_hex": self.payload_hex,
            "stdout": self.stdout,
            "stderr": self.stderr,
            "command": self.command,
            "duration_s": self.duration_s,
            "frames": self.frames,
            "extra": self.extra,
        }


class Backend(Protocol):
    """Comparable decoder interface."""

    name: str

    def available(self) -> bool:
        ...

    def decode(self, vector_path: Path, metadata: Dict[str, Any], options: CompareOptions) -> DecodeResult:
        ...


def collect_vector_pairs(vectors_dir: Path) -> List[Tuple[Path, Path]]:
    """Return (cf32, json) pairs within a directory."""
    pairs: List[Tuple[Path, Path]] = []
    if not vectors_dir.exists():
        return pairs
    for cf32 in sorted(vectors_dir.glob("*.cf32")):
        js = cf32.with_suffix(".json")
        if js.exists():
            pairs.append((cf32, js))
    return pairs


def _extract_result_json(stdout: str) -> Optional[Dict[str, Any]]:
    for line in stdout.splitlines():
        s = line.strip()
        if s.startswith("result_json="):
            raw = s.split("=", 1)[1].strip()
            try:
                return json.loads(raw)
            except json.JSONDecodeError:
                return None
    return None


def _parse_gr_frames(stdout: str) -> List[Dict[str, Any]]:
    frames: List[Dict[str, Any]] = []
    for line in stdout.strip().splitlines():
        s = line.strip()
        if not s:
            continue
        if s.startswith("Frame") and ":" in s:
            frames.append({"info": s.split(":", 1)[1].strip()})
        elif s.startswith("Hex:") and frames:
            frames[-1]["hex"] = s.replace("Hex:", "").strip().replace(" ", "").lower()
        elif s.startswith("Text:") and frames:
            frames[-1]["text"] = s.replace("Text:", "").strip()
    return frames


def _resolve_cpp_binary() -> Path:
    for candidate in CPP_CANDIDATES:
        if candidate.exists():
            return candidate
    raise FileNotFoundError("decode_cli binary not found; build cpp_receiver first")


def _ldro_enabled(meta: Dict[str, Any]) -> bool:
    if "ldro_enabled" in meta:
        return bool(meta["ldro_enabled"])
    if "ldro" in meta:
        return bool(meta["ldro"])
    ldro_mode = meta.get("ldro_mode")
    if isinstance(ldro_mode, str):
        try:
            ldro_mode = int(ldro_mode)
        except ValueError:
            ldro_mode = None
    if ldro_mode in (1, True):
        return True
    if ldro_mode == 2:
        try:
            sf_val = int(meta.get("sf", 0))
            bw_val = float(meta.get("bw", 0))
            symbol_time = (1 << sf_val) / bw_val if bw_val else 0.0
            return symbol_time > 0.016
        except Exception:
            return False
    return False


def _channel_meta(meta: Dict[str, Any]) -> Dict[str, Any]:
    chan = meta.get("channel")
    return chan if isinstance(chan, dict) else {}


class GnuRadioBatchBackend:
    """Wraps the GNU Radio offline decoder script."""

    name = "gnu_radio_batch"

    def __init__(self, script: Optional[Path] = None):
        self.script = script or GR_SCRIPT

    def available(self) -> bool:
        return self.script.exists()

    def decode(self, vector_path: Path, metadata: Dict[str, Any], options: CompareOptions) -> DecodeResult:
        if not self.available():
            return DecodeResult(status="skipped", stderr=f"Missing GNU Radio script: {self.script}")

        cmd: List[str] = [
            sys.executable,
            str(self.script),
            str(vector_path),
            "--sf",
            str(metadata["sf"]),
            "--bw",
            str(metadata["bw"]),
            "--samp-rate",
            str(metadata.get("samp_rate") or metadata.get("sample_rate")),
            "--cr",
            str(metadata["cr"]),
            "--ldro-mode",
            str(metadata.get("ldro_mode", 0)),
            "--format",
            "cf32",
        ]
        cmd.append("--has-crc" if metadata.get("crc", True) else "--no-crc")
        cmd.append("--impl-header" if metadata.get("impl_header") or metadata.get("implicit_header") else "--explicit-header")

        start = time.perf_counter()
        try:
            res = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=options.timeout_gr,
                errors="replace",
            )
        except subprocess.TimeoutExpired:
            duration = time.perf_counter() - start
            return DecodeResult(
                status="timeout",
                stdout="",
                stderr="",
                command=" ".join(cmd),
                duration_s=duration,
            )

        duration = time.perf_counter() - start
        frames = _parse_gr_frames(res.stdout)
        result_json = _extract_result_json(res.stdout)

        success_flag = False
        if res.returncode == 0:
            if result_json and isinstance(result_json.get("success"), bool):
                success_flag = bool(result_json.get("success"))
            elif frames:
                success_flag = True

        status = "success" if success_flag else "failed"
        payload_hex: Optional[str] = None
        for fr in frames:
            if "hex" in fr:
                payload_hex = fr["hex"]
                break
        if payload_hex is None and result_json:
            raw_hex = result_json.get("payload_hex")
            if isinstance(raw_hex, str):
                payload_hex = raw_hex.strip().lower()

        extra = {
            "returncode": res.returncode,
            "frame_count": len(frames),
        }
        if result_json is not None:
            extra["result_json"] = result_json
        return DecodeResult(
            status=status,
            payload_hex=payload_hex,
            stdout=res.stdout,
            stderr=res.stderr,
            command=" ".join(cmd),
            duration_s=duration,
            frames=frames,
            extra=extra,
        )


class CppStreamingBackend:
    """Invokes decode_cli in streaming mode with optional CFO sweep heuristics."""

    name = "cpp_stream"

    def __init__(self, binary: Optional[Path] = None):
        self.binary = binary or _resolve_cpp_binary()

    def available(self) -> bool:
        return Path(self.binary).exists()

    def decode(self, vector_path: Path, metadata: Dict[str, Any], options: CompareOptions) -> DecodeResult:
        if not self.available():
            return DecodeResult(status="skipped", stderr=f"Missing decode_cli binary: {self.binary}")

        fs = int(metadata.get("samp_rate") or metadata.get("sample_rate"))
        channel = _channel_meta(metadata)
        expected_cfo = float(channel.get("cfo_hz", 0.0) or 0.0)
        cfo_drift = float(channel.get("cfo_drift_hz", 0.0) or 0.0)
        expected_ppm = float(channel.get("sampling_offset_ppm", 0.0) or 0.0)

        base_cmd: List[str] = [
            str(self.binary),
            "--sf",
            str(metadata["sf"]),
            "--bw",
            str(metadata["bw"]),
            "--fs",
            str(fs),
            "--streaming",
            str(vector_path),
        ]

        ldro_mode = metadata.get("ldro_mode")
        if ldro_mode is not None:
            try:
                base_cmd.extend(["--ldro-mode", str(int(ldro_mode))])
            except Exception:
                base_cmd.extend(["--ldro", "1" if _ldro_enabled(metadata) else "0"])
        else:
            base_cmd.extend(["--ldro", "1" if _ldro_enabled(metadata) else "0"])

        if expected_ppm != 0.0:
            base_cmd.extend(["--ppm-offset", f"{expected_ppm:.6f}"])

        implicit = bool(metadata.get("impl_header") or metadata.get("implicit_header"))
        has_crc = bool(metadata.get("crc", True))
        if implicit:
            payload_len = int(metadata.get("payload_len") or metadata.get("payload_length") or 0)
            cr = int(metadata.get("cr", 1))
            base_cmd.extend(
                [
                    "--implicit-header",
                    "--payload-len",
                    str(payload_len),
                    "--cr",
                    str(cr),
                    "--has-crc" if has_crc else "--no-crc",
                ]
            )

        sync_word = metadata.get("sync_word")
        if sync_word is not None:
            base_cmd.extend(["--sync-word", str(sync_word)])

        if options.streaming_chunk:
            base_cmd.extend(["--chunk", str(int(options.streaming_chunk))])

        if options.hdr_cfo_sweep:
            base_cmd.append("--hdr-cfo-sweep")
            if options.hdr_cfo_range is not None:
                base_cmd.extend(["--hdr-cfo-range", str(options.hdr_cfo_range)])
            if options.hdr_cfo_step is not None:
                base_cmd.extend(["--hdr-cfo-step", str(options.hdr_cfo_step)])

        dump_dir = options.dump_header_iq_dir
        if dump_dir:
            out_path = Path(dump_dir) / f"{vector_path.stem}_header.cf32"
            base_cmd.extend(["--dump-header-iq", str(out_path)])
            if options.dump_header_iq_payload_syms is not None:
                base_cmd.extend(
                    ["--dump-header-iq-payload-syms", str(options.dump_header_iq_payload_syms)]
                )
            if options.dump_header_iq_always:
                base_cmd.append("--dump-header-iq-always")

        attempts: List[Dict[str, Any]] = []

        def invoke(extra: Sequence[str], tag: str) -> Tuple[DecodeResult, bool]:
            cmd = [*base_cmd, *extra]
            start = time.perf_counter()
            try:
                res = subprocess.run(
                    cmd,
                    capture_output=True,
                    text=True,
                    timeout=options.timeout_cpp,
                    errors="replace",
                )
            except subprocess.TimeoutExpired:
                duration = time.perf_counter() - start
                attempt_summary = {
                    "tag": tag,
                    "status": "timeout",
                    "duration_s": duration,
                    "command": cmd,
                }
                attempts.append(attempt_summary)
                return (
                    DecodeResult(
                        status="timeout",
                        stdout="",
                        stderr="",
                        command=" ".join(cmd),
                        duration_s=duration,
                        extra={"attempts": attempts},
                    ),
                    False,
                )

            duration = time.perf_counter() - start
            payload_hex: Optional[str] = None
            for line in res.stdout.splitlines():
                if line.startswith("payload_hex="):
                    payload_hex = line.split("=", 1)[1].strip()
                    break
            result_json = _extract_result_json(res.stdout)
            if payload_hex is None and result_json:
                raw_hex = result_json.get("payload_hex")
                if isinstance(raw_hex, str):
                    payload_hex = raw_hex.strip().lower()
            status = "success" if res.returncode == 0 else "failed"
            attempt_summary = {
                "tag": tag,
                "status": status,
                "duration_s": duration,
                "payload_hex": payload_hex,
                "returncode": res.returncode,
                "command": " ".join(cmd),
            }
            if result_json is not None:
                attempt_summary["result_json"] = result_json
            attempts.append(attempt_summary)

            result = DecodeResult(
                status=status,
                payload_hex=payload_hex,
                stdout=res.stdout,
                stderr=res.stderr,
                command=" ".join(cmd),
                duration_s=duration,
                extra={"attempts": attempts},
            )
            if result_json is not None:
                result.extra["result_json"] = result_json
            return result, payload_hex is not None and status == "success"

        final_result, ok = invoke([], "base")
        if ok:
            return final_result

        if (
            options.enable_auto_cfo_sweep
            and "--hdr-cfo-sweep" not in base_cmd
            and (abs(expected_cfo) + abs(cfo_drift) > 15.0 or bool(channel))
        ):
            sweep_range = options.hdr_cfo_range
            if sweep_range is None:
                sweep_range = max(150.0, abs(expected_cfo) * 2.5 + abs(cfo_drift) * 20.0)
            sweep_step = options.hdr_cfo_step
            if sweep_step is None:
                sweep_step = max(10.0, sweep_range / 12.0)

            sweep_cmd = [
                "--hdr-cfo-sweep",
                "--hdr-cfo-range",
                f"{sweep_range:.3f}",
                "--hdr-cfo-step",
                f"{sweep_step:.3f}",
            ]
            final_result, ok = invoke(sweep_cmd, "auto_cfo_sweep")
            if ok:
                return final_result

            wider_cmd = [
                "--hdr-cfo-sweep",
                "--hdr-cfo-range",
                f"{max(sweep_range * 1.5, 300.0):.3f}",
                "--hdr-cfo-step",
                f"{max(sweep_step / 2.0, 5.0):.3f}",
            ]
            final_result, ok = invoke(wider_cmd, "auto_cfo_sweep_wide")
            if ok:
                return final_result

        # If not successful, ensure attempts list present in extra
        final_result.extra.setdefault("attempts", attempts)
        return final_result
