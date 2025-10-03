#!/usr/bin/env python3
"""
Deprecated: use scripts.sdr_lora_batch_decode instead.

This shim forwards to the new batch decoder for backward compatibility.
"""

import sys

try:
    from .sdr_lora_batch_decode import main as _main  # type: ignore
except Exception as e:  # pragma: no cover
    sys.stderr.write(
        "[DEPRECATED] Please run 'python -m scripts.sdr_lora_batch_decode ...' (failed to import: %s)\n" % (e,)
    )
    raise SystemExit(2)

if __name__ == "__main__":
    raise SystemExit(_main())
