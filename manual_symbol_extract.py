#!/usr/bin/env python3
"""Deprecated wrapper. Archived under legacy/manual_symbol_extract.py. Prefer lora_cli.py."""
import sys, pathlib, runpy

def _go():
    target_rel = "legacy/manual_symbol_extract.py"
    here = pathlib.Path(__file__).resolve().parent
    target = here / target_rel
    if not target.exists():
        sys.stderr.write(f"[DEPRECATED] Target not found: {target_rel}\n")
        sys.exit(2)
    sys.stderr.write(f"[DEPRECATED] Redirecting to {target_rel}. Prefer lora_cli.py.\n")
    sys.argv[0] = str(target)
    runpy.run_path(str(target), run_name="__main__")
    sys.exit(0)

if __name__ == "__main__":
    _go()
