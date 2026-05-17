# Vortex — Multi-Recall Engine

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
![C++17](https://img.shields.io/badge/C++-17-blue.svg)
[![CMake](https://img.shields.io/badge/CMake-3.16%2B-blue.svg)](CMakeLists.txt)

Vortex is a high-performance multi-strategy recall engine written in modern C++17.
It supports pluggable index backends — inverted index for keyword search and vector index for semantic search — and fuses multi-path results into a single ranked list.

Vortex is designed as a **lightweight embeddable library**: link it directly into your application with no separate server process.

## Architecture

```
┌──────────────────────────────────────┐
│  RecallEngine (orchestration+fusion) │  ← fan-out multi-index queries + fuse
├──────────────────────────────────────┤
│  InvertedIndex  │  VectorIndex | ... │  ← pluggable index implementations
├──────────────────────────────────────┤
│         Index (abstract interface)   │
├──────────────────────────────────────┤
│  Analyzer │ DocStore │ Serializer   │  ← shared infrastructure
└──────────────────────────────────────┘
```

| Layer | Responsibility |
|-------|---------------|
| **RecallEngine** | Holds multiple `Index` instances, dispatches queries concurrently, merges results via RRF / weighted fusion |
| **Index implementations** | `InvertedIndex` (full-text), `VectorIndex` (semantic), extensible via the `Index` interface |
| **Index abstract interface** | `build()`, `search()`, `insert()`, `remove()`, `serialize()` / `deserialize()` |
| **Shared infrastructure** | Analyzer (tokenization), DocStore (forward index), Serializer (FlatBuffers persistence) |

## Quick Start

```bash
# Prerequisites: GCC 13+ / Clang 18+, CMake 3.16+

# Debug build with tests
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DVORTEX_BUILD_TESTS=ON

# Build
cmake --build build -j$(nproc)

# Run all tests
cd build && ctest --output-on-failure

# Run a single test
cd build && ctest -R BuildIndexAndSearchTerm --output-on-failure

# Release build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Run benchmarks
cd build && ./benchmarks/vortex_benchmarks
```

## Testing

### Unit / Integration Tests

```bash
# Debug build + tests
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DVORTEX_BUILD_TESTS=ON
cmake --build build -j$(nproc)

# Option 1: via ctest
cd build && ctest --output-on-failure

# Option 2: run the test binary directly
cd build && ./tests/test_integration

# Option 3: run a specific test case
cd build && ctest -R FlushAndSearchAcrossSegments --output-on-failure
```

### Standalone Debug Programs

Quickly compile and run isolated tests to verify specific modules during development:

```bash
# Verify FST construction and lookup
c++ -std=c++17 -I include -I build/_deps/roaring-src/include \
  /tmp/vortex_fst_debug.cpp \
  -L build -lvortex -L build/_deps/roaring-build/src -lroaring \
  -o /tmp/fst_test && /tmp/fst_test

# Verify full pipeline (write → flush → search → score)
c++ -std=c++17 -I include -I build/_deps/roaring-src/include \
  /tmp/vortex_pipeline_test.cpp \
  -L build -lvortex -L build/_deps/roaring-build/src -lroaring \
  -o /tmp/pipe_test && /tmp/pipe_test
```

### Sanitizers

- **AddressSanitizer**: add `-DVORTEX_ASAN=ON` during CMake configuration
- **UndefinedBehaviorSanitizer**: add `-DVORTEX_UBSAN=ON`

Integration tests clean up temporary index files automatically in `TearDown()` — no leftover files.

### Benchmarks

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DVORTEX_BUILD_BENCHMARKS=ON
cmake --build build -j$(nproc)
cd build && ./benchmarks/vortex_benchmarks
```

## Design Principles

1. **Data structures determine performance ceilings** — FST, SIMD-BP128, SkipList, Roaring, HNSW; every choice is validated by industry benchmarks
2. **Explicit, controllable memory allocation** — Arena allocation guarantees no malloc on hot paths and predictable memory budgets
3. **Zero-copy reads** — mmap + FST direct mapping; query path never copies intermediate buffers
4. **Data safety by default** — xxHash64 checksums on every file, atomic flush (tmp → rename) prevents partial writes
5. **Observable from day one** — histograms, counters, and statistics built in, not retrofitted
6. **Errors via API, never exceptions** — `Status` / `Result<T>` at every layer boundary; only logic bugs trigger PANIC
7. **Runtime SIMD dispatch** — single binary supports all x86-64 CPUs; optimal path selected at initialization
8. **Single writer, multiple readers** — industry-standard model (Lucene, Tantivy); multi-writer via external sharding

## Roadmap

| Phase | Scope | Status |
|-------|-------|--------|
| 0 | Infrastructure: Status, Arena, xxHash64, type aliases | ✅ Done |
| 1 | Inverted index: Analyzer, FST dictionary, SIMD-BP128 codec, BM25F | ✅ Done |
| 2 | Vector index: HNSW graph, L2/IP/Cosine distance | 📋 Planned |
| 3 | RecallEngine: multi-index orchestration, RRF/weighted fusion, concurrent queries | 📋 Planned |
| 4 | FlatBuffers serialization, index persistence/loading | 📋 Planned |
| 5 | Performance benchmarks and tuning | 📋 Planned |
| 6 | Web console: visual debugging, document management, query analysis | 🔮 Future |

## Design Docs

- [Inverted Index V1](docs/inverted_index_design_v1.md) — segment architecture, FST, SIMD-BP128, WAL, BM25F, RCU concurrency model

## License

MIT — see [LICENSE](LICENSE).
