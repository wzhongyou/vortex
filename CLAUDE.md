# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Vortex — a C++17 multi-recall engine supporting inverted index, vector index, and pluggable index backends, with result fusion. MIT licensed.

## Build Commands

```bash
# Configure (from repo root)
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build -j$(nproc)

# Debug build with tests
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DVORTEX_BUILD_TESTS=ON
cmake --build build -j$(nproc)

# Run tests
cd build && ctest --output-on-failure

# Run a single test
cd build && ctest -R <test_name> --output-on-failure

# Run benchmarks
cd build && ./benchmarks/vortex_benchmarks
```

## Architecture

Four-layer design:

```
┌──────────────────────────────────────┐
│  RecallEngine (orchestration+fusion) │
├──────────────────────────────────────┤
│  InvertedIndex  │  VectorIndex | ... │  ← Index implementations
├──────────────────────────────────────┤
│         Index (abstract interface)   │
├──────────────────────────────────────┤
│  Analyzer │ DocStore │ Serializer   │  ← Shared infra
└──────────────────────────────────────┘
```

### Key abstractions

- **`Index`** (`include/vortex/core/index.h`) — virtual base: `build()`, `search(query) → Results`, `insert(doc)`, `remove(id)`, `serialize()/deserialize()`
- **`InvertedIndex`** — tokenizes text fields, builds posting lists, scores via BM25/TF-IDF
- **`VectorIndex`** — stores embeddings, builds HNSW graph, supports L2/IP/Cosine distance
- **`RecallEngine`** — holds multiple `Index` instances, fans out queries concurrently, merges ranked lists (RRF or weighted fusion)
- **`DocStore`** — raw document storage with forward-field access

### Directory layout

```
include/vortex/core/     # Index, Document, Result, Field
include/vortex/inverted/ # InvertedIndex, Analyzer, PostingList, Scorer
include/vortex/vector/   # VectorIndex, HNSW, Distance
include/vortex/engine/   # RecallEngine, Fusion
include/vortex/store/    # DocStore
src/                     # Implementation (mirrors include/)
tests/                   # GoogleTest unit tests
benchmarks/              # Google Benchmark micro-benchmarks
```

## Code conventions

- C++17, CMake 3.16+
- Header-only where feasible for template-heavy code; `.cpp` for non-template logic
- GoogleTest for tests, Google Benchmark for perf
- FlatBuffers for on-disk serialization
- Namespace: `vortex::`
- Include guard style: `#pragma once`
