#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0

"""
Batch generator for LoRa IQ captures using the tx_rx_simulation flowgraph.

Example:
    PYTHONPATH=$PWD/gr_lora_sdr/install/lib/python3.12/site-packages \\
    LD_LIBRARY_PATH=$PWD/gr_lora_sdr/install/lib \\
    conda run -n gr310 python gr_lora_sdr/tools/generate_vectors.py
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from dataclasses import dataclass, asdict
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
EXAMPLE_SCRIPT = REPO_ROOT / "gr_lora_sdr" / "examples" / "tx_rx_simulation.py"
GENERATED_DIR = REPO_ROOT / "gr_lora_sdr" / "data" / "generated"


@dataclass(frozen=True)
class Scenario:
    sf: int
    bw: int
    cr: int
    snr_db: float
    sample_rate: int
    payload_len: int = 16
    preamble_len: int = 8
    duration_secs: float = 2.0

    def output_name(self) -> str:
        snr_token = ("p" if self.snr_db >= 0 else "m") + f"{abs(self.snr_db):.1f}".replace(".", "p")
        return f"tx_rx_sf{self.sf}_bw{self.bw}_cr{self.cr}_snr{snr_token}.cf32"


def run_scenario(scenario: Scenario) -> Path:
    GENERATED_DIR.mkdir(parents=True, exist_ok=True)

    env = os.environ.copy()
    env["LORA_SF"] = str(scenario.sf)
    env["LORA_BW"] = str(scenario.bw)
    env["LORA_CR"] = str(scenario.cr)
    env["LORA_SNR_DB"] = str(scenario.snr_db)
    env["LORA_SAMPLE_RATE"] = str(scenario.sample_rate)
    env["LORA_PAYLOAD_LEN"] = str(scenario.payload_len)
    env["LORA_PREAMBLE_LEN"] = str(scenario.preamble_len)
    env["LORA_OUTPUT_NAME"] = scenario.output_name()
    env["LORA_OUTPUT_DIR"] = str(GENERATED_DIR)
    env["LORA_AUTOSTOP_SECS"] = str(scenario.duration_secs)

    python_path = str(REPO_ROOT / "gr_lora_sdr" / "install" / "lib" / "python3.12" / "site-packages")
    existing_py = env.get("PYTHONPATH")
    env["PYTHONPATH"] = python_path if not existing_py else f"{python_path}{os.pathsep}{existing_py}"

    ld_path = str(REPO_ROOT / "gr_lora_sdr" / "install" / "lib")
    existing_ld = env.get("LD_LIBRARY_PATH")
    env["LD_LIBRARY_PATH"] = ld_path if not existing_ld else f"{ld_path}{os.pathsep}{existing_ld}"

    subprocess.run(
        [sys.executable, str(EXAMPLE_SCRIPT)],
        check=True,
        cwd=REPO_ROOT,
        env=env,
    )

    return GENERATED_DIR / scenario.output_name()


def write_metadata(capture_path: Path, scenario: Scenario) -> None:
    bytes_count = capture_path.stat().st_size
    sample_count = bytes_count // (4 * 2)  # complex float
    metadata = asdict(scenario)
    metadata.update(
        {
            "capture_file": capture_path.name,
            "sample_count": int(sample_count),
            "byte_size": int(bytes_count),
        }
    )
    meta_path = capture_path.with_suffix(".json")
    meta_path.write_text(json.dumps(metadata, indent=2))


def main() -> None:
    scenarios = [
        Scenario(sf=7, bw=125000, cr=1, snr_db=-5.0, sample_rate=500000),
        Scenario(sf=9, bw=125000, cr=3, snr_db=-2.0, sample_rate=500000),
        Scenario(sf=10, bw=250000, cr=4, snr_db=0.0, sample_rate=1000000),
    ]

    for scenario in scenarios:
        print(f"Generating capture for {scenario}")
        capture_path = run_scenario(scenario)
        write_metadata(capture_path, scenario)
        print(f"  -> {capture_path} ({capture_path.stat().st_size / (1024 * 1024):.1f} MiB)")


if __name__ == "__main__":
    main()
