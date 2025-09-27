#!/usr/bin/env python3
"""
Removed duplicate.

Run the canonical test suite instead:
    python -m scripts.lora_cli test [--quick-test|--test-vectors-dir ...]
or directly:
    python scripts/lora_test_suite.py ...
"""
import sys

sys.stderr.write(
        "[REMOVED] Use 'python -m scripts.lora_cli test ...' or 'python scripts/lora_test_suite.py ...'\n"
)
raise SystemExit(2)
