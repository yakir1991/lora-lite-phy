#!/usr/bin/env python3
"""
Removed duplicate.

Use instead:
    python -m scripts.lora_cli analyze --which symbols
or run directly:
    python analysis/advanced_demod_analysis.py
"""
import sys

sys.stderr.write(
        "[REMOVED] Use 'python -m scripts.lora_cli analyze --which symbols' or 'python analysis/advanced_demod_analysis.py'\n"
)
raise SystemExit(2)
