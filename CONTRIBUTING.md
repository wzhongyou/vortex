# Contributing to Vortex

Thanks for your interest! Vortex is currently in the **planning phase** — early design discussions and contributions are welcome.

## How to Contribute

- **Design discussions** — participate in architecture and API design via GitHub Issues
- **Code** — fork the repo, develop on a feature branch, submit PRs to `main`
- **Documentation** — improvements and translations are also appreciated

## Development Environment

- C++17 compiler (GCC 9+ / Clang 10+ / Apple Clang 14+)
- CMake 3.16+
- GoogleTest (testing) / Google Benchmark (benchmarks)
- FlatBuffers (serialization)

```bash
# Configure and build
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DVORTEX_BUILD_TESTS=ON
cmake --build build -j$(nproc)

# Run tests
cd build && ctest --output-on-failure
```

## Code Style

- C++17 standard
- `#pragma once` header guards
- `vortex::` namespace
- Header-only for template-heavy code; `.cpp` for non-template logic
- Commit messages in English

## License

MIT — all contributions are licensed under this agreement.
