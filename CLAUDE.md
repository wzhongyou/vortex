# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Vortex — a C++17 inverted index (full-text search) engine. MIT licensed.

## Build Commands

```bash
# Configure (from repo root)
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build -j$(sysctl -n hw.ncpu)

# Debug build with tests
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DVORTEX_BUILD_TESTS=ON
cmake --build build -j$(sysctl -n hw.ncpu)

# Run tests
cd build && ctest --output-on-failure

# Run a single test
cd build && ctest -R <test_name> --output-on-failure

# Run benchmarks
cd build && ./benchmarks/vortex_benchmarks
```

## Directory layout

```
include/vortex/core/     # Arena, Document, Query, Schema, Stats, Status, Types
include/vortex/inverted/ # Analyzer, FST Dict, PostingList, BM25F, Segment, WAL
src/                     # Implementation (mirrors include/)
tests/                   # GoogleTest unit tests
examples/                # Usage example
benchmarks/              # Google Benchmark micro-benchmarks
```

## Code conventions

- C++17, CMake 3.16+
- Header-only where feasible for template-heavy code; `.cpp` for non-template logic
- GoogleTest for tests, Google Benchmark for perf
- Custom binary format for on-disk serialization (no FlatBuffers)
- Namespace: `vortex::`
- Include guard style: `#pragma once`

## Architecture

See [README.md](README.md) for the four-layer architecture overview and design principles.
