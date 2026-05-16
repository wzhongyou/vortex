# Inverted Index V1 Design

## 1. Goals & Scope

Implement a production-grade single-node inverted index. The core architecture is modeled after Lucene's Segment design, with data structures and algorithms designed from day one for production deliverables.

### 1.1 V1 Deliverables

- Term / AND / OR / NOT boolean queries
- BM25F field-weighted relevance scoring, pluggable Scorer
- Per-field indexing
- Segment architecture: in-memory writes → flush to immutable on-disk segments
- Snapshot isolation: RCU-protected read path, writes never block reads
- Binary WAL, CRC32 checksum, Group Commit
- mmap zero-copy disk reads
- FST dictionary + SIMD-BP128 block-encoded posting lists
- Arena-based memory allocation, fully controllable
- Checksums on all disk files
- Observable query latency and indexing throughput

### 1.2 V1 Out of Scope

Segment merging, phrase queries, NRT (near-real-time) search, distributed.

## 2. Error Handling Model

A production search engine cannot tolerate exceptions everywhere. A layered error strategy is used:

```cpp
// Library code never throws; uses Status return
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

    static Status OK() { return Status{OK, ""}; }
    static Status InvalidArgument(const std::string& msg) { return {INVALID_ARGUMENT, msg}; }
    static Status IOError(const std::string& msg) { return {IO_ERROR, msg}; }
    static Status ChecksumMismatch(const std::string& msg) { return {CHECKSUM_MISMATCH, msg}; }
    static Status CorruptIndex(const std::string& msg) { return {CORRUPT_INDEX, msg}; }

private:
    Code code_;
    std::string msg_;  // small-string optimization, no extra allocation
};

// VORTEX_PANIC only for truly unrecoverable errors
#define VORTEX_PANIC(msg) \
    do { std::cerr << "[PANIC] " << msg << std::endl; std::abort(); } while(0)
```

Rules:
- API boundaries: return `Status` or `Result<T>`, never throw
- Internal helpers: `VORTEX_PANIC` is allowed for invariant violations (logic bugs, not runtime errors)
- Constructors: use `VORTEX_PANIC` only; factory methods use `Result<T>`

## 3. Memory Management

### 3.1 Arena Allocator

During indexing, 99% of allocations are temporary (tokens, in-memory segment postings, FST build intermediates). Using `new/malloc` directly leads to allocator lock contention and fragmentation.

```
Strategy: Layered Arenas

IndexWriter owns:
  active_arena_       ← used by current in-memory segment, bulk-reset after flush
  flush_arena_        ← segment being flushed (double-buffered to avoid blocking writes)

Each request binds a ThreadLocalArena (256KB slab chain growth):
  - Analyzer token output allocated here
  - bulk-reset on request completion

FST construction uses an independent 2-pass Arena:
  - Pass 1: collect all terms → build minimal FST node graph
  - Pass 2: serialize to byte array, free pass 1 temporary data
```

```cpp
class Arena {
public:
    explicit Arena(size_t chunk_size = 256 * 1024);

    void* allocate(size_t size, size_t alignment = alignof(std::max_align_t));
    void reset();  // retain current chunk, reset usage pointer

    size_t allocated() const;
    size_t wasted() const;

private:
    struct Chunk {
        std::unique_ptr<char[]> data;
        size_t size;
        size_t used;
        Chunk* next;
    };
    Chunk* head_;
    Chunk* current_;
    size_t chunk_size_;
};

// RAII binding of arena to current thread
class ScopedThreadArena {
public:
    ScopedThreadArena(Arena& arena);   // bind
    ~ScopedThreadArena();              // auto unbind + reset
};
```

### 3.2 Disk File Preallocation

```cpp
// .doc / .fst / .store files are fallocated before writing
// avoids fragmentation and inode update overhead from incremental appends
void prealloc_file(int fd, off_t estimated_size);
```

## 4. Data Model

### 4.1 Schema

```cpp
enum class FieldType : uint8_t {
    TEXT,       // full-text indexed + tokenized
    KEYWORD,    // exact match, no tokenization
};

struct FieldSchema {
    std::string name;
    FieldType type;
    bool stored;     // raw value stored
    bool indexed;    // added to inverted index
};

struct Schema {
    Status add_field(FieldSchema field);
    const FieldSchema* field(std::string_view name) const;

    std::vector<FieldSchema> fields;
    // O(1) field_index lookup by name
    absl::flat_hash_map<std::string, uint16_t> name_to_index;
};
```

### 4.2 Document

```cpp
struct FieldValue {
    std::string name;
    std::string value;  // V1: text only
};

struct Document {
    std::vector<FieldValue> fields;
};
```

Documents carry no external ID. IDs are assigned internally by IndexWriter (dense, monotonically increasing uint32). External IDs are managed by the application via a KEYWORD stored field.

### 4.3 Internal Document Representation

```cpp
struct InternalDoc {
    uint64_t segment_id;   // segment id
    uint32_t doc_id;       // dense id within segment (0, 1, 2, ...)
    uint32_t doc_length;   // total term count
    // field_index → term count
    uint32_t field_lengths[/*field_count*/];
    // stored field values, indexed by field_index
    std::string_view stored_values[/*stored_field_count*/];
};
```

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
│         │ flush                                       │
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
        └───────────────────────┘
```

### 5.2 Segment On-Disk Files

```
Segment N
├── _N.fst       FST dictionary: term → {doc_freq, posting_offset, posting_len}
├── _N.doc       Posting data: [block_header][SIMD-packed delta_doc_ids][freqs]
├── _N.pos       Position data (V2 phrase queries)
├── _N.store     Row-oriented doc store: doc_id → stored_fields
├── _N.fwd       Forward index: doc_id → field_lengths, doc_length
├── _N.meta      JSON metadata
└── _N.del       Deletion bitmap (roaring bitmap)
```

Each data file ends with an 8-byte xxHash64 checksum. On load, files are validated and rejected on mismatch with a CORRUPT_INDEX error.

### 5.3 Write Path

```
add_document(doc):
  1. Allocate segment-local doc_id
  2. WAL::append_add(internal_id, doc)  ← WAL first
  3. For each TEXT field: Tokenizer → [TokenFilter...] → collect tf
  4. Write into in-memory FST Builder and PostingListBuilder
  5. Update forward-index info
  6. Update global statistics (avgdl, etc.)
  7. In-memory segment reaches threshold → flush()

flush():
  1. Estimate file sizes, fallocate
  2. Write .fst / .doc / .store / .fwd / .meta
  3. Write xxHash64 tail per file
  4. fsync all files
  5. Atomically register new segment with SegmentManager (RCU update)
  6. WAL::truncate()
  7. Release active_arena_ → create new empty in-memory segment
```

### 5.4 Read Path (Lock-Free)

```
IndexReader::open():
  1. Acquire segments snapshot from SegmentManager (RCU load)
  2. mmap each segment's .fst and .doc (on first use)
  3. Validate file header magic + tail xxHash64

IndexReader::search(query, topk):
  1. Execute per segment independently: intra-segment query + scoring
  2. Cross-segment k-way merge via min-heap (topk size)
  3. Return global topk, each result carrying segment_id + intra-segment doc_id
```

## 6. Concurrency Model

### 6.1 RCU-Protected Segment List

`std::shared_mutex` + `vector<shared_ptr<Segment>>` has a problem: every `snapshot()` requires copying the vector + atomic inc/dec, which is not viable under high QPS.

Switch to an RCU style:

```cpp
class SegmentList {
public:
    // Read path: zero-lock, just load(acquire)
    // Returns raw pointer; lifetime guaranteed by epoch-based reclamation
    const Segments* snapshot() const {
        return head_.load(std::memory_order_acquire);
    }

    // Write path: copy → modify → atomic swap
    // Old version deallocated lazily (after all readers exit current epoch)
    void register_segment(std::shared_ptr<const Segment> seg);

private:
    struct Segments {
        uint32_t ref_count{0};  // active reader count (epoch tracking)
        std::vector<std::shared_ptr<const Segment>> segments;
    };
    std::atomic<Segments*> head_;

    // Retired list for deferred reclamation
    std::mutex retire_mutex_;
    std::vector<Segments*> retired_;
};
```

Read path algorithm:
```
reader_enter_epoch():
  atomic_fetch_add(&head_->ref_count, 1)

reader_search():
  segs = snapshot()  // raw pointer
  for seg in segs->segments: do_search(seg)

reader_exit_epoch():
  atomic_fetch_sub(&head_->ref_count, 1)
```

After a writer swap, the old `Segments` is placed on the retired list. Every N writes or M milliseconds, check retired versions with ref_count == 0 and free them.

### 6.2 Write Lock

```cpp
class IndexWriter {
    std::mutex write_mutex_;  // protects only the in-memory segment and WAL
    // Read path is completely unaffected by this lock
};
```

## 7. Unicode Text Analysis

### 7.1 Normalization

Foundation of search quality: fullwidth → halfwidth, diacritic standardization, case folding.

```cpp
// ICU or built-in compact NFKC table
// Compile-time macro: VORTEX_USE_ICU
std::string nfkc_normalize(std::string_view input);
std::string lowercase(std::string_view input);
```

V1 ships with built-in ASCII lowercase + common fullwidth/halfwidth mapping table (covering symbols, digits, and letters commonly used with Chinese IMEs). Enabling ICU auto-upgrades to full NFKC + locale-aware case folding.

### 7.2 Streaming Tokenizer

Returning `vector<Token>` causes memory spikes for large documents. Use a push-based design instead:

```cpp
class TokenConsumer {
public:
    virtual void on_token(Token token) = 0;
    virtual ~TokenConsumer() = default;
};

class Tokenizer {
public:
    virtual void tokenize(std::string_view text, TokenConsumer& consumer) = 0;
    virtual ~Tokenizer() = default;
};

// Standard Latin tokenization
class StandardTokenizer : public Tokenizer {
    void tokenize(std::string_view text, TokenConsumer& consumer) override;
};

// CJK Bigram
class CJKBigramTokenizer : public Tokenizer {
    void tokenize(std::string_view text, TokenConsumer& consumer) override;
};

// Hybrid: auto-dispatch by Unicode block
class MixedTokenizer : public Tokenizer { ... };
```

### 7.3 Filter Chain

```cpp
class TokenFilter {
public:
    virtual void process(Token token, TokenConsumer& downstream) = 0;
    virtual ~TokenFilter() = default;
};

class LowercaseFilter : public TokenFilter { ... };
class StopwordFilter : public TokenFilter {
    explicit StopwordFilter(
        std::shared_ptr<absl::flat_hash_set<std::string>> stopwords);
};
class NFKCFilter : public TokenFilter { ... };  // normalizes token text
```

### 7.4 Analyzer

```cpp
class Analyzer {
public:
    Analyzer(std::unique_ptr<Tokenizer> tokenizer,
             std::vector<std::unique_ptr<TokenFilter>> filters);

    // Get term list (positions stripped, deduplicated terms + tf only)
    // Results allocated on ScopedThreadArena
    struct TermWithFreq {
        std::string_view term;
        uint32_t tf;
    };
    std::vector<TermWithFreq> analyze(std::string_view text, Arena& arena);

private:
    std::unique_ptr<Tokenizer> tokenizer_;
    std::vector<std::unique_ptr<TokenFilter>> filters_;
};
```

## 8. FST Dictionary

### 8.1 Design

```
Term Dictionary (FST):
  Input: term → Output: {df: uint32, posting_offset: uint64, posting_len: uint32}

  Characteristics:
  - Byte-based FST, one byte per arc label
  - Automatic prefix/suffix sharing (10-30x memory savings vs. hash map)
  - Natural lexicographic ordering, O(k) prefix queries
  - Incremental minimal construction (Lucene Builder algorithm)
```

### 8.2 API

```cpp
class TermDictBuilder {
public:
    // Must insert in lexicographic order
    void insert(std::string_view term, uint32_t doc_freq,
                uint64_t posting_offset, uint32_t posting_len);
    // Finalize construction
    void finish();

    // Serialize to file (writes fst_data + tail xxHash64)
    Status write_file(const std::string& path);

private:
    // Internal state for incremental FST minimization
    // Maintains a prefix stack of unfrozen nodes
};

class TermDict {
public:
    struct TermInfo {
        uint32_t doc_freq;
        uint64_t posting_offset;
        uint32_t posting_len;
    };

    // Construct from mmap data (zero-copy, no data duplication)
    static Result<std::unique_ptr<TermDict>> from_mmap(
        const uint8_t* data, size_t len);

    // Exact lookup
    const TermInfo* lookup(std::string_view term) const;

    // Prefix range scan
    void prefix_range(std::string_view prefix,
                      std::function<void(std::string_view, const TermInfo&)> fn) const;

private:
    const uint8_t* fst_data_;
    size_t fst_len_;
};
```

## 9. Posting List Codec

### 9.1 SIMD-BP128

Fixed block size 128, aligned to SIMD register width (128-bit × 4 = 512 bits). Each 128-wide lane stores the same bit-position across 128 doc deltas.

```
Block encoding (128 docs):

Header (4 bytes):
  [num_docs:8][doc_bits:8][freq_bits:8][flags:8]

Packed Deltas:
  SIMD-BP128: 128 numbers × doc_bits bit-width
  With doc_bits=13: (128*13 + 7)/8 = 208 bytes
  13 bit-planes, each 128-bit aligned

Packed Freqs:
  Same SIMD-BP128, 128 numbers × freq_bits bit-width
```

Decoding uses SSE4.2/AVX2 instructions to unpack 128 doc_ids at once, achieving 3-5x throughput vs. scalar FOR.

```cpp
namespace vortex::codec {

// Returns bytes written
size_t encode_block_simd(const uint32_t* doc_deltas,
                         const uint32_t* freqs,
                         uint8_t num_docs,
                         uint8_t doc_bits,
                         uint8_t freq_bits,
                         uint8_t* output);

// Decodes one block, returns bytes read
// Uses __m128i / __m256i for bit unpack
size_t decode_block_simd(const uint8_t* input,
                         uint32_t* doc_ids_out,
                         uint32_t* freqs_out,
                         uint8_t& num_docs_out);

}  // namespace vortex::codec
```

### 9.2 Block-Level Multi-Level Skip List

```cpp
struct SkipEntry {
    uint32_t last_doc_id;      // max doc_id in the block
    uint64_t block_offset;     // block offset in .doc
};

class SkipList {
public:
    // Build multi-level skip list, skip_interval in blocks, default 4
    void build(const std::vector<uint64_t>& block_offsets,
               const std::vector<uint32_t>& max_doc_per_block,
               int skip_interval = 4);

    // Skip to first block containing >= target, return block offset
    uint64_t skip_to(uint32_t target_doc, uint64_t current_offset) const;

private:
    // Multi-level: level[0] = every 4 blocks, level[1] = every 16 blocks, etc.
    std::vector<std::vector<SkipEntry>> levels_;
};
```

### 9.3 PostingList Construction & Reading

```cpp
class PostingListBuilder {
public:
    void append(uint32_t doc_id, uint32_t term_freq);
    // Returns {offset_in_doc_file, byte_count}
    // Also writes skip list structure to footer_with_skip
    Result<std::pair<uint64_t, uint32_t>> flush(
        int fd, Arena& scratch);

    size_t doc_freq() const;

private:
    uint32_t doc_buf_[128];
    uint32_t freq_buf_[128];
    uint8_t  count_;
    std::vector<uint64_t> block_offsets_;
    std::vector<uint32_t> block_max_doc_;
    size_t total_docs_;
};

class PostingListReader {
public:
    PostingListReader(const uint8_t* data, size_t len,
                      const SkipList& skip);

    // Iterate all postings + score
    template<typename Fn>
    void for_each(Fn&& fn) const;

    // advance_to: skip to first posting >= target
    // Uses skip list to jump over whole blocks, linear scan within block
    bool advance_to(uint32_t target, uint32_t& doc_id, uint32_t& tf) const;

    size_t doc_freq() const;

private:
    const uint8_t* data_;
    size_t len_;
    const SkipList* skip_;
    size_t block_count_;
};
```

### 9.4 Sparse Posting List Optimization

High-frequency terms (appearing in > 10% of docs) can have posting lists in the millions:

- IDF for such terms is extremely low; contribution to final score is minimal
- In AND intersection, these "long lists" are processed last
- Marked with `SPARSE_OPT` flag; score computation uses approximate IDF (global stats instead of exact doc_freq)
- `advance_to` supports early termination: if current doc's BM25 contribution is below threshold, skip

## 10. AND/OR Intersection Algorithms

### 10.1 AND Conjunction

```
intersect(readers, topk):
  1. Sort readers by doc_freq ascending
  2. Drive with shortest reader
  3. for each posting in readers[0]:
       doc = posting.doc_id
       ok = true
       for each r in readers[1..]:
         if !r.advance_to(doc, ...): ok = false; break
       if ok:
         score = bm25f(doc, all_hit_tfs, field_lengths)
         push heap(score, doc)
  4. Optional early termination:
     If min score in heap topk > max possible contribution of remaining postings → exit
```

### 10.2 OR Disjunction

```
union_merge(readers, topk):
  min_heap of (reader_idx, doc_id, tf)
  while heap not empty:
    pop all entries with same min doc_id
    collect per-field tf from all matching readers
    score = bm25f(doc, tfs, field_lengths)
    push topk_heap(score, doc)
    advance each emptied reader, push back if not exhausted
```

## 11. BM25F Scoring

### 11.1 Field-Weighted BM25

Plain BM25 treats title and body equally, which is unreasonable. BM25F uses per-field parameter linear combination:

```cpp
struct FieldBM25Params {
    double k1 = 1.2;
    double b = 0.75;
    double weight = 1.0;  // field weight
};

struct BM25Params {
    std::vector<FieldBM25Params> fields;  // per field_index
};

class BM25Scorer {
public:
    BM25Scorer(const BM25Params& params,
               uint64_t total_docs,
               const std::vector<double>& field_avg_lengths);

    // Single term, single field score contribution
    double score(uint32_t tf, uint32_t doc_freq,
                 uint32_t field_length, uint8_t field_index) const;

    // Final doc score = Σ over terms Σ over fields
    //   field_weight[f] * IDF(term) * tf_norm(tf, field_len, k1_f, b_f)
    double combine(std::vector<double> per_field_term_scores) const;

    double idf(uint32_t doc_freq) const;

private:
    BM25Params params_;
    uint64_t total_docs_;
    std::vector<double> field_avg_lengths_;
};
```

### 11.2 Pluggable Scorer

```cpp
class Scorer {
public:
    virtual ~Scorer() = default;

    // Initialize (bind to segment statistics)
    virtual Status init(uint64_t total_docs,
                        const std::vector<double>& field_avg_lengths) = 0;

    // Single term-field contribution
    virtual double term_score(uint32_t tf, uint32_t doc_freq,
                              uint32_t field_length,
                              uint8_t field_index) const = 0;

    // Combination
    virtual double combine(
        absl::Span<const double> term_scores) const = 0;

    // Field weight
    virtual double field_weight(uint8_t field_index) const = 0;
};

// Default implementation
class BM25FScorer : public Scorer { ... };

// V2 extensibility: LTR, LambdaMART
```

## 12. WAL (Write-Ahead Log)

### 12.1 Binary Format

JSON line format is unacceptable for throughput. Use a binary format instead:

```
WAL Record:
┌──────────┬──────────┬──────────┬────────────┬──────────┬──────────┐
│ CRC32    │ op_type  │ length   │ key_len    │ key      │ value    │
│ (4 bytes)│ (1 byte) │ (4 bytes)│  (4 bytes) │ (var)    │ (var)    │
└──────────┴──────────┴──────────┴────────────┴──────────┴──────────┘

op_type: 0x01 = ADD, 0x02 = REMOVE
```

### 12.2 Group Commit

```cpp
class WAL {
public:
    WAL(const std::string& path);

    // Append (thread-safe), writes to in-memory buffer, no immediate fsync
    void append_add(uint64_t internal_id, const Document& doc);
    void append_remove(uint64_t internal_id);

    // Force flush to disk (periodic or threshold-triggered)
    Status sync();

    // Truncate after flush
    Status truncate();

    // Crash recovery
    struct RecoveryState {
        uint64_t next_internal_id;
        std::vector<std::pair<uint64_t, Document>> active_docs;
        absl::flat_hash_set<uint64_t> removed_ids;
    };
    Result<RecoveryState> recover();

private:
    int fd_;
    std::mutex mutex_;
    std::vector<uint8_t> write_buf_;  // group commit buffer
    uint32_t max_buf_size_ = 64 * 1024;  // 64KB accumulation threshold
};
```

Group commit policy:
- Write to buffer; when buffer reaches 64KB or 10ms since last sync → fsync
- Crash recovery replays all records (WAL only exists between two flushes, data size < 1GB)

## 13. Data Integrity

### 13.1 File Tail Checksums

All data files (.fst, .doc, .store, .fwd) have an 8-byte tail:

```
┌──────────────────┬──────────────────┐
│  Data            │  xxHash64 (8B)   │
└──────────────────┴──────────────────┘
```

### 13.2 Segment Manifest Checksums

segments.manifest contains a file list + expected checksums per segment:

```json
{
  "version": 1,
  "checksum_algorithm": "xxhash64",
  "segments": [
    {
      "id": 0,
      "doc_count": 5123,
      "files": {
        "_0.fst": {"size": 1234567, "xxhash64": "a1b2c3d4e5f6a7b8"},
        "_0.doc": {"size": 4567890, "xxhash64": "b2c3d4e5f6a7b8c9"},
        "_0.store": {"size": 1024000, "xxhash64": "..."},
        "_0.fwd": {"size": 20480, "xxhash64": "..."},
        "_0.meta": {"size": 256, "xxhash64": "..."}
      }
    }
  ]
}
```

Every file is validated on segment load. Any mismatch rejects the segment and reports CORRUPT_INDEX.

## 14. File Organization

```
index_dir/
├── wal.log              ← binary WAL
├── segments.manifest    ← segment manifest + checksums (JSON)
├── _0.fst / _0.doc / _0.store / _0.fwd / _0.meta / _0.del
├── _1.fst / _1.doc / _1.store / _1.fwd / _1.meta / _1.del
└── ...
```

### 14.1 Segment Metadata

```json
{
  "segment_id": 0,
  "doc_count": 5123,
  "total_terms": 152340,
  "avgdl": 29.7,
  "field_avg_lengths": {"title": 5.2, "content": 123.4},
  "schema": {
    "fields": [
      {"name": "title", "type": "TEXT", "stored": true, "indexed": true},
      {"name": "content", "type": "TEXT", "stored": false, "indexed": true},
      {"name": "doc_id", "type": "KEYWORD", "stored": true, "indexed": false}
    ]
  }
}
```

## 15. Observability

Instrumentation is built-in from day one:

```cpp
struct IndexStats {
    // Index statistics
    uint64_t total_docs;
    uint64_t total_terms;
    uint64_t segment_count;
    uint64_t memory_bytes;     // current in-memory segment usage
    uint64_t disk_bytes;       // total on-disk file size
    double avgdl;

    // WAL
    uint64_t wal_bytes;
    uint32_t wal_sync_count;

    // Writes
    uint64_t docs_added_total;
    uint64_t docs_removed_total;
    uint64_t flush_count;
    Histogram flush_latency_ms;
    Histogram doc_add_latency_us;
};

struct QueryStats {
    // Queries
    uint64_t queries_total;
    Histogram query_latency_ms;     // P50/P90/P99
    Histogram segments_per_query;
    Histogram posting_bytes_read;
    Histogram docs_scored_per_query;
};

// API
const IndexStats& writer->stats() const;
const QueryStats& reader->stats() const;
```

Histogram uses a fixed-bucket integer count array with no dynamic allocation.

## 16. Query Model

```cpp
enum class QueryType : uint8_t { TERM, AND, OR, NOT };

struct Query {
    QueryType type;
    std::string term;               // TERM
    std::vector<Query> sub_queries; // AND/OR/NOT

    // Factory methods
    static Query Term(std::string t);
    static Query And(std::vector<Query> sub);
    static Query Or(std::vector<Query> sub);
    static Query Not(Query q);
};

struct ScoredDoc {
    uint32_t internal_doc_id;
    float score;
    uint64_t segment_id;  // locates the segment
};

struct SearchResult {
    std::vector<ScoredDoc> docs;
    uint64_t total_hits;  // approximate (equals matched doc count)
    uint64_t elapsed_us;
};
```

## 17. API Entry Points

```cpp
namespace vortex {

// Create or open index
Result<std::shared_ptr<IndexWriter>> open_index(
    const std::string& index_dir,
    const Schema& schema);

// Write
writer->add_document(doc);           // → Status
writer->remove_document(doc_id);     // → Status
writer->flush();                     // → Status

// Read (acquire point-in-time snapshot reader)
auto reader = writer->get_reader();  // → shared_ptr<IndexReader>
auto result = reader->search(query, topk);  // → Result<SearchResult>

// Statistics
writer->stats();   // → IndexStats
reader->stats();   // → QueryStats

}  // namespace vortex
```

## 18. Directory Layout

```
include/vortex/core/
├── status.h             // Status, Result<T>
├── arena.h              // Arena, ScopedThreadArena
├── document.h           // Document, FieldValue
├── schema.h             // Schema, FieldSchema, FieldType
├── query.h              // Query, QueryType
├── search_result.h      // ScoredDoc, SearchResult
├── stats.h              // IndexStats, QueryStats, Histogram
├── checksum.h           // xxHash64 wrapper
└── types.h              // fundamental type aliases

include/vortex/inverted/
├── index_writer.h       // IndexWriter
├── index_reader.h       // IndexReader
├── segment.h            // Segment, MemorySegment
├── segment_list.h       // SegmentList (RCU)
├── term_dict.h          // TermDict, TermDictBuilder
├── posting_codec.h      // SIMD-BP128 encode/decode
├── posting_list.h       // PostingListBuilder, PostingListReader
├── skip_list.h          // SkipList
├── scorer.h             // Scorer interface, BM25FScorer
├── analyzer.h           // Analyzer, Tokenizer, TokenFilter
├── tokenizer.h          // StandardTokenizer, CJKBigramTokenizer, MixedTokenizer
├── filter.h             // LowercaseFilter, StopwordFilter, NFKCFilter
├── wal.h                // WAL
└── unicode.h            // NFKC normalize, lowercase

src/inverted/
├── index_writer.cpp
├── index_reader.cpp
├── segment.cpp
├── segment_list.cpp
├── term_dict.cpp
├── term_dict_builder.cpp   // FST incremental minimal construction
├── posting_codec.cpp       // SIMD-BP128 (SSE4.2 + AVX2 dispatch)
├── posting_list.cpp
├── skip_list.cpp
├── scorer.cpp
├── analyzer.cpp
├── tokenizer.cpp
├── filter.cpp
├── wal.cpp
└── unicode.cpp
```

## 19. Build & CI Requirements

### 19.1 Compilers

```
Minimum: GCC 11+, Clang 18+
Recommended: GCC 14+, Clang 20+
SIMD: compile-time detection, SSE4.2 minimum, AVX2 optional acceleration
```

### 19.2 CMake Additions

```cmake
# Strict warnings
target_compile_options(vortex PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -Wno-unused-parameter    # reserved interface parameters
)

# Sanitizer build types
option(VORTEX_ASAN ON)
option(VORTEX_UBSAN ON)

# SIMD options
option(VORTEX_USE_AVX2 ON)
option(VORTEX_USE_ICU OFF)  # optional ICU dependency

# clang-tidy
set(CMAKE_CXX_CLANG_TIDY clang-tidy)
```

### 19.3 CI Pipeline

```
Matrix: {gcc-13, gcc-14, clang-18, clang-20} × {Debug, Release, ASan+UBSan}

Per job:
  cmake -B build -DCMAKE_BUILD_TYPE=$TYPE -DVORTEX_BUILD_TESTS=ON
  cmake --build build -j$(nproc)
  cd build && ctest --output-on-failure

Release extra:
  cd build && ./benchmarks/vortex_index_benchmarks

clang-tidy check (separate job)
clang-format --dry-run check (separate job)
```

### 19.4 Coverage

```cmake
# Coverage build
option(VORTEX_COVERAGE OFF)
if(VORTEX_COVERAGE)
    target_compile_options(vortex PRIVATE --coverage -O0)
    target_link_options(vortex PUBLIC --coverage)
endif()
```

Target: line coverage > 85%.

## 20. Implementation Order

| Phase | Content | Output | Est. Effort |
|-------|---------|--------|-------------|
| 0 | Infrastructure: Status, Arena, Checksum, Stats | All 6 headers in core/ | Baseline |
| 1 | Unicode normalization + Analyzer + Tokenizer | analyzer/, unicode/ | 3d |
| 2 | PostingList Codec (SIMD-BP128) + SkipList | posting_codec, skip_list | 3d |
| 3 | FST dictionary construction + queries | term_dict, term_dict_builder | 5d |
| 4 | Segment (in-memory + on-disk) + serialization | segment | 3d |
| 5 | BM25F Scorer | scorer | 2d |
| 6 | WAL (binary + group commit) | wal | 2d |
| 7 | SegmentList (RCU) | segment_list | 2d |
| 8 | IndexWriter + IndexReader integration | index_writer, index_reader | 3d |
| 9 | Full unit test coverage | tests/ (per module) | Ongoing |
| 10 | Benchmark + perf tuning | benchmarks/ | 3d |
| 11 | CI/CD configuration | .github/workflows/ | 1d |

Dependency chain for phases 1-4: 1 → 2/3 (parallelizable) → 4 (depends on 2+3)
Phases 5-8: 5+6 in parallel → 7 → 8
Phase 9: continuous, write tests as each module completes

## 21. Design Principles Summary

1. **Data structures set the performance ceiling** — FST, SIMD-BP128, SkipList: every choice is benchmark-validated and industry-proven
2. **Controlled allocation** — no bare new/malloc; layered Arena isolation
3. **Zero-copy reads** — mmap + FST direct mapping; query path never copies posting data
4. **Data safety** — every file carries a checksum; validated on load
5. **Observable** — stats instrumentation from day one, not an afterthought
6. **No-throw API** — Status/Result\<T\>, clean error handling
7. **Compile-time dispatch** — SIMD path selected by CPU capability, no runtime branching
