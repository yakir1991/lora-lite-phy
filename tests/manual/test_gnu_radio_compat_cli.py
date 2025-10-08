#!/usr/bin/env python3
# This file provides the 'test gnu radio compat cli' functionality for the LoRa Lite PHY toolkit.
"""Command-line wrapper around the GNU Radio vs. C++ parity check."""

# Imports specific objects with 'from __future__ import annotations'.
from __future__ import annotations

# Imports the module(s) sys.
import sys
# Imports specific objects with 'from pathlib import Path'.
from pathlib import Path


# Defines the function main.
def main() -> None:
    # Executes the statement `root = Path(__file__).resolve().parents[2]`.
    root = Path(__file__).resolve().parents[2]
    # Begins a conditional branch to check a condition.
    if str(root) not in sys.path:
        # Executes the statement `sys.path.insert(0, str(root))`.
        sys.path.insert(0, str(root))

    # Imports specific objects with 'from tests.test_gnu_radio_compat import main as compat_main'.
    from tests.test_gnu_radio_compat import main as compat_main

    # Executes the statement `compat_main()`.
    compat_main()


# Begins a conditional branch to check a condition.
if __name__ == "__main__":
    # Executes the statement `main()`.
    main()

