#!/usr/bin/env python3
"""
Thin wrapper to keep the historical entrypoint name while delegating to the
modular receiver package. Uses a dynamic import to be robust in both
package and flat-script contexts.
"""

from __future__ import annotations

import importlib.util
import pathlib
import sys


def _load_cli_main() -> callable:
    base = pathlib.Path(__file__).resolve().parent
    # Prefer package import if available on sys.path
    try:
        from receiver.cli import main as entry  # type: ignore
        return entry
    except Exception:
        pass
    # Fallback: load module directly from file
    sys.path.insert(0, str(base))
    cli_path = base / "receiver" / "cli.py"
    spec = importlib.util.spec_from_file_location("receiver.cli", cli_path)
    if spec is None or spec.loader is None:  # pragma: no cover
        raise RuntimeError("Failed to locate receiver/cli.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)  # type: ignore[attr-defined]
    return getattr(module, "main")


def main(argv=None) -> int:
    entry = _load_cli_main()
    return entry(argv)


if __name__ == "__main__":
    raise SystemExit(main())
