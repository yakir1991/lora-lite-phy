"""
Automation harness utilities for LoRa Lite PHY tooling.

Currently exposes backend helpers that wrap the C++ receiver binaries and the
GNU Radio reference decoder so higher-level scripts can orchestrate comparisons
without duplicating subprocess and parsing logic.
"""

from .backends import (  # noqa: F401
    Backend,
    CompareOptions,
    DecodeResult,
    CppStreamingBackend,
    GnuRadioBatchBackend,
    collect_vector_pairs,
)
from .manifest import (  # noqa: F401
    VectorEntry,
    load_manifest,
    load_manifest_or_scan,
    normalize_metadata,
    scan_vectors,
)

__all__ = [
    "Backend",
    "CompareOptions",
    "DecodeResult",
    "CppStreamingBackend",
    "GnuRadioBatchBackend",
    "collect_vector_pairs",
    "VectorEntry",
    "normalize_metadata",
    "scan_vectors",
    "load_manifest",
    "load_manifest_or_scan",
]
