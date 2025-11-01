from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Optional


RESERVED_KEYS = {
    "path",
    "group",
    "source",
    "notes",
    "clean_path",
    "air_path",
    "metadata",
}


@dataclass
class VectorEntry:
    path: Path
    metadata: Dict[str, Any]
    group: Optional[str] = None
    source: Optional[str] = None
    clean_path: Optional[Path] = None
    air_path: Optional[Path] = None
    notes: Optional[str] = None

    def to_dict(self) -> Dict[str, Any]:
        data = {
            "path": str(self.path),
            "metadata": self.metadata,
        }
        if self.group is not None:
            data["group"] = self.group
        if self.source is not None:
            data["source"] = self.source
        if self.clean_path is not None:
            data["clean_path"] = str(self.clean_path)
        if self.air_path is not None:
            data["air_path"] = str(self.air_path)
        if self.notes is not None:
            data["notes"] = self.notes
        return data


def _resolve_path(base: Path, value: Optional[str]) -> Optional[Path]:
    if not value:
        return None
    candidate = Path(value)
    if not candidate.is_absolute():
        candidate = (base / candidate).resolve()
    return candidate


def normalize_metadata(meta: Dict[str, Any]) -> Dict[str, Any]:
    norm = dict(meta)
    # Harmonise implicit header fields
    if "impl_header" in norm and "implicit_header" not in norm:
        norm["implicit_header"] = bool(norm["impl_header"])
    if "implicit_header" in norm and "impl_header" not in norm:
        norm["impl_header"] = bool(norm["implicit_header"])

    # Sample rate aliases
    if "samp_rate" in norm and "sample_rate" not in norm:
        norm["sample_rate"] = norm["samp_rate"]
    if "sample_rate" in norm and "samp_rate" not in norm:
        norm["samp_rate"] = norm["sample_rate"]
    if "sample_rate_hz" in norm and "sample_rate" not in norm:
        norm["sample_rate"] = norm["sample_rate_hz"]
        norm["samp_rate"] = norm["sample_rate_hz"]

    # Bandwidth aliases
    if "bandwidth_hz" in norm and "bw" not in norm:
        norm["bw"] = norm["bandwidth_hz"]

    # Coding rate aliases
    if "coding_rate" in norm and "cr" not in norm:
        norm["cr"] = norm["coding_rate"]

    # Payload length aliases
    if "payload_length" in norm and "payload_len" not in norm:
        norm["payload_len"] = norm["payload_length"]
    if "payload_len" in norm and "payload_length" not in norm:
        norm["payload_length"] = norm["payload_len"]

    # Normalise payload hex casing
    if "payload_hex" in norm and isinstance(norm["payload_hex"], str):
        norm["payload_hex"] = norm["payload_hex"].replace(" ", "").lower()

    int_fields = (
        "sf",
        "bw",
        "bandwidth_hz",
        "samp_rate",
        "sample_rate",
        "sample_rate_hz",
        "cr",
        "coding_rate",
        "payload_len",
        "payload_length",
        "ldro_mode",
    )
    for key in int_fields:
        if key in norm:
            try:
                norm[key] = int(norm[key])
            except (ValueError, TypeError):
                pass

    bool_fields = ("crc", "implicit_header", "impl_header", "ldro_enabled")
    for key in bool_fields:
        if key in norm:
            norm[key] = bool(norm[key])

    channel = norm.get("channel")
    if channel is not None and not isinstance(channel, dict):
        norm["channel"] = {}

    return norm


def _entry_from_item(item: Dict[str, Any], base_dir: Path) -> VectorEntry:
    if "path" not in item:
        raise ValueError("Manifest entry missing 'path'")
    path = _resolve_path(base_dir, item["path"])
    if path is None:
        raise ValueError("Manifest entry has null path")
    metadata = {k: v for k, v in item.items() if k not in RESERVED_KEYS}
    embedded_meta = item.get("metadata")
    if isinstance(embedded_meta, dict):
        metadata.update(embedded_meta)
    normalized = normalize_metadata(metadata)
    return VectorEntry(
        path=path,
        metadata=normalized,
        group=item.get("group"),
        source=item.get("source"),
        clean_path=_resolve_path(base_dir, item.get("clean_path")),
        air_path=_resolve_path(base_dir, item.get("air_path")),
        notes=item.get("notes"),
    )


def load_manifest(manifest_path: Path) -> list[VectorEntry]:
    manifest_path = manifest_path.expanduser().resolve()
    data = json.loads(manifest_path.read_text())
    if isinstance(data, dict) and "vectors" in data:
        items = data["vectors"]
    elif isinstance(data, list):
        items = data
    else:
        raise ValueError("Manifest must be a list or contain a 'vectors' list")

    base_dir = manifest_path.parent
    entries: list[VectorEntry] = []
    for item in items:
        if not isinstance(item, dict):
            raise ValueError("Manifest entries must be objects")
        entries.append(_entry_from_item(item, base_dir))
    return entries


def scan_vectors(root: Path, recursive: bool = False) -> list[VectorEntry]:
    root = root.expanduser().resolve()
    entries: list[VectorEntry] = []
    iterator = root.rglob("*.cf32") if recursive else root.glob("*.cf32")
    for cf32 in sorted(iterator):
        meta_path = cf32.with_suffix(".json")
        if not meta_path.exists():
            continue
        meta = json.loads(meta_path.read_text())
        normalized = normalize_metadata(meta)
        try:
            rel_parent = cf32.parent.relative_to(root)
            group = rel_parent.as_posix() if rel_parent != Path(".") else root.name
        except ValueError:
            group = cf32.parent.as_posix()
        entries.append(
            VectorEntry(
                path=cf32,
                metadata=normalized,
                group=group,
            )
        )
    return entries


def load_manifest_or_scan(
    manifest_path: Optional[Path],
    vectors_dir: Path,
    recursive: bool = False,
) -> list[VectorEntry]:
    if manifest_path:
        manifest_path = manifest_path.expanduser()
        if manifest_path.exists():
            return load_manifest(manifest_path)
    return scan_vectors(vectors_dir, recursive=recursive)
