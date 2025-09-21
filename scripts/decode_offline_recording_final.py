#!/usr/bin/env python3
"""Final decode script for offline LoRa baseband capture using the new C++ pipeline.

This script reads complex32 IQ samples from disk, feeds them to the 
new C++ pipeline and prints the recovered payloads together with CRC information.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

import numpy as np

HEX_VALUE_RE = re.compile(r"^[0-9a-fA-F]+$")


def parse_int_field(text: str) -> int:
    """Parse an integer field that may be rendered in decimal or hexadecimal."""

    token = text.strip().split()[0]

    try:
        return int(token, 0)
    except ValueError:
        if HEX_VALUE_RE.fullmatch(token):
            return int(token, 16)
        raise


@dataclass
class FrameResult:
    index: int
    payload: bytes
    crc_valid: Optional[bool]
    has_crc: Optional[bool]
    message_text: Optional[str]
    frame_sync_info: Dict[str, Any]
    header_info: Dict[str, Any]

    def hex_payload(self) -> str:
        return " ".join(f"{byte:02x}" for byte in self.payload)


def parse_numeric_suffix(value: str) -> float:
    match = re.fullmatch(r"(?P<base>\d+(?:\.\d+)?)(?P<suffix>[kKmMgG]?)", value)
    if not match:
        raise ValueError(f"Cannot parse numeric value '{value}'")
    base = float(match.group("base"))
    suffix = match.group("suffix").lower()
    scale = {"": 1.0, "k": 1e3, "m": 1e6, "g": 1e9}[suffix]
    return base * scale


def infer_parameters_from_name(stem: str) -> Dict[str, object]:
    pattern = re.compile(
        r"sps_(?P<sps>[^_]+)_bw_(?P<bw>[^_]+)_sf_(?P<sf>\d+)_cr_(?P<cr>\d+)"
        r"_ldro_(?P<ldro>[^_]+)_crc_(?P<crc>[^_]+)_implheader_(?P<impl>[^_]+)"
    )
    match = pattern.search(stem)
    if not match:
        return {}

    params: Dict[str, object] = {
        "samp_rate": parse_numeric_suffix(match.group("sps")),
        "bw": parse_numeric_suffix(match.group("bw")),
        "sf": int(match.group("sf")),
        "cr": int(match.group("cr")),
        "has_crc": match.group("crc").lower() == "true",
        "impl_head": match.group("impl").lower() == "true",
    }

    ldro_raw = match.group("ldro").lower()
    if ldro_raw in {"true", "false"}:
        params["ldro_mode"] = 1 if ldro_raw == "true" else 0
    else:
        try:
            params["ldro_mode"] = int(ldro_raw)
        except ValueError:
            params["ldro_mode"] = 2  # default to auto when inference fails

    return params


def run_cpp_pipeline(input_file: Path, sf: int, sync_word: int = 0x34) -> Dict[str, Any]:
    """Run the C++ pipeline and parse its output."""
    pipeline_exe = Path(__file__).parent.parent / "build" / "test_gr_pipeline"
    
    if not pipeline_exe.exists():
        return {"error": f"Pipeline executable not found: {pipeline_exe}"}
    
    try:
        result = subprocess.run(
            [str(pipeline_exe), str(input_file)],
            capture_output=True,
            text=True,
            timeout=30
        )
        
        if result.returncode != 0:
            return {
                "success": False,
                "error": f"Pipeline failed with return code {result.returncode}",
                "stderr": result.stderr
            }
        
        # Parse the output
        output_lines = result.stdout.strip().split('\n')
        parsed_output = {
            "success": False,
            "failure_reason": None,
            "frame_sync": {},
            "header": {},
            "payload": {},
            "raw_output": result.stdout
        }
        
        for line in output_lines:
            line = line.strip()
            
            if line.startswith("Success: true"):
                parsed_output["success"] = True
            elif line.startswith("Success: false"):
                parsed_output["success"] = False
            elif line.startswith("Failure reason:"):
                parsed_output["failure_reason"] = line.split(":", 1)[1].strip()
            elif line.startswith("Frame sync detected:"):
                parsed_output["frame_sync"]["detected"] = "true" in line
            elif line.startswith("OS:"):
                parsed_output["frame_sync"]["os"] = int(line.split(":", 1)[1].strip())
            elif line.startswith("Phase:"):
                parsed_output["frame_sync"]["phase"] = int(line.split(":", 1)[1].strip())
            elif line.startswith("CFO:"):
                parsed_output["frame_sync"]["cfo"] = float(line.split(":", 1)[1].strip())
            elif line.startswith("STO:"):
                parsed_output["frame_sync"]["sto"] = int(line.split(":", 1)[1].strip())
            elif line.startswith("Sync detected:"):
                parsed_output["frame_sync"]["sync_detected"] = "true" in line
            elif line.startswith("Sync start sample:"):
                parsed_output["frame_sync"]["sync_start_sample"] = int(line.split(":", 1)[1].strip())
            elif line.startswith("Aligned start sample:"):
                parsed_output["frame_sync"]["aligned_start_sample"] = int(line.split(":", 1)[1].strip())
            elif line.startswith("Header start sample:"):
                parsed_output["frame_sync"]["header_start_sample"] = int(line.split(":", 1)[1].strip())
            elif line.startswith("Payload length:"):
                field = line.split(":", 1)[1]
                try:
                    parsed_output["header"]["payload_len"] = parse_int_field(field)
                except ValueError:
                    parsed_output["header"]["payload_len_raw"] = field.strip()
            elif line.startswith("CR:"):
                parsed_output["header"]["cr"] = int(line.split(":", 1)[1].strip())
            elif line.startswith("Has CRC:"):
                parsed_output["header"]["has_crc"] = "true" in line
            elif line.startswith("CRC OK:"):
                parsed_output["payload"]["crc_ok"] = "true" in line
            elif line.startswith("Length:"):
                field = line.split(":", 1)[1]
                try:
                    parsed_output["payload"]["length"] = parse_int_field(field)
                except ValueError:
                    parsed_output["payload"]["length_raw"] = field.strip()
            elif line.startswith("Data:"):
                # Parse hex bytes
                hex_part = line.split(":", 1)[1].strip()
                try:
                    bytes_list = [int(x, 16) for x in hex_part.split()]
                    parsed_output["payload"]["data"] = bytes_list
                except ValueError:
                    pass
        
        return parsed_output
        
    except subprocess.TimeoutExpired:
        return {
            "success": False,
            "error": "Pipeline timed out"
        }
    except Exception as e:
        return {
            "success": False,
            "error": f"Failed to run pipeline: {e}"
        }


def process_pipeline_result(result: Dict[str, Any]) -> List[FrameResult]:
    """Process pipeline result and extract frame information."""
    frames = []
    
    if not result.get("success", False):
        print(f"Pipeline failed: {result.get('failure_reason', result.get('error', 'Unknown error'))}")
        return frames
    
    # Extract frame sync information
    frame_sync_info = result.get("frame_sync", {})
    
    # Extract header information
    header_info = result.get("header", {})
    
    # Extract payload information
    payload_data = result.get("payload", {})
    payload_bytes = bytes(payload_data.get("data", []))
    crc_valid = payload_data.get("crc_ok", False)
    
    # Try to decode as text
    message_text = None
    try:
        message_text = payload_bytes.decode("utf-8")
    except UnicodeDecodeError:
        try:
            message_text = payload_bytes.decode("latin-1", errors="replace")
        except:
            pass
    
    # Determine CRC presence
    has_crc = header_info.get("has_crc", None)
    
    frame = FrameResult(
        index=0,
        payload=payload_bytes,
        crc_valid=crc_valid,
        has_crc=has_crc,
        message_text=message_text,
        frame_sync_info=frame_sync_info,
        header_info=header_info,
    )
    
    frames.append(frame)
    return frames


def main() -> None:
    parser = argparse.ArgumentParser(description="Decode a stored LoRa IQ vector using the new C++ pipeline")
    parser.add_argument("input", type=Path, help="Path to the binary IQ capture")
    parser.add_argument("--sf", type=int, help="Spreading factor")
    parser.add_argument("--bw", type=float, help="Signal bandwidth in Hz")
    parser.add_argument("--samp-rate", type=float, help="Sampling rate in Hz (not used by new pipeline)")
    parser.add_argument("--cr", type=int, help="Coding rate (1-4) (not used by new pipeline)")
    parser.add_argument("--has-crc", dest="has_crc", action="store_true", help="Force CRC usage")
    parser.add_argument("--no-crc", dest="has_crc", action="store_false", help="Force CRC disabled")
    parser.add_argument("--impl-header", dest="impl_head", action="store_true", help="Use implicit header (not used by new pipeline)")
    parser.add_argument("--explicit-header", dest="impl_head", action="store_false", help="Use explicit header (not used by new pipeline)")
    parser.add_argument("--ldro-mode", type=int, choices=[0, 1, 2], help="Low data-rate optimisation mode (0=off,1=on,2=auto)")
    parser.add_argument("--sync-word", type=lambda x: int(x, 0), default=0x34, help="Sync word as integer")
    parser.add_argument("--pay-len", type=int, default=255, help="Maximum payload length (not used by new pipeline)")
    parser.add_argument("--limit", type=int, default=None, help="Process only the first N IQ samples")
    parser.add_argument("--print-tags", action="store_true", help="Dump full frame_info tag dictionaries")
    parser.add_argument("--dump-json", type=Path, help="Write detailed frame information to a JSON file")
    parser.add_argument("--show-messages", action="store_true", help="Print raw GNU Radio messages captured on the RX out port (not applicable to new pipeline)")
    parser.set_defaults(has_crc=None, impl_head=None)

    args = parser.parse_args()

    file_path = args.input.expanduser().resolve()
    if not file_path.exists():
        raise FileNotFoundError(file_path)

    inferred = infer_parameters_from_name(file_path.stem)

    # Determine parameters
    sf = args.sf or inferred.get("sf", 7)
    bw = args.bw or inferred.get("bw", 125000.0)
    sync_word = args.sync_word
    has_crc = args.has_crc if args.has_crc is not None else inferred.get("has_crc", True)
    
    print(f"Processing file: {file_path}")
    print(f"Parameters:")
    print(f"  SF: {sf}")
    print(f"  Bandwidth: {bw} Hz")
    print(f"  Sync word: 0x{sync_word:02x}")
    print(f"  Expect CRC: {has_crc}")

    # Run the C++ pipeline
    print("Running new C++ pipeline...")
    result = run_cpp_pipeline(file_path, sf, sync_word)
    
    if "error" in result:
        print(f"Error: {result['error']}")
        if "stderr" in result:
            print(f"Stderr: {result['stderr']}")
        return
    
    # Process results
    frames = process_pipeline_result(result)

    total_frames = len(frames)
    print(f"Decoded {total_frames} frame(s)")

    if not frames:
        print("No frames were decoded")
        if args.dump_json:
            dump_path = args.dump_json.expanduser()
            dump_path.parent.mkdir(parents=True, exist_ok=True)
            with dump_path.open("w", encoding="utf-8") as fh:
                json.dump(
                    {
                        "input_file": str(file_path),
                        "parameters": {
                            "sf": sf,
                            "bw": bw,
                            "sync_word": sync_word,
                            "expect_crc": has_crc,
                        },
                        "frames": [],
                        "pipeline_result": result,
                    },
                    fh,
                    indent=2,
                )
        return

    for frame in frames:
        crc_state = "unknown"
        if frame.crc_valid is True:
            crc_state = "valid"
        elif frame.crc_valid is False:
            crc_state = "invalid"
        elif frame.has_crc is False:
            crc_state = "not present"
        
        print(f"Frame {frame.index}: {len(frame.payload)} bytes, CRC {crc_state}")
        print(f"  Hex: {frame.hex_payload()}")
        
        if frame.message_text is not None:
            print(f"  Text: {frame.message_text}")
        
        if args.print_tags:
            print(f"  Frame sync info: {json.dumps(frame.frame_sync_info, indent=2)}")
            print(f"  Header info: {json.dumps(frame.header_info, indent=2)}")

    if args.dump_json:
        dump_path = args.dump_json.expanduser()
        dump_path.parent.mkdir(parents=True, exist_ok=True)
        dump_payload = [
            {
                "index": frame.index,
                "payload_hex": frame.hex_payload(),
                "payload_text": frame.message_text,
                "payload_len": len(frame.payload),
                "crc_valid": frame.crc_valid,
                "has_crc": frame.has_crc,
                "frame_sync_info": frame.frame_sync_info,
                "header_info": frame.header_info,
            }
            for frame in frames
        ]
        with dump_path.open("w", encoding="utf-8") as fh:
            json.dump(
                {
                    "input_file": str(file_path),
                    "parameters": {
                        "sf": sf,
                        "bw": bw,
                        "sync_word": sync_word,
                        "expect_crc": has_crc,
                    },
                    "frames": dump_payload,
                    "pipeline_result": result,
                },
                fh,
                indent=2,
            )


if __name__ == "__main__":
    main()
