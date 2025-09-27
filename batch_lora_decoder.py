#!/usr/bin/env python3
"""
Removed duplicate.

Run the canonical tool instead:
    python -m scripts.lora_cli batch <inputs> [options]
or directly:
    python scripts/batch_lora_decoder.py ...
"""
import sys

sys.stderr.write(
        "[REMOVED] Use 'python -m scripts.lora_cli batch ...' or 'python scripts/batch_lora_decoder.py ...'\n"
)
raise SystemExit(2)
