# LoRa Lite PHY - Project Structure

## 📁 Directory Organization

```
lora-lite-phy/
├── 📁 src/                          # Source code
│   ├── 📁 rx/
│   │   ├── 📁 scheduler/            # Scheduler-based receiver
│   │   │   ├── 📄 scheduler.hpp     # Scheduler header
│   │   │   └── 📄 scheduler.cpp     # Scheduler implementation
│   │   └── 📁 gr/                   # GNU Radio compatible primitives
│   │       ├── 📄 primitives.cpp    # Core primitives
│   │       ├── 📄 header_decode.cpp # Header decoding
│   │       └── 📄 utils.cpp         # Utility functions
│   └── 📄 workspace.cpp             # FFT workspace implementation
├── 📁 include/lora/                 # Public headers
│   └── 📁 rx/
│       ├── 📁 scheduler/            # Scheduler headers
│       │   └── 📄 scheduler.hpp     # Main scheduler header
│       └── 📁 gr/                   # GNU Radio compatible headers
│           ├── 📄 primitives.hpp    # Primitives header
│           ├── 📄 header_decode.hpp # Header decode header
│           └── 📄 utils.hpp         # Utils header
├── 📁 tests/                        # Test executables
│   ├── 📄 test_gr_pipeline.cpp      # Main CLI/test executable
│   └── 📄 test_scheduler.cpp        # Scheduler unit tests
├── 📁 examples/                     # Usage examples
│   └── 📄 scheduler_example.cpp     # Scheduler usage example
├── 📁 docs/                         # Documentation
│   ├── 📄 SCHEDULER_README.md       # Scheduler documentation
│   ├── 📄 PERFORMANCE_ANALYSIS.md   # Performance analysis
│   ├── 📄 offline_decode_investigation.md # Debug notes
│   └── 📄 LoRa_Lite_Migration_Plan_README.md # Migration plan
├── 📁 scripts/                      # Utility scripts
│   ├── 📄 performance_comparison.py # Performance comparison
│   ├── 📄 simple_performance_test.py # Simple performance test
│   ├── 📄 create_performance_chart.py # Chart generation
│   ├── 📄 create_corrected_chart.py # Corrected chart generation
│   └── 📄 decode_offline_recording_final.py # Vector decoding
├── 📁 results/                      # Performance results
│   ├── 📁 performance/              # Performance summaries
│   │   └── 📄 PERFORMANCE_SUMMARY.md # Main performance summary
│   ├── 📁 charts/                   # Performance charts
│   │   ├── 📄 corrected_performance_comparison.png
│   │   ├── 📄 corrected_performance_comparison.pdf
│   │   ├── 📄 detailed_performance_analysis.png
│   │   └── 📄 efficiency_comparison.png
│   └── 📁 data/                     # Raw performance data
│       ├── 📄 individual_performance_results.json
│       └── 📄 performance_results.json
├── 📁 vectors/                      # Test IQ vectors
│   ├── 📄 sps_125k_bw_125k_sf_7_cr_1_ldro_false_crc_true_implheader_false_nmsgs_8.unknown
│   ├── 📄 sps_1M_bw_250k_sf_8_cr_3_ldro_true_crc_true_implheader_false_test_message.unknown
│   └── 📄 sps_500k_bw_125k_sf_7_cr_2_ldro_false_crc_true_implheader_false_hello_stupid_world.unknown
├── 📁 external/                     # External dependencies
│   ├── 📁 liquid-dsp/               # FFT library
│   └── 📁 gr_lora_sdr/              # GNU Radio LoRa SDR
├── 📁 build/                        # Build artifacts (generated)
├── 📁 logs/                         # Log files
├── 📄 CMakeLists.txt                # CMake configuration
├── 📄 README.md                     # Main README
└── 📄 PROJECT_STRUCTURE.md          # This file
```

## 🎯 Key Components

### Core Scheduler (`src/rx/scheduler/`)
- **`scheduler.hpp`** - Main scheduler class and data structures
- **`scheduler.cpp`** - Scheduler implementation with state machine

### GNU Radio Primitives (`src/rx/gr/`)
- **`primitives.cpp`** - Core signal processing primitives
- **`header_decode.cpp`** - Header decoding logic
- **`utils.cpp`** - Utility functions and helpers

### Tests (`tests/`)
- **`test_gr_pipeline.cpp`** - Main CLI/test executable
- **`test_scheduler.cpp`** - Scheduler unit tests

### Examples (`examples/`)
- **`scheduler_example.cpp`** - Usage example

### Documentation (`docs/`)
- **`SCHEDULER_README.md`** - Detailed scheduler documentation
- **`PERFORMANCE_ANALYSIS.md`** - Performance analysis
- **`offline_decode_investigation.md`** - Debug investigation notes

### Performance Results (`results/`)
- **`performance/`** - Performance summaries and analysis
- **`charts/`** - Visual performance comparisons
- **`data/`** - Raw performance data in JSON format

### Scripts (`scripts/`)
- **`performance_comparison.py`** - Performance comparison script
- **`simple_performance_test.py`** - Simple performance testing
- **`create_corrected_chart.py`** - Chart generation
- **`decode_offline_recording_final.py`** - Vector decoding utility

## 🏗️ Build System

### CMake Configuration
- **`CMakeLists.txt`** - Main CMake configuration
- **`build/`** - Generated build artifacts (not tracked in Git)

### Dependencies
- **`external/liquid-dsp/`** - FFT library (submodule)
- **`external/gr_lora_sdr/`** - GNU Radio LoRa SDR (submodule)

## 📊 Performance Data

### Raw Data (`results/data/`)
- **`individual_performance_results.json`** - Individual test results
- **`performance_results.json`** - Aggregated performance data

### Charts (`results/charts/`)
- **`corrected_performance_comparison.png/pdf`** - Main comparison charts
- **`detailed_performance_analysis.png`** - Detailed analysis
- **`efficiency_comparison.png`** - Efficiency comparison

### Summaries (`results/performance/`)
- **`PERFORMANCE_SUMMARY.md`** - Main performance summary

## 🧪 Test Vectors

### Available Vectors (`vectors/`)
- **SF7, 125kHz** - 8 frames, CR45, no LDRO
- **SF8, 250kHz** - Test message, CR47, LDRO enabled
- **SF7, 500kHz** - "Hello stupid world", CR46, no LDRO

## 🔧 Development Workflow

### Adding New Features
1. **Scheduler**: Modify `src/rx/scheduler/`
2. **Primitives**: Add to `src/rx/gr/`
3. **Tests**: Add to `tests/`
4. **Documentation**: Update `docs/`

### Performance Testing
1. **Run tests**: `python3 scripts/simple_performance_test.py`
2. **Generate charts**: `python3 scripts/create_corrected_chart.py`
3. **View results**: Check `results/` directory

### Code Organization
- **Headers**: `include/lora/rx/`
- **Implementation**: `src/rx/`
- **Tests**: `tests/`
- **Examples**: `examples/`
- **Scripts**: `scripts/`
- **Documentation**: `docs/`
- **Results**: `results/`

## 📈 Performance Metrics

### Key Performance Indicators
- **Sample Processing**: 4.17 MSamples/sec (average)
- **Frame Processing**: 320.8 frames/sec (average)
- **Success Rate**: 100% on all test vectors
- **Improvement**: 8.3x faster than GNU Radio

### Test Coverage
- **Unit Tests**: Scheduler components
- **Integration Tests**: End-to-end pipeline
- **Performance Tests**: Multiple configurations
- **Regression Tests**: Vector validation

---

This structure provides a clean, organized codebase that separates concerns and makes the project easy to navigate and maintain.