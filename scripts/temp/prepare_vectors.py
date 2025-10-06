#!/usr/bin/env python3

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

from __future__ import annotations

import json
import re
import shutil
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Optional, Tuple


VECTOR_DIR = Path("/home/aquay/lora-lite-phy/vectors")
SUPPORTED_SUFFIXES = {".unknown", ".bin"}


UNIT_MULTIPLIERS = {
    "": 1,
    "k": 1_000,
    "khz": 1_000,
    "m": 1_000_000,
    "mhz": 1_000_000,
}


TRUE_TOKENS = {"true", "t", "1", "yes", "on"}
FALSE_TOKENS = {"false", "f", "0", "no", "off"}


@dataclass
class ParsedVector:
    source_path: Path
    cf32_path: Path
    metadata_path: Path
    metadata: Dict[str, object]


def _split_tokens(stem: str) -> Tuple[str, ...]:
    """Split the filename stem into lower-case tokens."""

    tokens = tuple(token for token in re.split(r"[_\-]+", stem) if token)
    return tokens


def _parse_unit(value: str) -> Optional[int]:
    value = value.strip().lower()
    if not value:
        return None

    for suffix, multiplier in UNIT_MULTIPLIERS.items():
        if suffix and value.endswith(suffix):
            number_part = value[:-len(suffix)]
            break
    else:
        number_part = value
        multiplier = 1

    if not number_part:
        return None

    try:
        numeric = float(number_part)
    except ValueError:
        return None

    return int(numeric * multiplier)


def _coerce_bool(token: str, default: Optional[bool] = None) -> Optional[bool]:
    lower = token.lower()
    if lower in TRUE_TOKENS:
        return True
    if lower in FALSE_TOKENS:
        return False
    return default


def _map_cr(value: str) -> Optional[int]:
    value = value.strip().lower()
    if not value:
        return None

    if value.isdigit():
        numeric = int(value)
        if 1 <= numeric <= 4:
            return numeric
        if 45 <= numeric <= 48:
            return numeric - 44
        if 55 <= numeric <= 58:
            return numeric - 54
    if value.startswith("4/") and value[2:].isdigit():
        denominator = int(value[2:])
        if 5 <= denominator <= 8:
            return denominator - 4
    return None


def _parse_metadata(stem: str) -> Dict[str, object]:
    tokens = _split_tokens(stem)
    metadata: Dict[str, object] = {}

    i = 0
    while i < len(tokens):
        token = tokens[i].lower()

        # Key-value pairs separated by delimiters (e.g., `bw`, `125k`).
        handled = False
        if token in {"bw", "bandwidth"} and i + 1 < len(tokens):
            parsed = _parse_unit(tokens[i + 1])
            if parsed:
                metadata["bw"] = parsed
                handled = True
                i += 2
        elif token in {"sf", "spreading", "spreadingfactor"} and i + 1 < len(tokens):
            try:
                metadata["sf"] = int(tokens[i + 1])
                handled = True
                i += 2
            except ValueError:
                pass
        elif token == "cr" and i + 1 < len(tokens):
            mapped = _map_cr(tokens[i + 1])
            if mapped:
                metadata["cr"] = mapped
                handled = True
                i += 2
        elif token == "sps" and i + 1 < len(tokens):
            parsed = _parse_unit(tokens[i + 1])
            if parsed:
                metadata["samp_rate"] = parsed
                handled = True
                i += 2
        elif token in {"fs", "samp", "samplerate", "sample", "samp_rate"} and i + 1 < len(tokens):
            parsed = _parse_unit(tokens[i + 1])
            if parsed:
                metadata["samp_rate"] = parsed
                handled = True
                i += 2
        elif token == "ldro" and i + 1 < len(tokens):
            bool_val = _coerce_bool(tokens[i + 1], default=None)
            if bool_val is not None:
                metadata["ldro_mode"] = bool_val
            else:
                try:
                    metadata["ldro_mode"] = int(tokens[i + 1])
                except ValueError:
                    pass
            handled = True
            i += 2
        elif token == "crc" and i + 1 < len(tokens):
            bool_val = _coerce_bool(tokens[i + 1], default=None)
            if bool_val is not None:
                metadata["crc"] = bool_val
            handled = True
            i += 2
        elif token in {"implheader", "implicit", "implicitheader"} and i + 1 < len(tokens):
            bool_val = _coerce_bool(tokens[i + 1], default=None)
            if bool_val is not None:
                metadata["impl_header"] = bool_val
            handled = True
            i += 2
        elif token in {"explicitheader", "explicit"} and i + 1 < len(tokens):
            bool_val = _coerce_bool(tokens[i + 1], default=None)
            if bool_val is not None:
                metadata["impl_header"] = not bool_val
            handled = True
            i += 2

        if handled:
            continue

        # Inline tokens (no separator, e.g., `bw125k`, `sf7`, `sps250k`).
        if token.startswith("bw"):
            parsed = _parse_unit(token[2:])
            if parsed:
                metadata["bw"] = parsed
        elif token.startswith("sf"):
            try:
                metadata["sf"] = int(token[2:])
            except ValueError:
                pass
        elif token.startswith("cr"):
            mapped = _map_cr(token[2:])
            if mapped:
                metadata["cr"] = mapped
        elif token.startswith("sps"):
            parsed = _parse_unit(token[3:])
            if parsed:
                metadata["samp_rate"] = parsed
        elif token.startswith("fs"):
            parsed = _parse_unit(token[2:])
            if parsed:
                metadata["samp_rate"] = parsed
        elif token.startswith("ldro"):
            suffix = token[4:]
            if suffix:
                bool_val = _coerce_bool(suffix, default=None)
                if bool_val is not None:
                    metadata["ldro_mode"] = bool_val
                elif suffix.isdigit():
                    metadata["ldro_mode"] = int(suffix)
        elif token.startswith("crc"):
            bool_val = _coerce_bool(token[3:], default=None)
            if bool_val is not None:
                metadata["crc"] = bool_val
        elif token.startswith("impl") or token.startswith("implicit"):
            bool_val = _coerce_bool(token.split("impl", 1)[-1], default=None)
            if bool_val is not None:
                metadata["impl_header"] = bool_val
        elif token.startswith("explicit"):
            suffix = token[len("explicit") :]
            bool_val = _coerce_bool(suffix, default=None)
            if bool_val is not None:
                metadata["impl_header"] = not bool_val

        i += 1

    return metadata


def _ensure_required(metadata: Dict[str, object]) -> bool:
    required = {"sf", "bw", "samp_rate"}
    return required.issubset(metadata.keys())


def prepare_vector(file_path: Path) -> Optional[ParsedVector]:
    metadata = _parse_metadata(file_path.stem)

    if not _ensure_required(metadata):
        return None

    metadata.setdefault("cr", 1)
    metadata.setdefault("ldro_mode", False)
    metadata.setdefault("crc", True)
    metadata.setdefault("impl_header", False)

    ldro_value = metadata.get("ldro_mode")
    if isinstance(ldro_value, bool):
        metadata["ldro_mode"] = 1 if ldro_value else 0
    elif isinstance(ldro_value, str):
        coerced_bool = _coerce_bool(ldro_value, default=None)
        if coerced_bool is not None:
            metadata["ldro_mode"] = 1 if coerced_bool else 0
        else:
            try:
                metadata["ldro_mode"] = int(ldro_value)
            except ValueError:
                metadata["ldro_mode"] = 0
    elif isinstance(ldro_value, int):
        metadata["ldro_mode"] = ldro_value
    else:
        metadata["ldro_mode"] = 0

    cf32_path = file_path.with_suffix(".cf32")
    metadata_path = cf32_path.with_suffix(".json")

    if not cf32_path.exists():
        shutil.copy2(file_path, cf32_path)

    metadata["filename"] = cf32_path.name

    return ParsedVector(
        source_path=file_path,
        cf32_path=cf32_path,
        metadata_path=metadata_path,
        metadata=metadata,
    )


def write_metadata(parsed: ParsedVector) -> None:
    parsed.metadata_path.write_text(json.dumps(parsed.metadata, indent=2))


def main() -> None:
    if not VECTOR_DIR.exists():
        raise SystemExit(f"Vector directory not found: {VECTOR_DIR}")

    prepared = []
    skipped = []

    for file_path in sorted(VECTOR_DIR.iterdir()):
        if not file_path.is_file():
            continue
        if file_path.suffix.lower() not in SUPPORTED_SUFFIXES:
            continue

        parsed = prepare_vector(file_path)
        if parsed is None:
            skipped.append(file_path.name)
            continue

        write_metadata(parsed)
        prepared.append(parsed)

    print("Prepared vectors:")
    for pv in prepared:
        print(f"  - {pv.source_path.name} -> {pv.cf32_path.name}, {pv.metadata_path.name}")

    if skipped:
        print("\nSkipped (insufficient metadata):")
        for name in skipped:
            print(f"  - {name}")

    if not prepared:
        print("No vectors prepared.")


if __name__ == "__main__":  # pragma: no cover - utility script
    main()

