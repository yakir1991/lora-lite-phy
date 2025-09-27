#!/usr/bin/env python3
"""
Deprecated entry point.

Use the unified CLI module instead:
  python -m scripts.lora_cli [subcommand] [options]

This thin wrapper forwards execution to scripts.lora_cli for backward compatibility.
"""
import sys

try:
    from scripts.lora_cli import main as _main  # type: ignore
except Exception as e:  # pragma: no cover
    sys.stderr.write(
        "[DEPRECATED] Please run 'python -m scripts.lora_cli ...' (failed to import: %s)\n" % (e,)
    )
    raise SystemExit(2)

if __name__ == '__main__':
    raise SystemExit(_main())
