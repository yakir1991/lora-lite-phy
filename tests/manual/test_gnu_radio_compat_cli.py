#!/usr/bin/env python3
"""Command-line wrapper around the GNU Radio vs. C++ parity check."""

from __future__ import annotations

import sys
from pathlib import Path


def main() -> None:
    root = Path(__file__).resolve().parents[2]
    if str(root) not in sys.path:
        sys.path.insert(0, str(root))

    from tests.test_gnu_radio_compat import main as compat_main

    compat_main()


if __name__ == "__main__":
    main()

