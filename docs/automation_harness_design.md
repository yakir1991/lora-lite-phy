# Automation Harness Design

This document sketches the extensions required to turn the existing comparison scripts into a unified harness that exercises both receivers (C++ and GNU Radio) in batch and streaming modes, captures performance telemetry, and emits machine-readable reports.

## Current State (2025-10-21)

- `tools/compare_streaming_compat.py`  
  - Runs GNU Radio offline decoder (`decode_offline_recording.py`) and our C++ `decode_cli --streaming` sequentially for each vector.  
  - Compares payload hex strings and writes a JSON report with per-vector outcomes.  
  - No execution-time metrics; collects only stdout/stderr.  
  - Batch-only (file input); no socket/real-time mode.

- `tools/run_channel_regressions.py`  
  - Generates synthetic vectors via GNU Radio exporter (`--channel-sim`) into paired clean/air directories.  
  - Calls `compare_streaming_compat` as a sub-process to evaluate streaming mode.  
  - Tracks only decode success; performance data and GNU Radio payload extraction handled externally.

These scripts already provide the command assembly logic we need. The design below focuses on abstracting backend execution, normalising results, and extending coverage to batch + streaming runs.

## Goals

1. **Dual backend abstraction**: treat “our receiver” and “GNU Radio reference” as implementations conforming to a common interface (input vector → decode result, instrumentation payload).  
2. **Batch + streaming**: support both `decode_cli` (one-shot) and `decode_cli --streaming`, as well as GNU Radio’s offline decoder and streaming flowgraph (Python script or GRC-generated harness).  
3. **Performance metrics**: capture wall-clock time, CPU usage, chunk latency stats, retry counts, sample-rate ratio, CFO estimates, etc. Output structured logs (JSON/CSV).  
4. **Config sweep orchestration**: feed vectors from manifest (see `docs/test_vector_corpus.md`), iterate over configuration matrix, and store side-by-side comparison results.  
5. **Extensibility**: make it easy to plug in additional measurement probes (e.g., perf, `/usr/bin/time -v`, streaming harness CSV dumps).

## Proposed Architecture

```
tools/
  harness/
    __init__.py
    backends.py         # Backend abstractions & registry
    runners.py          # BatchRunner / StreamingRunner orchestrators
    metrics.py          # Helpers to parse runtime metrics
    manifest.py         # Loader for vector manifest (see docs/test_vector_corpus.md)
    reporting.py        # JSON/CSV emitters, diff summaries
  compare_receivers.py  # CLI entry point replacing/augmenting compare_streaming_compat.py
```

### Backend Interface

```python
class DecodeResult(TypedDict):
    status: Literal["success", "failed", "timeout", "skipped"]
    payload_hex: Optional[str]
    frame_count: Optional[int]
    crc_ok: Optional[bool]
    sample_rate_ratio: Optional[float]
    cfo_hz: Optional[float]
    retries: Dict[str, int]        # e.g., {"sr_scan": 1, "payload_retry": 2}
    timings_us: Dict[str, float]   # e.g., {"sync": 3400.0, "header": 1200.0}
    stdout: str
    stderr: str
    metadata: Dict[str, Any]       # Passthrough of backend-specific details

class Backend(Protocol):
    name: str
    mode: Literal["batch", "streaming"]

    def prepare(self) -> None: ...
    def decode(self, vector: VectorEntry, options: HarnessOptions) -> DecodeResult: ...
    def cleanup(self) -> None: ...
```

- **C++ Batch**: wraps `decode_cli` without `--streaming`, uses JSON sidecar to configure implicit header, LDRO, etc.  
- **C++ Streaming**: wraps `decode_cli --streaming`, optionally launches `streaming_harness` for socket mode. Parses stdout for `payload_hex=…`, instrumentation lines (`sr_scan=`, `cfo_sweep=`, `chunk_us=` once logging enhancements land).  
- **GNU Radio Batch**: wraps `decode_offline_recording.py`, capturing payload hex, frame count, and optionally extends the script to emit JSON summary (consider `--dump-json`).  
- **GNU Radio Streaming**: uses `external/gr_lora_sdr/examples/tx_rx_simulation.py` (or a minimal Python bridge) to stream IQ to TCP; our harness connects via socket. Alternatively, rely on GNU Radio offline script for parity and treat streaming path as future enhancement.

Backends register via a simple dictionary:

```python
BACKENDS = {
    "cpp_batch": CppBatchBackend(...),
    "cpp_stream": CppStreamingBackend(...),
    "gr_batch": GnuRadioBatchBackend(...),
    "gr_stream": GnuRadioStreamingBackend(...),
}
```

### Runner Layer

- `BatchRunner`: iterates over manifest entries, runs each requested backend, compares payloads/diagnostics, records timings (using `monotonic_ns` around `backend.decode`).  
- `StreamingRunner`: similar, but supports chunk-size sweeps, throttle options, and socket vs. file feed. Coordinates streaming harness for our receiver and GNU Radio streaming script (start subprocesses, manage lifecycle).

Runner output structure:

```json
{
  "vector": "vectors/synthetic_batch/tx_sf8_....cf32",
  "phy": {...},
  "backends": {
    "cpp_stream": { ... DecodeResult ... },
    "gr_batch": { ... },
    "gr_stream": { ... }
  },
  "comparisons": {
    "payload_match": true,
    "frame_count_diff": 0,
    "sr_ratio_diff": 0.0003,
    "cfo_diff_hz": 5.2
  },
  "timing": {
    "wall_clock_s": {
      "cpp_stream": 0.052,
      "gr_batch": 0.134
    }
  }
}
```

### Metrics Capture

1. **Wall-clock & CPU**: wrap backend invocation with `time.perf_counter()` and optional `/usr/bin/time -v` (when available). Provide CLI flag `--measure=perf` to enable.  
2. **Chunk timing**: extend `StreamingReceiver` and `streaming_harness` to emit CSV/JSON (see master plan §6). Harness will parse those files and attach stats (avg, p99).  
3. **Resource usage**: optionally integrate `psutil` or Linux `/proc` sampling for RSS/CPU%.  
4. **Retries/diagnostics**: rely on instrumentation additions to C++ code (counts already present in `DecodeResult`) and extend GNU Radio scripts to log equivalent metrics (e.g., CFO estimate, header retries).

### CLI Sketch

```bash
python -m tools.compare_receivers \
  --manifest results/vector_manifest.json \
  --backends cpp_stream,gr_batch \
  --modes batch,streaming \
  --limit 50 \
  --measure perf \
  --streaming-throttle-us 0,1000,5000 \
  --output results/comparison_matrix.json \
  --report-html results/comparison_report.html
```

Options:
- `--manifest`: required path to manifest (see `docs/test_vector_corpus.md`).  
- `--vectors`: fallback to single directory scan if manifest missing.  
- `--backends`: comma-separated list (defaults to `cpp_stream,gr_batch`).  
- `--streaming-throttle-us`: list of throttle settings to test in streaming runner.  
- `--socket-mode`: toggle to pipe vectors through `streaming_harness` socket emulator instead of direct file streaming.  
- `--measure`: choose `none`, `perf`, `time`.  
- `--dump-artifacts`: directory for logs, header slices, chunk timing CSVs.  
- `--only-failures`: rerun or summarise only failing cases (links to existing tools like `dump_header_slices_for_failures.py`).

### Logging & Reporting

- Store raw run data as JSON (`results/comparison_matrix.json`).  
- Provide helper to generate Markdown/HTML summary tables (for human review).  
- Integrate with existing scripts (`summarize_batch_results.py`, `summarize_compat_results.py`) by aligning schema or adding adapters.

## Required Enhancements

1. **C++ receiver logging**: emit structured metrics (JSON or key=value) from streaming decode. Extend `StreamingReceiver` to track chunk timings, CFO sweeps, retries, sample-rate ratios.  
2. **GNU Radio instrumentation**: modify `decode_offline_recording.py` (or wrap) to emit JSON summarising CFO estimate, frame counters, processing time. Optionally extend flowgraphs with lightweight logging blocks.  
3. **Manifest builder**: script to scan vectors and produce canonical manifest as per `docs/test_vector_corpus.md`.  
4. **Socket harness parity**: create a consistent interface (our `streaming_harness` ↔ GNU Radio TCP sink/source) so both stacks can be exercised in real-time mode.  
5. **Config sweeps**: encode test matrix (SF/BW/CR/header/LDRO) in manifest or separate JSON; harness iterates per row and records which vectors satisfy each combination.

## Near-Term Implementation Plan

1. Extract backend invocations from `compare_streaming_compat.py` into reusable classes (`CppStreamingBackend`, `GnuRadioBatchBackend`).  
2. Add `tools/harness/manifest.py` to normalise vector metadata (loading sidecar JSON → canonical fields).  
3. Implement `compare_receivers.py` CLI supporting batch mode parity using new harness; maintain compatibility by mapping old flags.  
4. Extend C++ `decode_cli` to emit JSON summary line (e.g., `result_json=...`) containing instrumentation fields.  
5. Introduce optional `/usr/bin/time -v` measurement wrapper in backend implementation when `--measure perf` is enabled.  
6. Plan streaming/socket mode integration once CSV logging exists.

This structure should let us scale from simple payload comparisons to full performance benchmarking without rewriting the underlying scripts for each new scenario.
