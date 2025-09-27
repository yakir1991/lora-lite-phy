#!/usr/bin/env python3
"""Deprecated wrapper.
This tool was archived under legacy/. Prefer using lora_cli.py where applicable.
"""
import sys, pathlib, runpy

def _redirect(target_rel: str) -> None:
    here = pathlib.Path(__file__).resolve().parent
    target = here / target_rel
    if not target.exists():
        sys.stderr.write(f"[DEPRECATED] Target not found: {target_rel}\n")
        sys.stderr.write("This script moved under legacy/. Consider using lora_cli.py.\n")
        sys.exit(2)
    sys.stderr.write(f"[DEPRECATED] Redirecting to {target_rel}. Prefer lora_cli.py.\n")
    sys.argv[0] = str(target)
    runpy.run_path(str(target), run_name="__main__")
    sys.exit(0)

if __name__ == "__main__":
    _redirect("legacy/advanced_symbol_extraction.py")
