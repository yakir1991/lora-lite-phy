import json
import os
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
GR_SCRIPT = ROOT / "external" / "gr_lora_sdr" / "scripts" / "decode_offline_recording.py"
GOLDEN_IQ = ROOT / "golden_vectors_demo" / "tx_sf7_bw125000_cr2_crc1_impl0_ldro2_pay11.cf32"
GOLDEN_META = GOLDEN_IQ.with_suffix('.json')


def _have_gnuradio_script() -> bool:
    return GR_SCRIPT.exists()


@pytest.mark.skipif(not GOLDEN_IQ.exists() or not GOLDEN_META.exists(), reason="golden demo vector not present")
def test_receiver_does_not_use_known_position(tmp_path):
    env = os.environ.copy()
    # Ensure known-position shortcut is disabled
    env.pop('LORA_ALLOW_KNOWN_POS', None)
    out_json = tmp_path / "result.json"
    cmd = [
        os.environ.get('PYTHON', 'python3'),
        "-m", "receiver.cli",
        "--sf", "7", "--bw", "125000", "--cr", "2", "--samp-rate", "500000", "--ldro-mode", "2",
        "--output", str(out_json),
        str(GOLDEN_IQ),
    ]
    r = subprocess.run(cmd, cwd=str(ROOT), env=env, capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, f"receiver run failed: {r.stderr}\nstdout={r.stdout}"
    data = json.loads(out_json.read_text())
    assert data.get('status') != 'error', f"receiver error: {data}"
    # Must not use known-position shortcut
    assert isinstance(data.get('sync_info'), dict)
    assert data['sync_info'].get('method') != 'known_position'
    # Must not use oracle assist by default
    assert 'oracle_info' not in data


@pytest.mark.skipif(not _have_gnuradio_script(), reason="GNU Radio offline decoder script not found")
@pytest.mark.skipif(not GOLDEN_IQ.exists() or not GOLDEN_META.exists(), reason="golden demo vector not present")
def test_redecode_to_gr_length_when_mismatch(tmp_path):
    env = os.environ.copy()
    env.pop('LORA_ALLOW_KNOWN_POS', None)
    out_json = tmp_path / "result.json"
    cmd = [
        os.environ.get('PYTHON', 'python3'),
        "-m", "receiver.cli",
        "--sf", "7", "--bw", "125000", "--cr", "2", "--samp-rate", "500000", "--ldro-mode", "2",
        "--compare-gnuradio", "--gr-dump-stages",
        "--output", str(out_json),
        str(GOLDEN_IQ),
    ]
    r = subprocess.run(cmd, cwd=str(ROOT), env=env, capture_output=True, text=True, timeout=240)
    assert r.returncode == 0, f"receiver run failed: {r.stderr}\nstdout={r.stdout}"
    data = json.loads(out_json.read_text())
    assert data.get('status') != 'error', f"receiver error: {data}"
    gr = data.get('gnuradio_compare') or {}
    assert gr.get('status') == 'success', f"GR compare failed: {gr}"
    gr_hex = (gr.get('gr_payload_hex') or '').strip()
    our_hex = (gr.get('our_payload_hex') or '').strip()
    if gr_hex and our_hex and gr_hex.lower() != our_hex.lower():
        # Expect re-decode helper to kick in and provide exact GR length
        rd = data.get('redecode_to_gr_length')
        assert isinstance(rd, dict), f"missing redecode_to_gr_length when mismatch: {data.keys()}"
        assert 'payload_hex' in rd and isinstance(rd['payload_hex'], str)
        assert len(rd['payload_hex']) == len(gr_hex)
