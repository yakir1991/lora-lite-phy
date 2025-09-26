# Scripts Directory

This directory contains utility scripts and tools for the LoRa receiver system.

## Main Scripts

- **`lora_test_suite.py`** - Automated test suite for multi-configuration validation
- **`batch_lora_decoder.py`** - GR LoRa SDR compatible batch processor for multiple files
- **`final_system_demo.py`** - Complete system demonstration script
- **`celebration_demo.py`** - Success demonstration and results summary

## Usage Examples

```bash
# Run comprehensive test suite
python scripts/lora_test_suite.py --quick-test

# Batch process multiple files
python scripts/batch_lora_decoder.py vectors/ --output-dir results/

# System demonstration
python scripts/final_system_demo.py
```

All scripts are ready for production use and integrate with the main receiver system.
