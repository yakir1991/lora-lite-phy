# Contributing to lora-lite-phy

Thank you for considering contributing! This document explains how to get
started, the development workflow, and the conventions we follow.

## Getting Started

```bash
git clone https://github.com/<you>/lora-lite-phy.git
cd lora-lite-phy
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build -j$(nproc)
```

All tests must pass before submitting a pull request (`ctest` exit code 0).

## Development Workflow

1. **Fork** the repository and create a feature branch from `master`.
2. Make your changes — keep commits small and focused.
3. Run the full test suite: `ctest --test-dir build -j$(nproc) --output-on-failure`
4. Push your branch and open a **pull request** against `master`.

CI runs GCC-13 and Clang-17 builds automatically on every PR.

## Commit Messages

Follow [Conventional Commits](https://www.conventionalcommits.org/):

```
feat: add Q15 soft-decision decoder
fix: correct CFO fractional-bin sign for negative offsets
test: add SF12 BW500 impairment sweep
docs: update OTA interop results table
refactor: extract burst detection into standalone function
```

## Code Style

- **Language:** C++20
- **Headers:** `#pragma once`, include-what-you-use
- **Braces:** Allman style (opening brace on its own line)
- **Indentation:** 4 spaces (no tabs)
- **Naming:** `snake_case` for functions/variables, `PascalCase` for types
- **Namespaces:** `host_sim` for library code, anonymous namespace for file-local helpers

A `.clang-format` file is provided — run `clang-format -i` on changed files,
or use your editor's format-on-save.

## Adding Tests

- TX→RX roundtrip tests go in `host_sim/CMakeLists.txt` under the `tx` label.
- OTA golden-file tests use label `ota`.
- Impairment sweep tests use labels `tx` and `impairment`.
- Unit tests are standalone executables in `host_sim/tests/`.

Every new feature should include at least one test.

## Reporting Issues

- Include the SF, BW, CR, and payload (or attach the `.cf32` + `.json`).
- If the issue is a decode failure, add `--verbose` output.
- For build issues, include your compiler version and OS.

## License

By contributing, you agree that your contributions will be licensed under the
same terms as the project (see [LICENSE](LICENSE)).
