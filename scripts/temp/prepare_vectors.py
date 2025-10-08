#!/usr/bin/env python3

# This file provides the 'prepare vectors' functionality for the LoRa Lite PHY toolkit.
"""Prep legacy vectors for GNU Radio vs C++ compatibility testing.

This utility scans the `/home/aquay/lora-lite-phy/vectors` directory for
binary captures that still use historic extensions such as `.unknown` or
`.bin`. For each capture it attempts to derive LoRa metadata from the file
name, creates a `.cf32` copy (if one does not already exist), and emits a
matching `.json` file so that `tests/test_gnu_radio_compat.py` can load the
vector.

Only minimal metadata required by the compatibility test is populated:
`sf`, `bw`, `samp_rate`, `cr`, `ldro_mode`, `crc`, and `impl_header`.
"""

# Imports specific objects with 'from __future__ import annotations'.
from __future__ import annotations

# Imports the module(s) json.
import json
# Imports the module(s) re.
import re
# Imports the module(s) shutil.
import shutil
# Imports specific objects with 'from dataclasses import dataclass'.
from dataclasses import dataclass
# Imports specific objects with 'from pathlib import Path'.
from pathlib import Path
# Imports specific objects with 'from typing import Dict, Optional, Tuple'.
from typing import Dict, Optional, Tuple


# Executes the statement `VECTOR_DIR = Path("/home/aquay/lora-lite-phy/vectors")`.
VECTOR_DIR = Path("/home/aquay/lora-lite-phy/vectors")
# Executes the statement `SUPPORTED_SUFFIXES = {".unknown", ".bin"}`.
SUPPORTED_SUFFIXES = {".unknown", ".bin"}


# Executes the statement `UNIT_MULTIPLIERS = {`.
UNIT_MULTIPLIERS = {
    # Executes the statement `"": 1,`.
    "": 1,
    # Executes the statement `"k": 1_000,`.
    "k": 1_000,
    # Executes the statement `"khz": 1_000,`.
    "khz": 1_000,
    # Executes the statement `"m": 1_000_000,`.
    "m": 1_000_000,
    # Executes the statement `"mhz": 1_000_000,`.
    "mhz": 1_000_000,
# Closes the previously opened dictionary or set literal.
}


# Executes the statement `TRUE_TOKENS = {"true", "t", "1", "yes", "on"}`.
TRUE_TOKENS = {"true", "t", "1", "yes", "on"}
# Executes the statement `FALSE_TOKENS = {"false", "f", "0", "no", "off"}`.
FALSE_TOKENS = {"false", "f", "0", "no", "off"}


# Executes the statement `@dataclass`.
@dataclass
# Declares the class ParsedVector.
class ParsedVector:
    # Executes the statement `source_path: Path`.
    source_path: Path
    # Executes the statement `cf32_path: Path`.
    cf32_path: Path
    # Executes the statement `metadata_path: Path`.
    metadata_path: Path
    # Executes the statement `metadata: Dict[str, object]`.
    metadata: Dict[str, object]


# Defines the function _split_tokens.
def _split_tokens(stem: str) -> Tuple[str, ...]:
    """Split the filename stem into lower-case tokens."""

    # Executes the statement `tokens = tuple(token for token in re.split(r"[_\-]+", stem) if token)`.
    tokens = tuple(token for token in re.split(r"[_\-]+", stem) if token)
    # Returns the computed value to the caller.
    return tokens


# Defines the function _parse_unit.
def _parse_unit(value: str) -> Optional[int]:
    # Executes the statement `value = value.strip().lower()`.
    value = value.strip().lower()
    # Begins a conditional branch to check a condition.
    if not value:
        # Returns the computed value to the caller.
        return None

    # Starts a loop iterating over a sequence.
    for suffix, multiplier in UNIT_MULTIPLIERS.items():
        # Begins a conditional branch to check a condition.
        if suffix and value.endswith(suffix):
            # Executes the statement `number_part = value[:-len(suffix)]`.
            number_part = value[:-len(suffix)]
            # Exits the nearest enclosing loop early.
            break
    # Provides the fallback branch when previous conditions fail.
    else:
        # Executes the statement `number_part = value`.
        number_part = value
        # Executes the statement `multiplier = 1`.
        multiplier = 1

    # Begins a conditional branch to check a condition.
    if not number_part:
        # Returns the computed value to the caller.
        return None

    # Begins a block that monitors for exceptions.
    try:
        # Executes the statement `numeric = float(number_part)`.
        numeric = float(number_part)
    # Handles a specific exception from the try block.
    except ValueError:
        # Returns the computed value to the caller.
        return None

    # Returns the computed value to the caller.
    return int(numeric * multiplier)


# Defines the function _coerce_bool.
def _coerce_bool(token: str, default: Optional[bool] = None) -> Optional[bool]:
    # Executes the statement `lower = token.lower()`.
    lower = token.lower()
    # Begins a conditional branch to check a condition.
    if lower in TRUE_TOKENS:
        # Returns the computed value to the caller.
        return True
    # Begins a conditional branch to check a condition.
    if lower in FALSE_TOKENS:
        # Returns the computed value to the caller.
        return False
    # Returns the computed value to the caller.
    return default


# Defines the function _map_cr.
def _map_cr(value: str) -> Optional[int]:
    # Executes the statement `value = value.strip().lower()`.
    value = value.strip().lower()
    # Begins a conditional branch to check a condition.
    if not value:
        # Returns the computed value to the caller.
        return None

    # Begins a conditional branch to check a condition.
    if value.isdigit():
        # Executes the statement `numeric = int(value)`.
        numeric = int(value)
        # Begins a conditional branch to check a condition.
        if 1 <= numeric <= 4:
            # Returns the computed value to the caller.
            return numeric
        # Begins a conditional branch to check a condition.
        if 45 <= numeric <= 48:
            # Returns the computed value to the caller.
            return numeric - 44
        # Begins a conditional branch to check a condition.
        if 55 <= numeric <= 58:
            # Returns the computed value to the caller.
            return numeric - 54
    # Begins a conditional branch to check a condition.
    if value.startswith("4/") and value[2:].isdigit():
        # Executes the statement `denominator = int(value[2:])`.
        denominator = int(value[2:])
        # Begins a conditional branch to check a condition.
        if 5 <= denominator <= 8:
            # Returns the computed value to the caller.
            return denominator - 4
    # Returns the computed value to the caller.
    return None


# Defines the function _parse_metadata.
def _parse_metadata(stem: str) -> Dict[str, object]:
    # Executes the statement `tokens = _split_tokens(stem)`.
    tokens = _split_tokens(stem)
    # Executes the statement `metadata: Dict[str, object] = {}`.
    metadata: Dict[str, object] = {}

    # Executes the statement `i = 0`.
    i = 0
    # Starts a loop that continues while the condition holds.
    while i < len(tokens):
        # Executes the statement `token = tokens[i].lower()`.
        token = tokens[i].lower()

        # Key-value pairs separated by delimiters (e.g., `bw`, `125k`).
        # Executes the statement `handled = False`.
        handled = False
        # Begins a conditional branch to check a condition.
        if token in {"bw", "bandwidth"} and i + 1 < len(tokens):
            # Executes the statement `parsed = _parse_unit(tokens[i + 1])`.
            parsed = _parse_unit(tokens[i + 1])
            # Begins a conditional branch to check a condition.
            if parsed:
                # Executes the statement `metadata["bw"] = parsed`.
                metadata["bw"] = parsed
                # Executes the statement `handled = True`.
                handled = True
                # Executes the statement `i += 2`.
                i += 2
        # Handles an additional condition in the branching logic.
        elif token in {"sf", "spreading", "spreadingfactor"} and i + 1 < len(tokens):
            # Begins a block that monitors for exceptions.
            try:
                # Executes the statement `metadata["sf"] = int(tokens[i + 1])`.
                metadata["sf"] = int(tokens[i + 1])
                # Executes the statement `handled = True`.
                handled = True
                # Executes the statement `i += 2`.
                i += 2
            # Handles a specific exception from the try block.
            except ValueError:
                # Acts as a no-operation placeholder statement.
                pass
        # Handles an additional condition in the branching logic.
        elif token == "cr" and i + 1 < len(tokens):
            # Executes the statement `mapped = _map_cr(tokens[i + 1])`.
            mapped = _map_cr(tokens[i + 1])
            # Begins a conditional branch to check a condition.
            if mapped:
                # Executes the statement `metadata["cr"] = mapped`.
                metadata["cr"] = mapped
                # Executes the statement `handled = True`.
                handled = True
                # Executes the statement `i += 2`.
                i += 2
        # Handles an additional condition in the branching logic.
        elif token == "sps" and i + 1 < len(tokens):
            # Executes the statement `parsed = _parse_unit(tokens[i + 1])`.
            parsed = _parse_unit(tokens[i + 1])
            # Begins a conditional branch to check a condition.
            if parsed:
                # Executes the statement `metadata["samp_rate"] = parsed`.
                metadata["samp_rate"] = parsed
                # Executes the statement `handled = True`.
                handled = True
                # Executes the statement `i += 2`.
                i += 2
        # Handles an additional condition in the branching logic.
        elif token in {"fs", "samp", "samplerate", "sample", "samp_rate"} and i + 1 < len(tokens):
            # Executes the statement `parsed = _parse_unit(tokens[i + 1])`.
            parsed = _parse_unit(tokens[i + 1])
            # Begins a conditional branch to check a condition.
            if parsed:
                # Executes the statement `metadata["samp_rate"] = parsed`.
                metadata["samp_rate"] = parsed
                # Executes the statement `handled = True`.
                handled = True
                # Executes the statement `i += 2`.
                i += 2
        # Handles an additional condition in the branching logic.
        elif token == "ldro" and i + 1 < len(tokens):
            # Executes the statement `bool_val = _coerce_bool(tokens[i + 1], default=None)`.
            bool_val = _coerce_bool(tokens[i + 1], default=None)
            # Begins a conditional branch to check a condition.
            if bool_val is not None:
                # Executes the statement `metadata["ldro_mode"] = bool_val`.
                metadata["ldro_mode"] = bool_val
            # Provides the fallback branch when previous conditions fail.
            else:
                # Begins a block that monitors for exceptions.
                try:
                    # Executes the statement `metadata["ldro_mode"] = int(tokens[i + 1])`.
                    metadata["ldro_mode"] = int(tokens[i + 1])
                # Handles a specific exception from the try block.
                except ValueError:
                    # Acts as a no-operation placeholder statement.
                    pass
            # Executes the statement `handled = True`.
            handled = True
            # Executes the statement `i += 2`.
            i += 2
        # Handles an additional condition in the branching logic.
        elif token == "crc" and i + 1 < len(tokens):
            # Executes the statement `bool_val = _coerce_bool(tokens[i + 1], default=None)`.
            bool_val = _coerce_bool(tokens[i + 1], default=None)
            # Begins a conditional branch to check a condition.
            if bool_val is not None:
                # Executes the statement `metadata["crc"] = bool_val`.
                metadata["crc"] = bool_val
            # Executes the statement `handled = True`.
            handled = True
            # Executes the statement `i += 2`.
            i += 2
        # Handles an additional condition in the branching logic.
        elif token in {"implheader", "implicit", "implicitheader"} and i + 1 < len(tokens):
            # Executes the statement `bool_val = _coerce_bool(tokens[i + 1], default=None)`.
            bool_val = _coerce_bool(tokens[i + 1], default=None)
            # Begins a conditional branch to check a condition.
            if bool_val is not None:
                # Executes the statement `metadata["impl_header"] = bool_val`.
                metadata["impl_header"] = bool_val
            # Executes the statement `handled = True`.
            handled = True
            # Executes the statement `i += 2`.
            i += 2
        # Handles an additional condition in the branching logic.
        elif token in {"explicitheader", "explicit"} and i + 1 < len(tokens):
            # Executes the statement `bool_val = _coerce_bool(tokens[i + 1], default=None)`.
            bool_val = _coerce_bool(tokens[i + 1], default=None)
            # Begins a conditional branch to check a condition.
            if bool_val is not None:
                # Executes the statement `metadata["impl_header"] = not bool_val`.
                metadata["impl_header"] = not bool_val
            # Executes the statement `handled = True`.
            handled = True
            # Executes the statement `i += 2`.
            i += 2

        # Begins a conditional branch to check a condition.
        if handled:
            # Skips to the next iteration of the loop.
            continue

        # Inline tokens (no separator, e.g., `bw125k`, `sf7`, `sps250k`).
        # Begins a conditional branch to check a condition.
        if token.startswith("bw"):
            # Executes the statement `parsed = _parse_unit(token[2:])`.
            parsed = _parse_unit(token[2:])
            # Begins a conditional branch to check a condition.
            if parsed:
                # Executes the statement `metadata["bw"] = parsed`.
                metadata["bw"] = parsed
        # Handles an additional condition in the branching logic.
        elif token.startswith("sf"):
            # Begins a block that monitors for exceptions.
            try:
                # Executes the statement `metadata["sf"] = int(token[2:])`.
                metadata["sf"] = int(token[2:])
            # Handles a specific exception from the try block.
            except ValueError:
                # Acts as a no-operation placeholder statement.
                pass
        # Handles an additional condition in the branching logic.
        elif token.startswith("cr"):
            # Executes the statement `mapped = _map_cr(token[2:])`.
            mapped = _map_cr(token[2:])
            # Begins a conditional branch to check a condition.
            if mapped:
                # Executes the statement `metadata["cr"] = mapped`.
                metadata["cr"] = mapped
        # Handles an additional condition in the branching logic.
        elif token.startswith("sps"):
            # Executes the statement `parsed = _parse_unit(token[3:])`.
            parsed = _parse_unit(token[3:])
            # Begins a conditional branch to check a condition.
            if parsed:
                # Executes the statement `metadata["samp_rate"] = parsed`.
                metadata["samp_rate"] = parsed
        # Handles an additional condition in the branching logic.
        elif token.startswith("fs"):
            # Executes the statement `parsed = _parse_unit(token[2:])`.
            parsed = _parse_unit(token[2:])
            # Begins a conditional branch to check a condition.
            if parsed:
                # Executes the statement `metadata["samp_rate"] = parsed`.
                metadata["samp_rate"] = parsed
        # Handles an additional condition in the branching logic.
        elif token.startswith("ldro"):
            # Executes the statement `suffix = token[4:]`.
            suffix = token[4:]
            # Begins a conditional branch to check a condition.
            if suffix:
                # Executes the statement `bool_val = _coerce_bool(suffix, default=None)`.
                bool_val = _coerce_bool(suffix, default=None)
                # Begins a conditional branch to check a condition.
                if bool_val is not None:
                    # Executes the statement `metadata["ldro_mode"] = bool_val`.
                    metadata["ldro_mode"] = bool_val
                # Handles an additional condition in the branching logic.
                elif suffix.isdigit():
                    # Executes the statement `metadata["ldro_mode"] = int(suffix)`.
                    metadata["ldro_mode"] = int(suffix)
        # Handles an additional condition in the branching logic.
        elif token.startswith("crc"):
            # Executes the statement `bool_val = _coerce_bool(token[3:], default=None)`.
            bool_val = _coerce_bool(token[3:], default=None)
            # Begins a conditional branch to check a condition.
            if bool_val is not None:
                # Executes the statement `metadata["crc"] = bool_val`.
                metadata["crc"] = bool_val
        # Handles an additional condition in the branching logic.
        elif token.startswith("impl") or token.startswith("implicit"):
            # Executes the statement `bool_val = _coerce_bool(token.split("impl", 1)[-1], default=None)`.
            bool_val = _coerce_bool(token.split("impl", 1)[-1], default=None)
            # Begins a conditional branch to check a condition.
            if bool_val is not None:
                # Executes the statement `metadata["impl_header"] = bool_val`.
                metadata["impl_header"] = bool_val
        # Handles an additional condition in the branching logic.
        elif token.startswith("explicit"):
            # Executes the statement `suffix = token[len("explicit") :]`.
            suffix = token[len("explicit") :]
            # Executes the statement `bool_val = _coerce_bool(suffix, default=None)`.
            bool_val = _coerce_bool(suffix, default=None)
            # Begins a conditional branch to check a condition.
            if bool_val is not None:
                # Executes the statement `metadata["impl_header"] = not bool_val`.
                metadata["impl_header"] = not bool_val

        # Executes the statement `i += 1`.
        i += 1

    # Returns the computed value to the caller.
    return metadata


# Defines the function _ensure_required.
def _ensure_required(metadata: Dict[str, object]) -> bool:
    # Executes the statement `required = {"sf", "bw", "samp_rate"}`.
    required = {"sf", "bw", "samp_rate"}
    # Returns the computed value to the caller.
    return required.issubset(metadata.keys())


# Defines the function prepare_vector.
def prepare_vector(file_path: Path) -> Optional[ParsedVector]:
    # Executes the statement `metadata = _parse_metadata(file_path.stem)`.
    metadata = _parse_metadata(file_path.stem)

    # Begins a conditional branch to check a condition.
    if not _ensure_required(metadata):
        # Returns the computed value to the caller.
        return None

    # Executes the statement `metadata.setdefault("cr", 1)`.
    metadata.setdefault("cr", 1)
    # Executes the statement `metadata.setdefault("ldro_mode", False)`.
    metadata.setdefault("ldro_mode", False)
    # Executes the statement `metadata.setdefault("crc", True)`.
    metadata.setdefault("crc", True)
    # Executes the statement `metadata.setdefault("impl_header", False)`.
    metadata.setdefault("impl_header", False)

    # Executes the statement `ldro_value = metadata.get("ldro_mode")`.
    ldro_value = metadata.get("ldro_mode")
    # Begins a conditional branch to check a condition.
    if isinstance(ldro_value, bool):
        # Executes the statement `metadata["ldro_mode"] = 1 if ldro_value else 0`.
        metadata["ldro_mode"] = 1 if ldro_value else 0
    # Handles an additional condition in the branching logic.
    elif isinstance(ldro_value, str):
        # Executes the statement `coerced_bool = _coerce_bool(ldro_value, default=None)`.
        coerced_bool = _coerce_bool(ldro_value, default=None)
        # Begins a conditional branch to check a condition.
        if coerced_bool is not None:
            # Executes the statement `metadata["ldro_mode"] = 1 if coerced_bool else 0`.
            metadata["ldro_mode"] = 1 if coerced_bool else 0
        # Provides the fallback branch when previous conditions fail.
        else:
            # Begins a block that monitors for exceptions.
            try:
                # Executes the statement `metadata["ldro_mode"] = int(ldro_value)`.
                metadata["ldro_mode"] = int(ldro_value)
            # Handles a specific exception from the try block.
            except ValueError:
                # Executes the statement `metadata["ldro_mode"] = 0`.
                metadata["ldro_mode"] = 0
    # Handles an additional condition in the branching logic.
    elif isinstance(ldro_value, int):
        # Executes the statement `metadata["ldro_mode"] = ldro_value`.
        metadata["ldro_mode"] = ldro_value
    # Provides the fallback branch when previous conditions fail.
    else:
        # Executes the statement `metadata["ldro_mode"] = 0`.
        metadata["ldro_mode"] = 0

    # Executes the statement `cf32_path = file_path.with_suffix(".cf32")`.
    cf32_path = file_path.with_suffix(".cf32")
    # Executes the statement `metadata_path = cf32_path.with_suffix(".json")`.
    metadata_path = cf32_path.with_suffix(".json")

    # Begins a conditional branch to check a condition.
    if not cf32_path.exists():
        # Executes the statement `shutil.copy2(file_path, cf32_path)`.
        shutil.copy2(file_path, cf32_path)

    # Executes the statement `metadata["filename"] = cf32_path.name`.
    metadata["filename"] = cf32_path.name

    # Returns the computed value to the caller.
    return ParsedVector(
        # Executes the statement `source_path=file_path,`.
        source_path=file_path,
        # Executes the statement `cf32_path=cf32_path,`.
        cf32_path=cf32_path,
        # Executes the statement `metadata_path=metadata_path,`.
        metadata_path=metadata_path,
        # Executes the statement `metadata=metadata,`.
        metadata=metadata,
    # Closes the previously opened parenthesis grouping.
    )


# Defines the function write_metadata.
def write_metadata(parsed: ParsedVector) -> None:
    # Executes the statement `parsed.metadata_path.write_text(json.dumps(parsed.metadata, indent=2))`.
    parsed.metadata_path.write_text(json.dumps(parsed.metadata, indent=2))


# Defines the function main.
def main() -> None:
    # Begins a conditional branch to check a condition.
    if not VECTOR_DIR.exists():
        # Raises an exception to signal an error.
        raise SystemExit(f"Vector directory not found: {VECTOR_DIR}")

    # Executes the statement `prepared = []`.
    prepared = []
    # Executes the statement `skipped = []`.
    skipped = []

    # Starts a loop iterating over a sequence.
    for file_path in sorted(VECTOR_DIR.iterdir()):
        # Begins a conditional branch to check a condition.
        if not file_path.is_file():
            # Skips to the next iteration of the loop.
            continue
        # Begins a conditional branch to check a condition.
        if file_path.suffix.lower() not in SUPPORTED_SUFFIXES:
            # Skips to the next iteration of the loop.
            continue

        # Executes the statement `parsed = prepare_vector(file_path)`.
        parsed = prepare_vector(file_path)
        # Begins a conditional branch to check a condition.
        if parsed is None:
            # Executes the statement `skipped.append(file_path.name)`.
            skipped.append(file_path.name)
            # Skips to the next iteration of the loop.
            continue

        # Executes the statement `write_metadata(parsed)`.
        write_metadata(parsed)
        # Executes the statement `prepared.append(parsed)`.
        prepared.append(parsed)

    # Outputs diagnostic or user-facing text.
    print("Prepared vectors:")
    # Starts a loop iterating over a sequence.
    for pv in prepared:
        # Outputs diagnostic or user-facing text.
        print(f"  - {pv.source_path.name} -> {pv.cf32_path.name}, {pv.metadata_path.name}")

    # Begins a conditional branch to check a condition.
    if skipped:
        # Outputs diagnostic or user-facing text.
        print("\nSkipped (insufficient metadata):")
        # Starts a loop iterating over a sequence.
        for name in skipped:
            # Outputs diagnostic or user-facing text.
            print(f"  - {name}")

    # Begins a conditional branch to check a condition.
    if not prepared:
        # Outputs diagnostic or user-facing text.
        print("No vectors prepared.")


# Begins a conditional branch to check a condition.
if __name__ == "__main__":  # pragma: no cover - utility script
    # Executes the statement `main()`.
    main()

