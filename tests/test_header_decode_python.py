#!/usr/bin/env python3
import json
import os
from pathlib import Path
from typing import Optional, Tuple, List
import pytest


def _load_meta(p: Path) -> dict:
    with open(p, 'r') as f:
        return json.load(f)


def _resolve_vector_pair() -> Optional[Tuple[Path, Path]]:
    base = Path(__file__).resolve().parents[1]
    # 1) Env overrides
    env_cf32 = os.getenv('LORA_TEST_CF32')
    env_json = os.getenv('LORA_TEST_META_JSON')
    if env_cf32 and env_json:
        cf32 = Path(env_cf32)
        meta = Path(env_json)
        if cf32.exists() and meta.exists():
            return cf32, meta
    # 2) Default demo pair
    demo_cf32 = base / "golden_vectors_demo/tx_sf7_bw125000_cr2_crc1_impl0_ldro2_pay11.cf32"
    demo_json = base / "golden_vectors_demo/tx_sf7_bw125000_cr2_crc1_impl0_ldro2_pay11.json"
    if demo_cf32.exists() and demo_json.exists():
        return demo_cf32, demo_json
    # 3) Search in batch folder
    batch = base / "golden_vectors_demo_batch"
    if batch.exists():
        for cf32 in sorted(batch.glob("*.cf32")):
            meta = cf32.with_suffix('.json')
            if meta.exists():
                return cf32, meta
    # 4) Search in generic vectors
    vecs = base / "vectors"
    if vecs.exists():
        for cf32 in sorted(vecs.glob("*.cf32")):
            meta = cf32.with_suffix('.json')
            if meta.exists():
                return cf32, meta
    return None


def test_header_decode_matches_metadata():
    pair = _resolve_vector_pair()
    if not pair:
        # Build diagnostic details for skip message
        base = Path(__file__).resolve().parents[1]
        searched_dirs: List[str] = [
            str(base / 'golden_vectors_demo'),
            str(base / 'golden_vectors_demo_batch'),
            str(base / 'vectors'),
        ]
        existing: List[str] = []
        missing: List[str] = []
        for d in searched_dirs:
            p = Path(d)
            if p.exists():
                # Show up to 3 cf32 files and if their json exists
                samples = []
                for cf in sorted(p.glob('*.cf32'))[:3]:
                    meta = cf.with_suffix('.json')
                    samples.append(f"{cf.name} (json={'yes' if meta.exists() else 'no'})")
                existing.append(f"{d}: {', '.join(samples) if samples else 'no .cf32 files'}")
            else:
                missing.append(d)
        msg = (
            "No suitable vector+metadata pair found. "
            "Set LORA_TEST_CF32 and LORA_TEST_META_JSON to force a specific file.\n"
            f"Searched: {', '.join(searched_dirs)}\n"
            f"Existing: {' | '.join(existing) if existing else 'None'}\n"
            f"Missing: {', '.join(missing) if missing else 'None'}"
        )
        pytest.skip(msg)
    cf32, meta_path = pair

    meta = _load_meta(meta_path)

    # Import the modular receiver
    try:
        from receiver.receiver import LoRaReceiver  # type: ignore
    except Exception as e:  # pragma: no cover
        pytest.skip(f"receiver package removed: {e}")

    # Instantiate with metadata parameters; do not enable oracle shortcuts
    rx = LoRaReceiver(
        sf=int(meta.get("sf", 7)),
        bw=int(meta.get("bw", 125000)),
        cr=int(meta.get("cr", 2)),
        has_crc=bool(meta.get("crc", True)),
        impl_head=bool(meta.get("impl_header", False)),
        ldro_mode=int(meta.get("ldro_mode", 0)),
        samp_rate=int(meta.get("samp_rate", meta.get("bw", 125000))),
        sync_words=meta.get("sync_words") or ([int(meta.get("sync_word"))] if meta.get("sync_word") is not None else [0x12]),
        adaptive=True,
        oracle_assist=False,
    )

    res = rx.decode_file(str(cf32))
    assert isinstance(res, dict), "Receiver returned non-dict result"
    assert res.get("status") in ("extracted", "decoded"), f"Unexpected status: {res.get('status')}"

    # Header fields must exist and be plausible
    hdr = res.get("header_fields")
    assert isinstance(hdr, dict), f"Missing header_fields in result: keys={list(res.keys())}"

    L = hdr.get("payload_len")
    cr_idx = hdr.get("cr_idx")
    has_crc = hdr.get("has_crc")

    assert isinstance(L, int) and 1 <= L <= 255, f"Invalid payload_len parsed: {L}"
    assert isinstance(cr_idx, int) and 0 <= cr_idx <= 7, f"Invalid cr_idx parsed: {cr_idx}"
    assert isinstance(has_crc, bool), f"Invalid has_crc parsed: {has_crc}"

    # Match against metadata where applicable
    if isinstance(meta.get("payload_len"), int):
        assert L == int(meta["payload_len"]) , f"payload_len mismatch: {L} != {meta['payload_len']}"
    # LoRa CR index in header (0..7) vs metadata CR (1..4) when explicit header is used.
    if meta.get("impl_header") is False:
        expected = int(meta.get("cr", 2))
        expected_idx_options = {expected, expected - 1}
        assert cr_idx in expected_idx_options, f"cr_idx {cr_idx} not in expected {expected_idx_options} for CR={meta.get('cr')}"
    if isinstance(meta.get("crc"), bool):
        assert bool(has_crc) == bool(meta["crc"]) , f"has_crc mismatch: {has_crc} != {meta['crc']}"

    # Sanity: header previews should exist
    hdr_gray = res.get("header_gray_symbols") or []
    assert len(hdr_gray) >= 8, "Expected at least 8 header gray symbols in preview"
