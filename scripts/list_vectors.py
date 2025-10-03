#!/usr/bin/env python3
"""
List available CF32+JSON vector pairs in common folders to help select a test input.

Usage:
  python scripts/list_vectors.py

Optional:
  Set LORA_TEST_CF32 and LORA_TEST_META_JSON to print the resolved pair status.
"""
from __future__ import annotations
import os
from pathlib import Path


def iter_pairs(root: Path):
    for cf in sorted(root.glob('*.cf32')):
        meta = cf.with_suffix('.json')
        yield cf, meta, meta.exists()


def main():
    base = Path(__file__).resolve().parents[1]
    folders = [
        base / 'golden_vectors_demo',
        base / 'golden_vectors_demo_batch',
        base / 'vectors',
    ]
    print('Vector pairs overview:')
    for folder in folders:
        print(f"- {folder}")
        if not folder.exists():
            print("   (missing)")
            continue
        count = 0
        for cf, meta, ok in iter_pairs(folder):
            print(f"   • {cf.name}  json={'yes' if ok else 'no'}")
            count += 1
            if count >= 20:
                print("   ... (truncated)")
                break
    env_cf32 = os.getenv('LORA_TEST_CF32')
    env_json = os.getenv('LORA_TEST_META_JSON')
    if env_cf32 or env_json:
        print('\nEnv overrides:')
        print(f"  LORA_TEST_CF32={env_cf32}")
        print(f"  LORA_TEST_META_JSON={env_json}")
        if env_cf32 and env_json:
            cf = Path(env_cf32)
            mj = Path(env_json)
            print(f"  Resolved: cf32={'ok' if cf.exists() else 'missing'}, json={'ok' if mj.exists() else 'missing'}")


if __name__ == '__main__':
    main()
