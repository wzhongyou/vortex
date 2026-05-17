# Inverted Index V1 Design

## 1. Goals & Scope

Implement a production-grade single-node inverted index. The core architecture follows Lucene's Segment design. All data structures and algorithms are designed from day one for production deliverables — no throwaway prototypes.

### 1.1 V1 Deliverables

| Feature | Standard |
|---------|----------|
| Boolean queries | Term / AND / OR / NOT |
| Scoring | BM25F with per-field weights, pluggable Scorer interface |
| Per-field indexing | each TEXT field independently tokenized and indexed |
| Segment architecture | in-memory writes → atomic flush to immutable on-disk segments |
| Snapshot isolation | RCU-protected read path, writes never block reads |
| WAL | binary format, CRC32 per record, Group Commit |
| Disk I/O | mmap zero-copy reads, fallocate preallocation |
| Dictionary | FST (Finite State Transducer), byte-based incremental minimization |
| Posting lists | SIMD-BP128 block encoding, multi-level skip list |
| Memory | Arena-based layered allocation, no bare new/malloc in hot paths |
| Data integrity | xxHash64 trailer on every data file, CRC32 on WAL records |
| Observability | latency histograms, throughput counters, memory/disk usage |
| Error handling | `Status` / `Result<T>` — no exceptions across API boundaries |

### 1.2 V1 Out of Scope

Segment merging, phrase queries, NRT search, distributed, custom analyzers via config file.

## 2. Error Handling Model

```
Rule: API boundaries return Status or Result<T>. No exceptions.
      Internal invariant violations use VORTEX_PANIC (logic bugs, not runtime errors).
      Constructors use VORTEX_PANIC only; factory methods return Result<T>.
```

```cpp
class Status {
public:
    enum Code : uint8_t {
        OK = 0,
        INVALID_ARGUMENT,
        IO_ERROR,
        CHECKSUM_MISMATCH,
        CORRUPT_INDEX,
        OUT_OF_MEMORY,
        INTERNAL,
    };

    bool ok() const { return code_ == OK; }
    Code code() const { return code_; }
    std::string_view message() const { return msg_; }

    static Status OK() { return {OK, {}}; }
    static Status InvalidArgument(std::string msg) { return {INVALID_ARGUMENT, std::move(msg)}; }
    static Status IOError(std::string msg) { return {IO_ERROR, std::move(msg)}; }
    static Status ChecksumMismatch(std::string msg) { return {CHECKSUM_MISMATCH, std::move(msg)}; }
    static Status CorruptIndex(std::string msg) { return {CORRUPT_INDEX, std::move(msg)}; }

private:
    Code code_;
    std::string msg_;  // SSO — no heap allocation for short messages
};

template<typename T>
class Result {
public:
    // success
    static Result<T> Ok(T value) { return {std::move(value), Status::OK()}; }
    // failure
    static Result<T> Err(Status status) { return {{}, std::move(status)}; }

    bool ok() const { return status_.ok(); }
    T& value() { return value_; }
    T&& move_value() { return std::move(value_); }
    const Status& status() const { return status_; }

private:
    T value_;
    Status status_;
};

#define VORTEX_PANIC(msg) \
    do { std::cerr << "[PANIC] " << msg << std::endl; std::abort(); } while(0)
```

## 3. Memory Management

### 3.1 Layered Arenas

All memory allocation during indexing flows through arenas. No bare `new`/`malloc` in write or read hot paths.

```
Arena hierarchy:

IndexWriter owns:
  ├── active_arena_        ← current in-memory segment (bulk-reset after flush)
  └── flush_arena_         ← double-buffer: while flushing, new writes use active_arena_

Per-request (thread-local):
  └── ThreadLocalArena     ← 256 KB slab chain, used by analyzer token output
                              ScopedThreadArena binds/unbinds, auto-reset on scope exit

FST construction (independent):
  ├── Pass 1 Arena         ← term collection, node graph construction
  └── Pass 2 Arena         ← serialization; then Pass 1 Arena is freed
```

```cpp
class Arena {
public:
    explicit Arena(size_t chunk_size = 256 * 1024);

    void* allocate(size_t size, size_t alignment = alignof(std::max_align_t));

    // Fast reset: preserves current chunk, resets usage cursor
    void reset();

    // Full release: frees all chunks
    void clear();

    size_t allocated() const;
    size_t wasted() const;

private:
    struct Chunk {
        std::unique_ptr<char[]> data;
        size_t size;
        size_t used;
        Chunk* next;
    };
    Chunk* head_ = nullptr;
    Chunk* current_ = nullptr;
    size_t chunk_size_;
};

// Per-thread arena binding
class ScopedThreadArena {
public:
    explicit ScopedThreadArena(Arena& arena);
    ~ScopedThreadArena();  // unbinds + reset arena
    ScopedThreadArena(const ScopedThreadArena&) = delete;
    ScopedThreadArena& operator=(const ScopedThreadArena&) = delete;
};

// Thread-local registry
Arena& thread_arena();
```

### 3.2 Disk Preallocation

```cpp
// Called before writing .doc / .fst / .store
// Uses posix_fallocate (Linux) or fcntl F_PREALLOCATE (macOS)
Status preallocate_file(int fd, off_t estimated_size);
```

### 3.3 Flush Threshold

Flush triggers when `active_arena_.allocated()` exceeds 64 MB. This is configurable via `IndexWriterOptions::ram_buffer_mb` (range: 16–256 MB). The 64 MB default balances write amplification against query latency (fewer segments = less cross-segment merge work).

## 4. Data Model

### 4.1 Schema

```cpp
enum class FieldType : uint8_t {
    TEXT,       // full-text indexed + tokenized
    KEYWORD,    // exact match, not tokenized
};

struct FieldSchema {
    std::string name;
    FieldType type;
    bool stored;     // raw value preserved for retrieval
    bool indexed;    // added to inverted index
};

struct Schema {
    Status add_field(FieldSchema field);
    const FieldSchema* field(std::string_view name) const;
    uint16_t field_index(std::string_view name) const;  // returns UINT16_MAX if not found

    std::vector<FieldSchema> fields;
    absl::flat_hash_map<std::string, uint16_t> name_to_index;
    uint16_t stored_field_count;   // cached count of stored=true fields
    uint16_t indexed_field_count;  // cached count of indexed=true, type=TEXT fields
};
```

### 4.2 Document

```cpp
struct FieldValue {
    std::string name;
    std::string value;       // V1: text only
};

struct Document {
    std::vector<FieldValue> fields;
};
```

Documents carry no external ID in the struct. Internal IDs are assigned by IndexWriter (dense, monotonically increasing uint32 within each segment). The external ID must be provided as a KEYWORD stored field.

### 4.3 Internal Document Representation

```cpp
struct InternalDoc {
    uint64_t segment_id;
    uint32_t doc_id;         // dense within segment
    uint32_t doc_length;     // sum of all term counts across fields

    // Per-field term counts, indexed by field_index.
    // Only populated for indexed TEXT fields (len = schema.indexed_field_count).
    // Uses small_vector: inline storage for <= 8 fields, heap for more.
    absl::InlinedVector<uint32_t, 8> field_lengths;

    // Stored field values, indexed by stored_field_index (not field_index).
    // Length = schema.stored_field_count.
    absl::InlinedVector<std::string, 4> stored_values;
};
```

`absl::InlinedVector` provides the small-size optimization needed here — typical schemas have 1–8 indexed fields, avoiding heap allocation for the common case while being valid C++.

## 5. Segment Architecture

### 5.1 Overall Structure

```
┌──────────────────────────────────────────────────────┐
│                   IndexWriter                         │
│                                                       │
│  ┌──────────────┐    ┌──────────────┐                │
│  │ MemorySegment│    │ WAL          │                │
│  │ (mutable)    │    │ (binary,     │                │
│  │ Arena-backed │    │  CRC32)      │                │
│  └──────┬───────┘    └──────────────┘                │
│         │ atomic flush (.tmp → rename)                │
│         ▼                                             │
│  ┌──────────────┐    ┌──────────────┐                │
│  │ Segment 0    │    │ Segment 1    │  (immutable,   │
│  │ (on disk)    │    │ (on disk)    │   mmap'd)      │
│  └──────────────┘    └──────────────┘                │
│         │                   │                         │
│         └─────────┬─────────┘                         │
│                   ▼                                   │
│         ┌──────────────────┐                          │
│         │ SegmentMerge     │ (V2)                     │
│         └──────────────────┘                          │
└──────────────────────────────────────────────────────┘
                    ▲
        ┌───────────┴───────────┐
        │     IndexReader       │
        │  (point-in-time view) │
        │  RCU-protected        │
        │  owns its own BM25    │
        │  statistics snapshot   │
        └───────────────────────┘
```

### 5.2 Segment On-Disk Files

```
Segment N (after atomic flush):
├── _N.fst       FST dictionary: term → {doc_freq, posting_offset, posting_len}
├── _N.doc       Posting data: [block_header][SIMD-packed deltas][freqs]
├── _N.pos       Position data (V2 phrase queries — NOT written in V1)
├── _N.store     Row-oriented doc store: doc_id → stored_field_values
├── _N.fwd       Forward index: doc_id → {doc_length, field_lengths[]}
├── _N.idm       External ID mapping: internal_doc_id → external_id (string)
├── _N.meta      JSON metadata with per-file xxHash64 checksums
└── _N.del       Deletion bitmap (Roaring Bitmap)
```

Each data file ends with an 8-byte xxHash64 trailer covering the file body (excluding the trailer itself).

### 5.3 Atomic Flush Protocol

Flushing is the most dangerous operation — a crash mid-flush must not corrupt the index.

```
flush() protocol:

1. Compute estimated file sizes, fallocate .tmp files:
   _N.fst.tmp, _N.doc.tmp, _N.store.tmp, _N.fwd.tmp, _N.idm.tmp, _N.meta.tmp

2. Write all data to .tmp files, compute xxHash64 trailers

3. fsync all .tmp files (in order: data files first, .meta.tmp last)

4. Atomic rename (same filesystem, so rename is atomic on POSIX):
   for each .tmp: rename(_N.xxx.tmp → _N.xxx)

5. Append segment entry to segments.manifest, fsync manifest

6. Register segment in SegmentList (RCU swap)

7. WAL::truncate()

8. Release flush_arena_, create new empty MemorySegment on active_arena_

Crash recovery:
  - List index_dir for *.tmp files → delete them (partial flush, discard)
  - Reload segments from manifest → each listed segment is complete
  - Replay WAL from last truncation point → rebuild memory segment
```

Key invariant: **A segment is only visible after its entry appears in segments.manifest.** The manifest is the authoritative source of truth about which segments exist.

### 5.4 Write Path

```
add_document(doc):
  1. Assign internal_doc_id = next_id_++
  2. WAL::append_add(internal_doc_id, doc)    ← crash-safe before index update
  3. For each TEXT field with indexed=true:
       a. MixedTokenizer → LowercaseFilter → StopwordFilter
       b. Output: vector<TermWithFreq> allocated on thread_arena()
       c. Insert each term into MemorySegment:
            FST builder: term → accumulating PostingListBuilder
            PostingListBuilder.append(doc_id, tf)
       d. Record field_lengths for this field
  4. Compute doc_length = sum(field_lengths)
  5. Write stored field values to MemorySegment's doc store
  6. Write external_id → internal_doc_id mapping
  7. If active_arena_.allocated() >= flush_threshold_ → flush()

remove_document(external_id):
  1. WAL::append_remove(external_id)
  2. Lookup external_id in global ID mapping → (segment_id, internal_doc_id)
  3. If in active MemorySegment: mark deleted in in-memory bitmap
  4. If in flushed segment: write to _N.del (Roaring Bitmap)
  5. Update global ID mapping (remove entry)

update_document(external_id, new_doc):
  = remove_document(external_id) + add_document(new_doc)
  (WAL records both operations atomically in a single group)
```

### 5.5 Read Path (Lock-Free Snapshot)

```
IndexReader construction:
  1. Obtain SegmentsSnapshot from SegmentList (RCU load, no lock)
  2. Compute reader-local aggregate statistics:
       total_docs = sum(seg.doc_count)
       avgdl = sum(seg.total_term_count) / total_docs
       field_avg_lengths[f] = sum(seg.field_total_terms[f]) / total_docs
  3. Construct BM25FScorer with these statistics
  4. For each segment: mmap .fst, .doc, .fwd, .del files (lazy, on first use)
  5. Validate xxHash64 trailers

IndexReader::search(query, topk):
  1. for each segment (in parallel if segments > 1):
       a. Execute query on segment → list of (doc_id, per_term_tfs[])
       b. Filter against segment's .del bitmap → discard deleted docs
       c. For each surviving candidate:
            Read .fwd to get doc_length, field_lengths[]
            Compute BM25F score using reader-local statistics
       d. Produce segment-local topk heap
  2. Cross-segment merge: min-heap of size topk, pop from each segment heap
  3. For each topk result: resolve external_id from .idm
  4. Return SearchResult

IndexReader destructor:
  1. Decrement epoch refcount on SegmentsSnapshot
```

## 6. Concurrency Model

### 6.1 RCU Segment List

```cpp
// SegmentsSnapshot — a point-in-time view of all committed segments.
// Reference-counted via epoch counter. Freed when no reader holds the epoch.
struct SegmentsSnapshot {
    std::atomic<uint32_t> active_readers{0};  // epoch refcount
    uint64_t epoch_id;
    std::vector<std::shared_ptr<const Segment>> segments;

    // Aggregate statistics, computed at snapshot creation time
    uint64_t total_docs;
    double avgdl;
    std::vector<double> field_avg_lengths;  // indexed by field_index
    std::vector<uint64_t> field_total_terms;
};

class SegmentList {
public:
    // Read path: lock-free, acquire ordering
    // Returns raw pointer valid until exit_epoch()
    const SegmentsSnapshot* acquire_snapshot();

    // Read path: release the snapshot
    void release_snapshot(const SegmentsSnapshot* snap);

    // Write path: install new snapshot with added segment
    void publish_segment(std::shared_ptr<const Segment> seg);

    // Triggered periodically by writer or on shutdown
    // Frees snapshots whose active_readers == 0
    void reclaim_retired_snapshots();

private:
    std::atomic<SegmentsSnapshot*> current_{nullptr};

    // Only accessed by writer thread (single writer assumption)
    std::vector<SegmentsSnapshot*> retired_;
    uint64_t next_epoch_{1};
};
```

### 6.2 mmap Lifecycle

mmap regions must outlive the last reader that references them. The design ties mmap lifetime to the Segment object, and Segment lifetime to SegmentsSnapshot epochs:

```
Segment lifetime protocol:

1. Active segments: referenced by SegmentsSnapshot::segments (shared_ptr)
2. When a new snapshot is published (after flush):
   − Old snapshot enters retired_ list
   − Its shared_ptr<Segment> refs keep files alive
3. reclaim_retired_snapshots() checks active_readers == 0
   − If zero: destruct snapshot → shared_ptr refs released → munmap
   − Segments not in any active snapshot are safe to delete

V2 merge: merged segments go through the same retirement pipeline.
Files are unlinked only after munmap completes (no SIGBUS possible).
```

```cpp
class MmapHandle {
public:
    MmapHandle(int fd, size_t size, const void* mapped_addr);
    ~MmapHandle();  // munmap + close(fd)
    MmapHandle(const MmapHandle&) = delete;
    MmapHandle& operator=(const MmapHandle&) = delete;

    const uint8_t* data() const;
    size_t size() const;

private:
    int fd_;
    size_t size_;
    const uint8_t* addr_;
};
```

### 6.3 Writer Locking

```cpp
class IndexWriter {
    std::mutex write_mutex_;  // serializes all write operations
    // Single-writer concurrency is the industry standard assumption
    // (Lucene, Tantivy, etc. all serialize writes).
    // Multi-writer scenarios use external sharding.
};
```

## 7. Unicode Text Analysis

### 7.1 Normalization

Full-width → half-width, accent folding, case folding. These are quality fundamentals — searching "café" must hit "cafe".

```cpp
// Built-in: ASCII lowercase + CJK fullwidth/halfwidth mapping table
// Covers common Chinese IME symbols, digits, and Latin letters.
// Table is compile-time generated, ~8 KB.
std::string nfkc_normalize(std::string_view input);
std::string lowercase_ascii(std::string_view input);

// Optional ICU integration: full NFKC + locale-aware case folding
// Controlled by VORTEX_USE_ICU compile flag
```

### 7.2 Streaming Tokenizer

Push-based model eliminates intermediate `vector<Token>` allocation for large text:

```cpp
class TokenConsumer {
public:
    virtual void on_token(Token token) = 0;
    virtual ~TokenConsumer() = default;
};

struct Token {
    std::string_view text;  // points into normalized text buffer
    uint16_t position;      // V1: reserved for V2 phrase queries
    uint16_t start_offset;
    uint16_t end_offset;
};

class Tokenizer {
public:
    virtual void tokenize(std::string_view text,
                          TokenConsumer& consumer) = 0;
    virtual ~Tokenizer() = default;
};

// Latin-script word boundary tokenizer
class StandardTokenizer : public Tokenizer { ... };

// CJK Bigram: "我爱北京" → ["我爱","爱北","北京"]
// Minimal viable approach — no external dictionary dependency
class CJKBigramTokenizer : public Tokenizer { ... };

// Auto-dispatch: detects CJK character ratio in text,
// delegates to Standard or CJKBigram internally
class MixedTokenizer : public Tokenizer { ... };
```

CJK Bigram is the validated minimal solution (Elasticsearch CJK Bigram Token Filter, same approach). V2 upgrades to dictionary-based segmentation with unchanged API.

### 7.3 Filter Chain

Filters are also push-based, composable in sequence:

```cpp
class TokenFilter {
public:
    // Receives one token, may emit 0..N tokens downstream
    virtual void process(Token token, TokenConsumer& downstream) = 0;
    virtual ~TokenFilter() = default;
};

class LowercaseFilter : public TokenFilter { ... };
class StopwordFilter : public TokenFilter {
    explicit StopwordFilter(std::shared_ptr<absl::flat_hash_set<std::string>> words);
};
class NFKCFilter : public TokenFilter { ... };
```

### 7.4 Analyzer API

```cpp
class Analyzer {
public:
    Analyzer(std::unique_ptr<Tokenizer> tokenizer,
             std::vector<std::unique_ptr<TokenFilter>> filters);

    // Output term → in-document term frequency.
    // Allocated on the current thread arena.
    struct TermWithFreq {
        std::string_view term;  // points into arena
        uint32_t tf;
    };
    void analyze(std::string_view text,
                 std::vector<TermWithFreq>& output,
                 Arena& arena);

private:
    std::unique_ptr<Tokenizer> tokenizer_;
    std::vector<std::unique_ptr<TokenFilter>> filters_;
};
```

## 8. FST Dictionary

### 8.1 Why FST

| | Hash Map | FST |
|------|----------|-----|
| Prefix compression | none | automatic prefix/suffix sharing |
| Memory (1M terms) | ~80–120 MB | ~4–8 MB |
| Ordered iteration | extra sort required | natural lexicographic order |
| Prefix query | O(k × N) | O(k) |
| Build cost | online O(N) | offline O(N log N) (sorted input) |

Search engine term dictionaries have large string key sets with natural ordering — FST is the clear choice. Lucene has used FST for its term dictionary since 4.0.

### 8.2 FST Data Flow

```
Indexing (build phase, sorted by term):
  term → {doc_freq, posting_offset, posting_len}
    │
    ▼
  TermDictBuilder (incremental minimization)
    │ finish()
    ▼
  byte array (serialized FST)
    │ write to _N.fst + xxHash64 trailer
    ▼
  TermDict (mmap'd directly, zero-copy)

Query:
  lookup("hello")  → traverse FST arcs → TermInfo*
  prefix_range("he") → iterate matching sub-tree
```

### 8.3 API

```cpp
struct TermInfo {
    uint32_t doc_freq;        // number of documents containing this term
    uint64_t posting_offset;  // byte offset in .doc file
    uint32_t posting_len;     // byte length in .doc file
};

// ── Build Phase ──
class TermDictBuilder {
public:
    // Terms MUST be inserted in lexicographic order
    void insert(std::string_view term, const TermInfo& info);

    // Finalize: run incremental minimization, produce serialized FST bytes
    std::vector<uint8_t> finish();

private:
    // Incremental minimization: maintains a stack of unfrozen nodes.
    // When the prefix diverges from the next term, freeze the divergent
    // suffix and reuse equivalent subtrees. Algorithm follows Lucene's
    // FST.Builder with key differences documented in implementation.
    struct UnfrozenNode {
        std::string_view prefix;
        std::vector<std::pair<uint8_t, uint32_t>> arcs; // label → target_node
        uint64_t output;  // accumulated output value
    };
    std::vector<UnfrozenNode> prefix_stack_;
    // Hash of [arc_labels + targets + output] → canonical node_id for
    // detecting equivalent subtrees (the core of minimization).
    absl::flat_hash_map<size_t, uint32_t> canonical_nodes_;
    std::vector<uint8_t> serialized_;
};

// ── Query Phase ──
class TermDict {
public:
    // Load from mmap'd buffer (zero-copy — references data, not copies)
    static Result<std::unique_ptr<TermDict>> from_mmap(
        const uint8_t* data, size_t len, uint64_t xxhash64_expected);

    const TermInfo* lookup(std::string_view term) const;

    // Iterate all terms with given prefix.
    // Callback signature: bool(std::string_view term, const TermInfo&)
    // Return false from callback to stop early.
    void prefix_range(std::string_view prefix,
                      std::function<bool(std::string_view, const TermInfo&)> fn) const;

    size_t term_count() const;
    size_t memory_usage() const { return data_len_; }

private:
    const uint8_t* data_;
    size_t data_len_;
};
```

### 8.4 FST Build Algorithm Sketch

Incremental minimization (the core FST build algorithm):

```
insert(term, info):
  // Find common prefix length with previous term
  cpl = common_prefix_len(term, prev_term)

  // Freeze nodes below cpl that are no longer on the shared path
  while prefix_stack_.size() > cpl + 1:
    node = prefix_stack_.pop_back()
    frozen = try_minimize(node)
    serialize frozen to output

  // Add new suffix nodes for the non-shared part of term
  for i = cpl .. term.size():
    prefix_stack_.push(new UnfrozenNode{term[0..i]})

  // Attach output to the terminal node
  prefix_stack_.back().output = encode(info)

finish():
  // Freeze remaining stack from bottom to top
  while prefix_stack_.size() > 1:
    node = prefix_stack_.pop_back()
    serialize try_minimize(node)
  // Root is the last remaining node
  root_node = prefix_stack_[0]
  write root_node with pointer to it as entry point

try_minimize(node):
  hash = hash_arcs(node.arcs, node.output)
  if existing = canonical_nodes_.find(hash):
    return existing  // reuse equivalent subtree
  canonical_nodes_[hash] = node
  return node
```

Implementation note: this is the most algorithmically complex component. Plan for 5 days of dev + 2 days of edge-case testing (empty term, single-char terms, very long common prefixes, 1M+ term stress test).

## 9. Posting List Codec

### 9.1 SIMD-BP128 Block Encoding

Block size = 128 documents, chosen to match SIMD register width:

- 128-bit SIMD (SSE4.2): process 128 docs as 128×4 = 512 bits = 4 × __m128i
- 256-bit SIMD (AVX2): process 128 docs as 128×2 = 256 bits = 2 × __m256i

Encoding within a block:

```
Block Layout:
┌────────────┬─────────────────────┬─────────────────────┬──────────┐
│ BlockHeader│ Packed DocID Deltas │ Packed Term Freqs   │ padding  │
│ (4 bytes)  │ (variable bytes)   │ (variable bytes)    │ (to 4B)  │
└────────────┴─────────────────────┴─────────────────────┴──────────┘

BlockHeader (4 bytes):
  num_docs  : uint8   (1–128)
  doc_bits  : uint8   (bits needed for largest docID delta)
  freq_bits : uint8   (bits needed for largest term freq)
  flags     : uint8   (reserved)

Packed Deltas (SIMD-BP128):
  Input:  128 docID deltas, each ≤ 2^doc_bits - 1
  Output: ceil(128 × doc_bits / 8) bytes, stored as bit-planes
  Bit-plane i stores bit i of all 128 deltas (128 bits = 16 bytes, naturally aligned)

Packed Freqs:
  Same layout as deltas, using freq_bits bit width.
```

### 9.2 SIMD Runtime Dispatch

The library compiles a single binary that runs on CPUs with or without AVX2:

```cpp
// posting_codec.cpp — compiled WITHOUT -mavx2 (baseline SSE4.2 path)
size_t decode_block(const uint8_t* input, uint32_t* doc_ids,
                    uint32_t* freqs, uint8_t& num_docs);

// posting_codec_avx2.cpp — compiled WITH -mavx2
size_t decode_block_avx2(const uint8_t* input, uint32_t* doc_ids,
                          uint32_t* freqs, uint8_t& num_docs);

// Function pointer, set once at init time
using DecodeFunc = size_t (*)(const uint8_t*, uint32_t*, uint32_t*, uint8_t&);
extern DecodeFunc g_decode_block;

// Called in IndexWriter::open()
void codec_init() {
    if (__builtin_cpu_supports("avx2")) {
        g_decode_block = decode_block_avx2;
    } else {
        g_decode_block = decode_block;
    }
}
```

The SSE4.2 baseline is always available (x86-64 requires SSE4.2), covering all deployment targets.

### 9.3 Multi-Level Skip List

Skip entries are at **block level**, not posting level:

```cpp
// One skip entry per block at each level
struct SkipEntry {
    uint32_t last_doc_id;     // max doc_id in the skipped block(s)
    uint64_t block_offset;    // file offset of the target block
};

class SkipList {
public:
    // Build multi-level skip from block metadata.
    // skip_interval: blocks between skip entries at level 0 (default 4)
    void build(const std::vector<uint64_t>& block_offsets,
               const std::vector<uint32_t>& block_max_doc,
               int skip_interval = 4);

    // Find the block just before `target`, starting from `current_offset`.
    // Returns block_offset (file position). Used by advance_to().
    uint64_t advance(uint32_t target, uint64_t current_offset) const;

    // Serialize to bytes (appended at end of posting data in .doc)
    std::vector<uint8_t> serialize() const;

    // Deserialize from tail of .doc
    static SkipList deserialize(const uint8_t* data, size_t len);

private:
    // levels_[0]: every skip_interval blocks
    // levels_[1]: every skip_interval^2 blocks
    // levels_[2]: every skip_interval^3 blocks, etc.
    std::vector<std::vector<SkipEntry>> levels_;
};
```

### 9.4 PostingList API

```cpp
class PostingListBuilder {
public:
    void append(uint32_t doc_id, uint32_t term_freq);

    // Returns {offset_in_doc_file, byte_count}
    // Posting data + skip list footer written contiguously
    Result<std::pair<uint64_t, uint32_t>> flush(int fd, Arena& scratch);

    size_t doc_freq() const { return total_docs_; }

private:
    uint32_t doc_buf_[128];
    uint32_t freq_buf_[128];
    uint8_t  count_ = 0;
    std::vector<uint64_t> block_offsets_;
    std::vector<uint32_t> block_max_doc_;
    size_t total_docs_ = 0;
};

class PostingListReader {
public:
    PostingListReader(const uint8_t* data, size_t len);
    // data points into mmap'd .doc file

    // Iterate all postings. Fn: void(uint32_t doc_id, uint32_t tf)
    template<typename Fn>
    void for_each(Fn&& fn) const;

    // Advance to the first doc_id >= target.
    // Returns false if no such doc (exhausted).
    // Uses skip list to jump over irrelevant blocks.
    bool advance_to(uint32_t target, uint32_t& doc_id, uint32_t& tf) const;

    size_t doc_freq() const { return total_docs_; }

private:
    const uint8_t* data_;
    size_t data_len_;
    size_t total_docs_;
    SkipList skip_list_;
    size_t block_count_;

    // Current decode position
    uint32_t current_block_;
    const uint8_t* next_block_ptr_;
};
```

### 9.5 Sparse Posting Optimization

Terms appearing in > 10% of documents have IDF contribution near zero. For these terms:

- Score computation uses an approximate IDF (precomputed constant), skipping exact doc_freq lookup
- `advance_to()` supports early termination: if the remaining postings' max possible score contribution is below the current topk heap minimum, stop traversal
- Flagged in term metadata as SPARSE

## 10. Forward Index (.fwd)

### 10.1 Purpose

The forward index maps `doc_id → (doc_length, field_lengths[])`. BM25F scoring requires `doc_length` and per-field lengths for term frequency normalization. Storing these in the inverted posting list itself would bloat it; a separate dense array is the standard solution.

### 10.2 Binary Format

```
Forward Index File (_N.fwd):

Header (16 bytes):
  doc_count        : uint32   (number of documents in this segment)
  field_count      : uint16   (number of indexed TEXT fields)
  entry_size       : uint16   (bytes per entry, fixed)
  reserved         : uint64   (padding to 16B alignment)

Body (doc_count × entry_size bytes):
  For each doc_id (dense, 0..doc_count-1):
    doc_length      : uint32   (total terms across all fields)
    field_length[0] : uint32   (term count for field 0)
    field_length[1] : uint32   (term count for field 1)
    ...
    field_length[field_count-1] : uint32

Trailer (8 bytes):
  xxHash64 over [Header + Body]
```

Each entry is `4 + 4 × field_count` bytes. For a typical schema with 3 text fields: 16 bytes per doc. 1M docs = 16 MB per segment — compact enough to mmap entirely.

### 10.3 API

```cpp
class ForwardIndexBuilder {
public:
    ForwardIndexBuilder(uint16_t field_count);

    // Called once per document, in doc_id order
    void append(uint32_t doc_length, const uint32_t* field_lengths);

    Status flush(int fd);

    size_t doc_count() const;

private:
    uint16_t field_count_;
    std::vector<uint8_t> buffer_;  // linear buffer, grows to doc_count * entry_size
    size_t doc_count_ = 0;
};

class ForwardIndex {
public:
    static Result<std::unique_ptr<ForwardIndex>> from_mmap(
        const uint8_t* data, size_t len, uint64_t xxhash64_expected);

    struct DocInfo {
        uint32_t doc_length;
        const uint32_t* field_lengths;  // points into mmap, length = field_count
    };

    DocInfo get(uint32_t doc_id) const;

    uint16_t field_count() const;
    uint32_t doc_count() const;
    size_t memory_usage() const;

private:
    const uint8_t* data_;
    size_t len_;
    uint32_t doc_count_;
    uint16_t field_count_;
    uint16_t entry_size_;
};
```

## 11. External ID Mapping (.idm)

### 11.1 Problem

`remove_document(external_id)` must locate which `(segment_id, internal_doc_id)` holds that external ID. Scanning stored fields across all segments is O(total_docs). A dedicated mapping is required.

### 11.2 In-Memory Map

```cpp
// Global mapping maintained by IndexWriter
class ExternalIdMap {
public:
    struct Location {
        uint64_t segment_id;
        uint32_t internal_doc_id;
    };

    // Returns nullptr if not found
    const Location* find(std::string_view external_id) const;

    // Insert on add_document
    void insert(std::string_view external_id, uint64_t seg_id, uint32_t doc_id);

    // Remove on delete
    void remove(std::string_view external_id);

    // Save to .idm file during flush (for the active memory segment)
    Status flush(int fd, Arena& scratch);  // sorted by doc_id

    // Load from .idm file
    static Result<std::unique_ptr<ExternalIdMap>> from_file(
        const std::string& path, uint64_t xxhash64_expected);

    size_t size() const;

private:
    // external_id → location in active memory segment or flushed segment
    absl::flat_hash_map<std::string, Location> map_;
    // Also store reverse: for a given segment, doc_id → external_id string
    // Used when building .idm file (sorted by doc_id)
    std::vector<std::string> pending_external_ids_;  // indexed by internal_doc_id
};
```

### 11.3 .idm File Format

```
_ID Mapping File (_N.idm):

Header (12 bytes):
  doc_count   : uint32
  reserved    : uint64

Body (variable, sorted by internal_doc_id):
  For each doc_id (0 .. doc_count-1):
    external_id_len : uint16
    external_id     : char[external_id_len]  (not null-terminated)

Trailer (8 bytes):
  xxHash64 over [Header + Body]
```

Sizing: assuming average external_id 20 bytes, 1M docs = ~22 MB per segment. Acceptable for an mmap'd file.

### 11.4 Search Result Resolution

```cpp
// In IndexReader::search(), after obtaining topk:
for (auto& doc : topk) {
    auto* loc = external_id_map_->resolve(doc.segment_id, doc.internal_doc_id);
    doc.external_id = loc ? std::string(loc->external_id) : "<deleted>";
}
```

```cpp
struct ScoredDoc {
    uint32_t internal_doc_id;
    uint64_t segment_id;
    float score;
    std::string external_id;   // resolved from .idm
};
```

## 12. Delete Handling

### 12.1 Deletion Bitmap

Deletions use Roaring Bitmap (compressed bitset, widely used in search engines):

```cpp
class DeleteBitmap {
public:
    bool is_deleted(uint32_t doc_id) const;
    void mark_deleted(uint32_t doc_id);
    void mark_deleted_bulk(const std::vector<uint32_t>& doc_ids);

    size_t deleted_count() const;
    size_t total_count() const { return total_; }
    void set_total(uint32_t total);

    // Serialization
    std::vector<uint8_t> serialize() const;  // Roaring format
    static DeleteBitmap deserialize(const uint8_t* data, size_t len);

private:
    Roaring roaring_;
    uint32_t total_ = 0;  // doc_count of this segment
};
```

### 12.2 Delete Flow

```
remove_document(external_id):
  1. WAL::append_remove(external_id)
  2. Location* loc = external_id_map_.find(external_id)
  3. If loc->segment_id == active_segment_id:
       memory_segment_.deletes.mark_deleted(loc->internal_doc_id)
  4. Else:
       Load _N.del for segment loc->segment_id
       bitmap.mark_deleted(loc->internal_doc_id)
       Save _N.del
  5. external_id_map_.remove(external_id)

Query filtering (index_reader.cpp):
  After collecting candidate doc_ids from posting list intersection/union:
    for each candidate:
      segment = segments_[candidate.segment_id]
      if segment->deletes().is_deleted(candidate.doc_id):
        continue  // skip deleted doc
      score = bm25f.score(...)
      push to heap
```

### 12.3 Delete Lifecycle

- Active segment deletes: in-memory bitmap, flushed with segment
- Flushed segment deletes: persisted to `.del` file, loaded on reader open
- V2 segment merge: deleted docs are physically removed (not copied to merged segment)

## 13. BM25F Scoring

### 13.1 Field-Weighted BM25

```cpp
struct FieldBM25Params {
    double k1 = 1.2;
    double b = 0.75;
    double weight = 1.0;  // relative field weight in final score
};

struct BM25Params {
    std::vector<FieldBM25Params> fields;  // indexed by field_index
};
```

Default: `k1=1.2`, `b=0.75`, `weight=1.0` for all fields. Field weights tuned via `BM25Params`.

### 13.2 Per-Reader Statistics

Each IndexReader computes its own aggregate statistics from the SegmentsSnapshot it holds. This is essential for correctness — different readers see different segment sets.

```cpp
// IndexReader-local, computed at open() time
struct ReaderStatistics {
    uint64_t total_docs;
    double avgdl;                         // across all documents
    std::vector<double> field_avg_lengths; // per-field_index
};
```

### 13.3 API

```cpp
class Scorer {
public:
    virtual ~Scorer() = default;

    // Initialize with reader-local aggregate statistics
    virtual void init(const ReaderStatistics& stats, const BM25Params& params) = 0;

    // Score a single (term, field) contribution.
    // tf: term frequency of this term in this field
    // doc_freq: total number of docs containing this term (from TermDict)
    // field_length: this document's field length (from .fwd)
    // field_index: which field this score is for
    virtual double term_score(uint32_t tf, uint32_t doc_freq,
                              uint32_t field_length,
                              uint8_t field_index) const = 0;

    // Combine per-term scores for a document into final score
    virtual double combine(absl::Span<const double> term_scores) const = 0;

    virtual double field_weight(uint8_t field_index) const = 0;
};

class BM25FScorer : public Scorer {
public:
    void init(const ReaderStatistics& stats, const BM25Params& params) override;

    double term_score(uint32_t tf, uint32_t doc_freq,
                      uint32_t field_length,
                      uint8_t field_index) const override;

    double combine(absl::Span<const double> term_scores) const override {
        // Sum of weighted term scores
        double total = 0.0;
        for (auto s : term_scores) total += s;
        return total;
    }

    double field_weight(uint8_t fi) const override {
        return params_.fields[fi].weight;
    }

private:
    double idf(uint32_t doc_freq) const {
        // IDF(qi) = ln((N - n(qi) + 0.5) / (n(qi) + 0.5) + 1)
        return std::log((total_docs_ - doc_freq + 0.5) /
                        (doc_freq + 0.5) + 1.0);
    }

    BM25Params params_;
    uint64_t total_docs_;
    double avgdl_;
    std::vector<double> field_avg_lengths_;
};
```

BM25F formula per term per field:

```
score(term, doc, field) = weight[field]
  × IDF(doc_freq)
  × tf × (k1 + 1)
  / (tf + k1 × (1 - b + b × field_length / field_avg_length))
```

## 14. WAL (Write-Ahead Log)

### 14.1 Binary Format

```
WAL Record:
┌──────────┬──────────┬──────────┬───────────┬───────────┬──────────┐
│ CRC32    │ op_type  │ body_len │ id_len    │ external  │ payload  │
│ (4B)     │ (1B)     │ (4B)     │ (2B)      │ _id       │ (var)    │
└──────────┴──────────┴──────────┴───────────┴───────────┴──────────┘

op_type: 0x01 = ADD, 0x02 = REMOVE

ADD payload:
  field_count : uint16
  For each field:
    name_len   : uint16
    name       : char[name_len]
    value_len  : uint32
    value      : char[value_len]

REMOVE: no payload (external_id in header is sufficient)
```

### 14.2 API

```cpp
class WAL {
public:
    explicit WAL(const std::string& path);

    Status append_add(uint32_t internal_id, std::string_view external_id,
                       const Document& doc);
    Status append_remove(std::string_view external_id);

    // Force fsync (called on group commit timer or threshold)
    Status sync();

    // Truncate after successful flush
    Status truncate();

    // Crash recovery: replay all records since last truncation
    struct RecoveryState {
        uint32_t next_internal_id;
        // map: internal_id → (external_id, Document)
        absl::flat_hash_map<uint32_t, std::pair<std::string, Document>> active_docs;
        absl::flat_hash_set<std::string> removed_ids;
    };
    Result<RecoveryState> recover(const Schema& schema);

    // Statistics
    uint64_t bytes_written() const;
    uint32_t record_count() const;

private:
    int fd_;
    std::mutex mutex_;
    std::vector<uint8_t> write_buf_;
    static constexpr uint32_t kMaxBufferSize = 64 * 1024;   // 64 KB
    static constexpr uint32_t kMaxSyncIntervalMs = 10;      // 10 ms
    std::chrono::steady_clock::time_point last_sync_;
};
```

### 14.3 Group Commit

```
append():
  1. Serialize record to write_buf_
  2. If write_buf_.size() >= kMaxBufferSize OR
        time_since_last_sync >= kMaxSyncIntervalMs:
       → write(fd_, write_buf_)
       → fdatasync(fd_)
       → write_buf_.clear()
       → last_sync_ = now
```

### 14.4 Recovery Protocol

```
recover():
  1. If wal.log doesn't exist or is empty → return empty state
  2. Validate each record's CRC32 sequentially
  3. If CRC32 mismatch → truncate at last valid record, log warning
  4. Replay: apply ADD/REMOVE records to in-memory state
  5. Return RecoveryState

On IndexWriter::open():
  1. Read segments.manifest → load committed segments
  2. Delete any *.tmp files (partial flush from crash)
  3. Call WAL::recover() → rebuild memory segment state
  4. Set next_internal_id = recovery.next_internal_id
  5. WAL::truncate()  // WAL replayed, safe to start fresh
```

## 15. Data Integrity

### 15.1 File Trailer

Every data file (.fst, .doc, .store, .fwd, .idm) ends with:

```
┌────────────────────┬────────────────┐
│  File Body         │  xxHash64 (8B) │
└────────────────────┴────────────────┘
```

Validation on load:
```cpp
Result<std::unique_ptr<MmapHandle>> open_and_verify(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    struct stat st;
    fstat(fd, &st);

    void* addr = mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);

    uint64_t expected = read_u64_le(addr, st.st_size - 8);
    uint64_t actual = xxhash64(addr, st.st_size - 8);

    if (expected != actual) {
        munmap(addr, st.st_size);
        close(fd);
        return Status::ChecksumMismatch(
            absl::StrCat(path, ": expected=", expected, " actual=", actual));
    }
    return std::make_unique<MmapHandle>(fd, st.st_size, addr);
}
```

### 15.2 Manifest as Source of Truth

`segments.manifest` is the authoritative list of committed segments. Crash recovery cleans any file not referenced by the manifest (or a .tmp file).

## 16. File Organization

```
index_dir/
├── wal.log                ← binary WAL (recreated on each open)
├── segments.manifest      ← authoritative segment list + checksums (JSON)
├── _0.fst                 ← Segment 0 FST dictionary
├── _0.doc                 ← Segment 0 posting data
├── _0.store               ← Segment 0 document store
├── _0.fwd                 ← Segment 0 forward index
├── _0.idm                 ← Segment 0 external ID mapping
├── _0.meta                ← Segment 0 metadata
├── _0.del                 ← Segment 0 deletion bitmap
├── _1.fst / _1.doc / ...
└── ...
```

Temporary files (during flush): `_N.*.tmp` — cleaned on crash recovery.

### 16.1 segments.manifest

```json
{
  "version": 1,
  "checksum_algorithm": "xxhash64",
  "segments": [
    {
      "id": 0,
      "doc_count": 5123,
      "files": {
        "_0.fst":  {"size": 1234567, "xxhash64": "a1b2c3d4e5f6a7b8"},
        "_0.doc":  {"size": 4567890, "xxhash64": "b2c3d4e5f6a7b8c9"},
        "_0.store":{"size": 1024000, "xxhash64": "c3d4e5f6a7b8c9d0"},
        "_0.fwd":  {"size": 81968,   "xxhash64": "d4e5f6a7b8c9d0e1"},
        "_0.idm":  {"size": 102400,  "xxhash64": "e5f6a7b8c9d0e1f2"},
        "_0.meta": {"size": 256,     "xxhash64": "f6a7b8c9d0e1f2a3"}
      }
    },
    {"id": 1, "doc_count": 2048, "files": {...}}
  ]
}
```

### 16.2 Segment Metadata (_N.meta)

```json
{
  "segment_id": 0,
  "doc_count": 5123,
  "total_terms": 152340,
  "avgdl": 29.7,
  "field_stats": {
    "title":   {"total_terms": 26639, "avg_length": 5.2},
    "content": {"total_terms": 125701, "avg_length": 24.5}
  },
  "schema": {
    "fields": [
      {"name": "title",   "type": "TEXT",    "stored": true,  "indexed": true},
      {"name": "content", "type": "TEXT",    "stored": false, "indexed": true},
      {"name": "doc_id",  "type": "KEYWORD", "stored": true,  "indexed": false}
    ]
  }
}
```

## 17. AND / OR Intersection Algorithms

### 17.1 AND

```
intersect(readers, topk, scorer, fwd_index, del_bitmaps):
  1. Sort readers by doc_freq ascending (shortest first)
  2. driver = readers[0]; others = readers[1..]

  3. For each posting in driver:
       doc_id = posting.doc_id

       // Check deletion
       if del_bitmaps.is_deleted(doc_id): continue

       // Try to find doc_id in all other readers
       found = true
       all_tfs = [driver.tf]
       for each r in others:
         if !r.advance_to(doc_id, matched_doc, matched_tf):
           found = false; break
         all_tfs.append(matched_tf)

       if found:
         // Look up document info from forward index
         doc_info = fwd_index.get(doc_id)
         // Compute BM25F across all field hits
         score = scorer.combine(scorer.term_score(all_tfs[i], ...))
         if heap.size() < topk or score > heap.min():
           heap.push({doc_id, score})

  4. Optional early termination:
       max_remaining_score = scorer.max_possible_term_contribution(driver.next_doc_id)
       if heap.size() == topk and heap.min() > max_remaining_score:
         break
```

### 17.2 OR

```
union_merge(readers, topk, scorer, fwd_index, del_bitmaps):
  1. Min-heap of (reader_idx, doc_id, tf)
  2. While heap not empty:
       // Pop all entries with same doc_id
       doc_id = heap.min().doc_id
       all_tfs = []
       while heap.min().doc_id == doc_id:
         entry = heap.pop()
         all_tfs.append({entry.reader_idx, entry.tf})
         advance entry.reader; if not exhausted: heap.push

       // Check deletion
       if del_bitmaps.is_deleted(doc_id): continue

       // Score
       doc_info = fwd_index.get(doc_id)
       score = scorer.combine(...)
       push topk_heap

  3. Early termination: same as AND
```

## 18. Observability

Baked in from day one. No after-the-fact instrumentation.

```cpp
// Thread-safe counters using std::atomic
struct AtomicCounter {
    void inc() { val_.fetch_add(1, std::memory_order_relaxed); }
    void add(uint64_t n) { val_.fetch_add(n, std::memory_order_relaxed); }
    uint64_t get() const { return val_.load(std::memory_order_relaxed); }
private:
    std::atomic<uint64_t> val_{0};
};

// Fixed-bucket integer histogram, no allocation after construction
class Histogram {
public:
    Histogram(std::vector<uint64_t> bucket_upper_bounds_us);

    void record(uint64_t value_us);
    uint64_t count() const;
    double percentile(double p) const;  // p in [0, 100]
    uint64_t min() const;
    uint64_t max() const;
    double avg() const;

private:
    std::vector<uint64_t> upper_bounds_;
    std::vector<std::atomic<uint64_t>> buckets_;
    AtomicCounter total_count_;
    AtomicCounter total_sum_;
    std::atomic<uint64_t> min_{UINT64_MAX};
    std::atomic<uint64_t> max_{0};
};

struct IndexStats {
    // Index state
    AtomicCounter total_docs;
    AtomicCounter total_terms;
    AtomicCounter segment_count;
    AtomicCounter memory_bytes;      // active arena usage
    AtomicCounter disk_bytes;        // sum of all segment file sizes

    // WAL
    AtomicCounter wal_bytes;
    AtomicCounter wal_syncs;

    // Write throughput
    AtomicCounter docs_added;
    AtomicCounter docs_removed;
    AtomicCounter flushes;
    Histogram flush_latency_ms{     {10, 50, 100, 500, 1000, 5000} };
    Histogram doc_add_latency_us{   {10, 50, 100, 500, 1000, 5000} };
};

struct QueryStats {
    AtomicCounter queries;
    Histogram query_latency_ms{     {1, 5, 10, 25, 50, 100, 250, 500} };
    Histogram segments_queried{     {1, 2, 5, 10, 25, 50} };
    Histogram docs_scored{          {10, 100, 1000, 10000, 100000} };
    Histogram posting_bytes_read{   {1024, 16384, 65536, 262144, 1048576} };
};
```

## 19. Query Model

```cpp
enum class QueryType : uint8_t {
    TERM,
    AND,
    OR,
    NOT,   // NOT queries run as AND minus the negated sub-query
};

struct Query {
    QueryType type;
    std::string term;               // only for TERM
    std::vector<Query> sub_queries; // for AND / OR / NOT

    static Query Term(std::string t) {
        return {QueryType::TERM, std::move(t), {}};
    }
    static Query And(std::vector<Query> subs) {
        return {QueryType::AND, {}, std::move(subs)};
    }
    static Query Or(std::vector<Query> subs) {
        return {QueryType::OR, {}, std::move(subs)};
    }
    static Query Not(Query q) {
        return {QueryType::NOT, {}, {std::move(q)}};
    }
};

struct ScoredDoc {
    float score;
    std::string external_id;     // resolved from .idm
    // internal fields for debugging
    uint64_t segment_id;
    uint32_t internal_doc_id;
};

struct SearchResult {
    std::vector<ScoredDoc> docs;
    uint64_t total_hits;         // total matching docs (for pagination)
    uint64_t elapsed_us;
};
```

## 20. API Entry

```cpp
namespace vortex {

struct IndexWriterOptions {
    std::string index_dir;
    Schema schema;
    uint32_t ram_buffer_mb = 64;     // flush threshold
    uint32_t wal_sync_interval_ms = 10;
    BM25Params bm25_params;

    // External ID field name (KEYWORD stored field, required)
    std::string external_id_field = "doc_id";
};

// Open or create an index
Result<std::unique_ptr<IndexWriter>> IndexWriter::open(IndexWriterOptions opts);

// ── Write API ──

// Add a document. Returns Status; document is crash-safe after return.
Status IndexWriter::add_document(const Document& doc);

// Remove by external ID.
Status IndexWriter::remove_document(std::string_view external_id);

// Atomic remove-then-add.
Status IndexWriter::update_document(std::string_view external_id,
                                     const Document& new_doc);

// Force flush of the in-memory segment to disk.
Status IndexWriter::flush();

// Get a point-in-time reader.
Result<std::shared_ptr<IndexReader>> IndexWriter::get_reader();

// Statistics.
const IndexStats& IndexWriter::stats() const;

// ── Read API ──

// Search with boolean query, return topk results.
Result<SearchResult> IndexReader::search(const Query& query, size_t topk = 10);

// Get a single document by external ID (stored field lookup).
Result<std::optional<Document>> IndexReader::get_document(
    std::string_view external_id);

// Statistics.
const QueryStats& IndexReader::stats() const;

}  // namespace vortex
```

## 21. Code Directory

```
include/vortex/core/
├── status.h             // Status, Result<T>
├── arena.h              // Arena, ScopedThreadArena
├── document.h           // Document, FieldValue
├── schema.h             // Schema, FieldSchema, FieldType
├── query.h              // Query, QueryType
├── search_result.h      // ScoredDoc, SearchResult
├── stats.h              // IndexStats, QueryStats, Histogram, AtomicCounter
├── checksum.h           // xxHash64 wrapper
└── types.h              // u32, u64, etc.

include/vortex/inverted/
├── index_writer.h       // IndexWriter, IndexWriterOptions
├── index_reader.h       // IndexReader
├── segment.h            // Segment (immutable, disk-backed), MemorySegment (mutable)
├── segment_list.h       // SegmentList, SegmentsSnapshot (RCU)
├── term_dict.h          // TermDict, TermDictBuilder, TermInfo
├── posting_codec.h      // SIMD-BP128 codec, runtime dispatch declarations
├── posting_list.h       // PostingListBuilder, PostingListReader
├── skip_list.h          // SkipList
├── forward_index.h      // ForwardIndex, ForwardIndexBuilder, DocInfo
├── external_id_map.h    // ExternalIdMap, IdMapping (per-segment .idm)
├── delete_bitmap.h      // DeleteBitmap (Roaring Bitmap wrapper)
├── scorer.h             // Scorer (interface), BM25FScorer
├── analyzer.h           // Analyzer
├── tokenizer.h          // Tokenizer, StandardTokenizer, CJKBigramTokenizer,
                         // MixedTokenizer, TokenConsumer
├── filter.h             // TokenFilter, LowercaseFilter, StopwordFilter, NFKCFilter
├── wal.h                // WAL
└── unicode.h            // nfkc_normalize, lowercase_ascii

src/inverted/
├── index_writer.cpp
├── index_reader.cpp
├── segment.cpp
├── segment_list.cpp
├── term_dict.cpp
├── term_dict_builder.cpp    // FST incremental minimization
├── posting_codec.cpp        // SSE4.2 baseline
├── posting_codec_avx2.cpp   // AVX2 accelerated (compiled with -mavx2)
├── posting_codec_dispatch.cpp // runtime dispatch setup
├── posting_list.cpp
├── skip_list.cpp
├── forward_index.cpp
├── external_id_map.cpp
├── delete_bitmap.cpp
├── scorer.cpp
├── analyzer.cpp
├── tokenizer.cpp
├── filter.cpp
├── wal.cpp
└── unicode.cpp
```

## 22. Build & CI Requirements

### 22.1 Compiler Policy

```
Minimum:      GCC 13+, Clang 18+
Recommended:  GCC 14+, Clang 20+
SIMD baseline: SSE4.2 (x86-64 mandatory)
SIMD optional: AVX2 (runtime dispatch, separate compilation unit)
```

### 22.2 CMake Configuration

```cmake
# Strict warnings
target_compile_options(vortex PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -Wno-unused-parameter
)

# Compile posting_codec_avx2.cpp with AVX2
if(VORTEX_USE_AVX2)
    set_source_files_properties(
        src/inverted/posting_codec_avx2.cpp
        PROPERTIES COMPILE_FLAGS "-mavx2"
    )
endif()

# Sanitizer builds
option(VORTEX_ASAN ON)
option(VORTEX_UBSAN ON)

# Optional ICU
option(VORTEX_USE_ICU OFF)

# Link dependencies
# absl::flat_hash_map, absl::InlinedVector
find_package(absl REQUIRED)
target_link_libraries(vortex PUBLIC absl::flat_hash_map absl::inlined_vector)

# Roaring Bitmap
include(FetchContent)
FetchContent_Declare(roaring GIT_REPOSITORY ... GIT_TAG v4.0.0)
target_link_libraries(vortex PRIVATE roaring)
```

### 22.3 CI Matrix

```
Matrix:
  compiler:  [gcc-13, gcc-14, clang-18, clang-20]
  build_type: [Debug, Release, ASan+UBSan]

Jobs per matrix cell:
  1. cmake -B build -DCMAKE_BUILD_TYPE=$TYPE -DVORTEX_BUILD_TESTS=ON
  2. cmake --build build -j$(nproc)
  3. cd build && ctest --output-on-failure -j$(nproc)

Standalone jobs:
  - clang-tidy (strict, no warnings allowed)
  - clang-format --dry-run -Werror
  - Coverage (Debug + coverage flags → lcov → > 85% line coverage)
  - Benchmark (Release → run benchmarks, compare with previous results)

Pre-commit hooks:
  - clang-format on staged files
  - clang-tidy on staged files (changed lines only)
```

## 23. Implementation Order

| Phase | Content | Files | Deps | Est. |
|-------|---------|-------|------|------|
| 0 | Infrastructure: Status, Arena, Checksum, Stats, Types | core/*.h | — | 2d |
| 1 | Unicode + Analyzer + Tokenizer + Filters | unicode, analyzer, tokenizer, filter | 0 | 3d |
| 2 | Posting Codec (SSE4.2 baseline + AVX2) + SkipList | posting_codec*, skip_list | 0 | 3d |
| 3 | FST dictionary build + query | term_dict* | 0 | 5d |
| 4 | ForwardIndex + ExternalIdMap + DeleteBitmap | forward_index, external_id_map, delete_bitmap | 0 | 3d |
| 5 | Segment (memory + disk, atomic flush, mmap) | segment | 3, 4 | 4d |
| 6 | BM25F Scorer | scorer | 4 | 2d |
| 7 | WAL (binary + group commit + recovery) | wal | 1 | 2d |
| 8 | SegmentList (RCU) | segment_list | 5 | 2d |
| 9 | IndexWriter + IndexReader integration | index_writer, index_reader | 5, 6, 7, 8 | 3d |
| 10 | Unit tests (per module, parallel with above) | tests/ | per-phase | ongoing |
| 11 | Benchmark + perf tuning | benchmarks/ | 9 | 3d |
| 12 | CI/CD pipeline | .github/ | — | 1d |

Phase dependency graph:
```
0 ──→ 1 ──→ 7
  ──→ 2 ──→ 5 ──→ 9
  ──→ 3 ──→ 5
  ──→ 4 ──→ 5, 6
               8 ──→ 9
```

Phases 1/2/3/4 are parallelizable. Phase 5 is the first integration point. Phase 9 is the final integration.

## 24. Design Principles

1. **Data structures define the performance ceiling.** FST, SIMD-BP128, SkipList, Roaring — each choice is validated by industry benchmarks. No "fix it later" data structures.

2. **Allocation is explicit and controllable.** Arena hierarchy ensures no malloc in hot paths. Memory budget is predictable.

3. **Zero-copy reads.** mmap + FST direct mapping. Query path never copies posting data into intermediate buffers.

4. **Data safety by default.** Every file carries a checksum. Crash recovery is validated end-to-end. Atomic flush (tmp → rename) prevents partial writes from corrupting committed state.

5. **Observable from day one.** Histograms, counters, and statistics are built into the first commit, not retrofitted.

6. **API returns errors, never throws.** `Status` / `Result<T>` at every boundary. Only logic bugs trigger PANIC.

7. **Runtime dispatch for SIMD.** One binary for all x86-64 CPUs. Optimal path chosen at init based on CPU capabilities.

8. **Delete handling is a first-class concern.** External ID mapping enables O(1) delete lookup. `.del` bitmaps integrate with the query path from day one.

9. **Single writer, multiple readers.** Matching the industry standard model (Lucene, Tantivy). Multi-writer scenarios use external sharding.
