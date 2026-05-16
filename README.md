# Vortex — Multi-Recall Engine

Vortex is a high-performance, multi-strategy recall engine written in modern C++(17).

It supports pluggable index backends — inverted index for keyword search, vector index for semantic search — and fuses their results into a unified ranked list.

## Planned Features

- **Inverted Index** — tokenization → posting lists → BM25 / TF-IDF scoring
- **Vector Index** — HNSW graph with L2 / inner product / cosine distance
- **Hybrid Recall** — query multiple indexes concurrently and merge results via RRF or weighted fusion
- **Pluggable Architecture** — implement the `Index` interface for custom recall strategies
- **Zero-copy Serialization** — FlatBuffers-based index persistence

## Project Status

🚧 **规划中 (Planning)** — 核心架构设计中。详见 [CLAUDE.md](CLAUDE.md)。

## License

MIT — see [LICENSE](LICENSE).
