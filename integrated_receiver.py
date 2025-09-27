#!/usr/bin/env python3
"""Deprecated wrapper.
Use: python lora_cli.py analyze integrated
This file redirects to analysis/integrated_receiver.py for backward compatibility.
"""
import sys
sys.stderr.write("[REMOVED] Use 'python -m scripts.lora_cli analyze --which integrated' or 'python analysis/integrated_receiver.py'\n")
raise SystemExit(2)
