# OTA Capture Catalogue Template

This folder captures metadata for field or lab recordings (IQ `.cf32` files, companion JSON metadata, logs). Populate one subdirectory per session.

## Directory Layout
```
docs/captures/
  session_YYYYMMDD_location/
    capture_manifest.json
    notes.md
    iq/
      example_capture.cf32
    logs/
      radio_config.txt
      soak_metrics.json
```

## Metadata Schema (`capture_manifest.json`)
| Field | Description |
| --- | --- |
| `session_id` | Unique identifier (string). |
| `date` | ISO date string. |
| `location` | City/site name. |
| `hardware` | Board/SR info (type, serial). |
| `antenna` | Antenna configuration. |
| `captures` | Array of capture entries (see below). |

### Capture Entry Fields
| Field | Description |
| --- | --- |
| `file` | Relative path to `.cf32`. |
| `metadata` | Relative path to `.json` (LoRa parameters). |
| `sf` | Spreading factor. |
| `bw_hz` | Bandwidth in Hz. |
| `cr` | Coding rate index. |
| `sample_rate_hz` | Sample rate used. |
| `notes` | Free-form observations (optional). |

## Example
```jsonc
{
  "session_id": "2024-07-15_tel-aviv",
  "date": "2024-07-15",
  "location": "Tel Aviv rooftop",
  "hardware": {
    "radio": "LimeSDR Mini",
    "serial": "123456",
    "fw": "v1.3"
  },
  "antenna": "Bi-quad 868MHz",
  "captures": [
    {
      "file": "iq/capture_sf7.cf32",
      "metadata": "iq/capture_sf7.json",
      "sf": 7,
      "bw_hz": 125000,
      "cr": 1,
      "sample_rate_hz": 500000,
      "notes": "Line-of-sight, 200m."
    }
  ]
}
```

## Workflow Checklist
1. Create `docs/captures/session_<date>_<site>/`.
2. Copy `.cf32` + metadata `.json`.
3. Fill in `capture_manifest.json`.
4. Add session notes (power levels, weather, etc.).
5. Commit alongside generated results from `tools/run_interop_compare.py` / `tools/summarise_validation.py`.
