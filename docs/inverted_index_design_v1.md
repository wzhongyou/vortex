# 倒排索引 V1 设计

## 1. 目标与范围

实现工业级单机倒排索引。核心对标 Lucene 的段（Segment）架构，从第一天起以生产可交付标准设计数据结构和算法。

### 1.1 V1 交付

- Term / AND / OR / NOT 布尔查询
- BM25F 字段加权相关性打分，插件化 Scorer
- 按 Field 独立建索引
- 段（Segment）架构：内存写入 → Flush 不可变磁盘段
- 快照隔离：RCU 保护读路径，写不阻塞读
- 二进制 WAL，CRC32 校验，Group Commit
- mmap 零拷贝磁盘读取
- FST 词典 + SIMD-BP128 块编码倒排链
- Arena 内存分配，全链路可控
- 所有磁盘文件带校验和
- 查询延迟、索引吞吐度可观测

### 1.2 V1 不做

段合并、短语查询、NRT 近实时搜索、分布式。

## 2. 错误处理模型

工业搜索引擎不允许异常满天飞。采用分层错误策略：

```cpp
// 库不抛异常，使用 Status 返回
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
    std::string msg_;  // 小字符串优化，不额外分配
};

// 只有真正的 unrecoverable 才用 panic
#define VORTEX_PANIC(msg) \
    do { std::cerr << "[PANIC] " << msg << std::endl; std::abort(); } while(0)
```

规则：
- API 边界：返回 `Status` 或 `Result<T>`，不抛异常
- 内部辅助函数：允许 `VORTEX_PANIC` 处理 invariant 破坏（逻辑 bug，不是运行时错误）
- 构造函数只用 `VORTEX_PANIC`，工厂方法用 `Result<T>`

## 3. 内存管理

### 3.1 Arena Allocator

建索引阶段，99% 的分配是临时的（分词 token、内存段 postings、FST 构建中间态）。直接用 `new/malloc` 会导致分配器锁竞争和碎片。

```
策略：分层 Arena

IndexWriter 拥有:
  active_arena_       ← 内存段当前使用，flush 后整体释放
  flush_arena_        ← 正在 flush 的段（双缓冲，避免 flush 阻塞写入）

每个请求绑定一个 ThreadLocalArena (256KB slab 链式增长)
  - Analyzer 的 token 输出分配在此
  - 请求结束整体释放

FST 构建使用独立的 2-pass Arena：
  - Pass 1: 收集所有 term → 建立 minimal FST 节点图
  - Pass 2: 序列化到 byte array，释放 pass 1 临时数据
```

```cpp
class Arena {
public:
    explicit Arena(size_t chunk_size = 256 * 1024);

    void* allocate(size_t size, size_t alignment = alignof(std::max_align_t));
    void reset();  // 保留当前 chunk，重置使用位置

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

// RAII 绑定 arena 到当前线程
class ScopedThreadArena {
public:
    ScopedThreadArena(Arena& arena);   // 绑定
    ~ScopedThreadArena();              // 自动解绑 + reset
};
```

### 3.2 磁盘文件预分配

```cpp
// .doc / .fst / .store 在开始写入前 fallocate
// 避免逐步 append 导致的文件碎片和 inode 更新开销
void prealloc_file(int fd, off_t estimated_size);
```

## 4. 数据模型

### 4.1 Schema

```cpp
enum class FieldType : uint8_t {
    TEXT,       // 全文索引 + 分词
    KEYWORD,    // 精确匹配，不分词
};

struct FieldSchema {
    std::string name;
    FieldType type;
    bool stored;     // 原始值存储
    bool indexed;    // 建倒排索引
};

struct Schema {
    Status add_field(FieldSchema field);
    const FieldSchema* field(std::string_view name) const;

    std::vector<FieldSchema> fields;
    // 用于 O(1) 按名查 field_index
    absl::flat_hash_map<std::string, uint16_t> name_to_index;
};
```

### 4.2 Document

```cpp
struct FieldValue {
    std::string name;
    std::string value;  // V1 统一文本
};

struct Document {
    std::vector<FieldValue> fields;
};
```

Document 不带外部 id。id 由 IndexWriter 内部分配（密集递增 uint32），外部 id 通过 KEYWORD stored field 自行管理。

### 4.3 内部文档表示

```cpp
struct InternalDoc {
    uint64_t segment_id;   // 段 id
    uint32_t doc_id;       // 段内密集 id（0,1,2,...）
    uint32_t doc_length;   // 总 term 数
    // field_index → term 数
    uint32_t field_lengths[/*field_count*/];
    // stored field values，按 field_index 索引
    std::string_view stored_values[/*stored_field_count*/];
};
```

## 5. 段架构

### 5.1 整体结构

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

### 5.2 Segment 磁盘文件

```
Segment N
├── _N.fst       FST 词典: term → {doc_freq, posting_offset, posting_len}
├── _N.doc       倒排数据: [block_header][SIMD-packed delta_doc_ids][freqs]
├── _N.pos       词位置数据 (V2 短语查询用)
├── _N.store     行式文档存储: doc_id → stored_fields
├── _N.fwd       正排索引: doc_id → field_lengths, doc_length
├── _N.meta      JSON 元信息
└── _N.del       删除位图 (roaring bitmap)
```

每个数据文件尾部写 8 字节 xxHash64 校验和。加载时校验，不匹配则拒绝该段并记录 CORRUPT_INDEX。

### 5.3 写路径

```
add_document(doc):
  1. 分配段内 doc_id
  2. WAL::append_add(internal_id, doc)  ← 先写 WAL
  3. 每个 TEXT field: Tokenizer → [TokenFilter...] → 统计 tf
  4. 写入内存段的 FST Builder 和 PostingListBuilder
  5. 更新正排信息
  6. 更新全局统计（avgdl 等）
  7. 内存段达到阈值 → flush()

flush():
  1. 计算各文件预估大小，fallocate
  2. 写入 .fst / .doc / .store / .fwd / .meta
  3. 每个文件尾部写入 xxHash64
  4. fsync 所有文件
  5. 原子注册新段到 SegmentManager（RCU 更新）
  6. WAL::truncate()
  7. 释放 active_arena_ → 新建空内存段
```

### 5.4 读路径（无锁）

```
IndexReader::open():
  1. 从 SegmentManager 获取 segments snapshot（RCU load）
  2. mmap 各段的 .fst 和 .doc（首次使用时）
  3. 校验文件头 magic + 尾 xxHash64

IndexReader::search(query, topk):
  1. 对每个段独立执行：段内查询 + 打分
  2. 跨段用小顶堆（topk size）多路归并
  3. 返回全局 topk，每结果含 segment_id + 段内 doc_id
```

## 6. 并发模型

### 6.1 RCU 保护段列表

`std::shared_mutex` + `vector<shared_ptr<Segment>>` 的问题：每次 `snapshot()` 需要拷贝 vector + atomic inc/dec。高 QPS 下不可视。

改用 RCU 风格：

```cpp
class SegmentList {
public:
    // 读路径：零锁，仅 load(acquire)
    // 返回裸指针，生命周期由 epoch-based 回收保证
    const Segments* snapshot() const {
        return head_.load(std::memory_order_acquire);
    }

    // 写路径：copy → 修改 → 原子 swap
    // 旧版本延迟释放（等到所有读者离开当前 epoch）
    void register_segment(std::shared_ptr<const Segment> seg);

private:
    struct Segments {
        uint32_t ref_count{0};  // 活跃 reader 数（epoch tracking）
        std::vector<std::shared_ptr<const Segment>> segments;
    };
    std::atomic<Segments*> head_;

    // 退休列表，延迟回收
    std::mutex retire_mutex_;
    std::vector<Segments*> retired_;
};
```

读路径算法：
```
reader_enter_epoch():
  atomic_fetch_add(&head_->ref_count, 1)

reader_search():
  segs = snapshot()  // 裸指针
  for seg in segs->segments: do_search(seg)

reader_exit_epoch():
  atomic_fetch_sub(&head_->ref_count, 1)
```

写者 swap 后，旧 `Segments` 放入退休列表。每 N 次写入或 M 毫秒后，检查退休列表中 ref_count == 0 的版本并释放。

### 6.2 写入锁

```cpp
class IndexWriter {
    std::mutex write_mutex_;  // 仅保护内存段和 WAL
    // 读路径完全不受此锁影响
};
```

## 7. Unicode 文本分析

### 7.1 规范化

搜索质量的基础：全角 → 半角，变音字符标准化，大小写折叠。

```cpp
// ICU 或内置精简 NFKC 表
// 编译期宏控制：VORTEX_USE_ICU
std::string nfkc_normalize(std::string_view input);
std::string lowercase(std::string_view input);
```

V1 内置 ASCII lowercase + 常用全角/半角映射表（覆盖中文输入法常用的符号、数字、字母）。启用 ICU 后自动升级为完整 NFKC + locale-aware case folding。

### 7.2 流式 Tokenizer

返回 `vector<Token>` 会导致大文本的内存峰值，改为 push-based：

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

// 标准拉丁分词
class StandardTokenizer : public Tokenizer {
    void tokenize(std::string_view text, TokenConsumer& consumer) override;
};

// CJK Bigram
class CJKBigramTokenizer : public Tokenizer {
    void tokenize(std::string_view text, TokenConsumer& consumer) override;
};

// 混合：按 Unicode 块自动分派
class MixedTokenizer : public Tokenizer { ... };
```

### 7.3 过滤器链

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
class NFKCFilter : public TokenFilter { ... };  // 增加：规范化 token 文本
```

### 7.4 Analyzer

```cpp
class Analyzer {
public:
    Analyzer(std::unique_ptr<Tokenizer> tokenizer,
             std::vector<std::unique_ptr<TokenFilter>> filters);

    // 获取 term 列表（去位置，只保留去重后的 term + tf）
    // 结果分配在 ScopedThreadArena 上
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

## 8. FST 词典

### 8.1 设计

```
Term Dictionary (FST):
  输入: term → 输出: {df: uint32, posting_offset: uint64, posting_len: uint32}

  特性:
  - byte-based FST，每个 arc label 一个 byte
  - 前缀/后缀自动共享（比 hash map 省 10-30x 内存）
  - 天然字典序，前缀查询 O(k)
  - 增量最小化构建（参考 Lucene Builder 算法）
```

### 8.2 API

```cpp
class TermDictBuilder {
public:
    // 必须按字典序升序插入
    void insert(std::string_view term, uint32_t doc_freq,
                uint64_t posting_offset, uint32_t posting_len);
    // 完成构建
    void finish();

    // 序列化到文件（写入 fst_data + tail xxHash64）
    Status write_file(const std::string& path);

private:
    // 增量最小化 FST 的内部结构
    // 维护未冻结节点的前缀栈
};

class TermDict {
public:
    struct TermInfo {
        uint32_t doc_freq;
        uint64_t posting_offset;
        uint32_t posting_len;
    };

    // 从 mmap 数据构造（零拷贝，不复制数据）
    static Result<std::unique_ptr<TermDict>> from_mmap(
        const uint8_t* data, size_t len);

    // 精确查找
    const TermInfo* lookup(std::string_view term) const;

    // 前缀范围扫描
    void prefix_range(std::string_view prefix,
                      std::function<void(std::string_view, const TermInfo&)> fn) const;

private:
    const uint8_t* fst_data_;
    size_t fst_len_;
};
```

## 9. 倒排链编解码

### 9.1 SIMD-BP128

固定块大小 128，契合 SIMD 寄存器宽度（128-bit x 4 = 512 bits）。每条 128 宽 lane 存 128 个 doc delta 的同一 bit position。

```
Block 编码 (128 docs):

Header (4 bytes):
  [num_docs:8][doc_bits:8][freq_bits:8][flags:8]

Packed Deltas:
  SIMD-BP128: 128 numbers × doc_bits 位宽
  假设 doc_bits=13，则需要 (128*13 + 7)/8 = 208 bytes
  13 个 bit-plane，每个 plane 128 bits 对齐

Packed Freqs:
  同样 SIMD-BP128，128 numbers × freq_bits 位宽
```

解码时使用 SSE4.2/AVX2 指令一次解 128 个 doc_id，标量 FOR 的 3-5x 吞吐。

```cpp
namespace vortex::codec {

// 返回写入字节数
size_t encode_block_simd(const uint32_t* doc_deltas,
                         const uint32_t* freqs,
                         uint8_t num_docs,
                         uint8_t doc_bits,
                         uint8_t freq_bits,
                         uint8_t* output);

// 解码一个 block，返回读取字节数
// 使用 __m128i / __m256i 实现 bit unpack
size_t decode_block_simd(const uint8_t* input,
                         uint32_t* doc_ids_out,
                         uint32_t* freqs_out,
                         uint8_t& num_docs_out);

}  // namespace vortex::codec
```

### 9.2 块级多层跳表

```cpp
struct SkipEntry {
    uint32_t last_doc_id;      // 块中最大 doc_id
    uint64_t block_offset;     // 块在 .doc 中的偏移
};

class SkipList {
public:
    // 构建多层跳表，skip_interval 为跳步（block 数），默认 4
    void build(const std::vector<uint64_t>& block_offsets,
               const std::vector<uint32_t>& max_doc_per_block,
               int skip_interval = 4);

    // 跳到 >= target 的第一个 block，返回 block 偏移
    uint64_t skip_to(uint32_t target_doc, uint64_t current_offset) const;

private:
    // 多层：level[0] = 每 4 block，level[1] = 每 16 block，level[2] = 每 64 block
    std::vector<std::vector<SkipEntry>> levels_;
};
```

### 9.3 PostingList 构造与读取

```cpp
class PostingListBuilder {
public:
    void append(uint32_t doc_id, uint32_t term_freq);
    // 返回 {offset_in_doc_file, byte_count}
    // 同时写入跳表结构到 footer_with_skip
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

    // 遍历全部 posting + 打分
    template<typename Fn>
    void for_each(Fn&& fn) const;

    // advance_to: 跳到 >= target 的第一个 posting
    // 利用 skip list 跳过整块，当前块内线性扫描
    bool advance_to(uint32_t target, uint32_t& doc_id, uint32_t& tf) const;

    size_t doc_freq() const;

private:
    const uint8_t* data_;
    size_t len_;
    const SkipList* skip_;
    size_t block_count_;
};
```

### 9.4 稀疏倒排链优化

高频 term（出现于 > 10% 文档）的 posting list 可能百万级：

- 此类 term 的 IDF 极低，对最终分数贡献小
- 在 AND 求交中作为"长链"被排在最后
- 标记为 `SPARSE_OPT` flag，score 计算时使用近似 IDF（使用全局统计而非精确 doc_freq）
- 支持 `advance_to` 提前终止：如果当前文档的 BM25 贡献低于阈值，跳过

## 10. AND/OR 交集算法

### 10.1 AND 求交

```
intersect(readers, topk):
  1. 按 doc_freq 升序排列 readers
  2. 以最短 reader 为驱动
  3. for each posting in readers[0]:
       doc = posting.doc_id
       ok = true
       for each r in readers[1..]:
         if !r.advance_to(doc, ...): ok = false; break
       if ok:
         score = bm25f(doc, all_hit_tfs, field_lengths)
         push heap(score, doc)
  4. 可选提前终止：
     如果 heap 中 topk 的最小分数 > 当前 reader 剩余所有 posting 的最大可能贡献 → 退出
```

### 10.2 OR 求并

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

## 11. BM25F 打分

### 11.1 字段加权 BM25

单一 BM25 将 title 和 body 同等对待，不合理。BM25F 按字段独立参数线性组合：

```cpp
struct FieldBM25Params {
    double k1 = 1.2;
    double b = 0.75;
    double weight = 1.0;  // 字段权重
};

struct BM25Params {
    std::vector<FieldBM25Params> fields;  // per field_index
};

class BM25Scorer {
public:
    BM25Scorer(const BM25Params& params,
               uint64_t total_docs,
               const std::vector<double>& field_avg_lengths);

    // 单 term 在单 field 上的分数
    double score(uint32_t tf, uint32_t doc_freq,
                 uint32_t field_length, uint8_t field_index) const;

    // 文档最终分数 = Σ over terms Σ over fields
    //   field_weight[f] * IDF(term) * tf_norm(tf, field_len, k1_f, b_f)
    double combine(std::vector<double> per_field_term_scores) const;

    double idf(uint32_t doc_freq) const;

private:
    BM25Params params_;
    uint64_t total_docs_;
    std::vector<double> field_avg_lengths_;
};
```

### 11.2 插件化 Scorer

```cpp
class Scorer {
public:
    virtual ~Scorer() = default;

    // 初始化（绑定段统计信息）
    virtual Status init(uint64_t total_docs,
                        const std::vector<double>& field_avg_lengths) = 0;

    // 单 term-field 贡献
    virtual double term_score(uint32_t tf, uint32_t doc_freq,
                              uint32_t field_length,
                              uint8_t field_index) const = 0;

    // 组合
    virtual double combine(
        absl::Span<const double> term_scores) const = 0;

    // 场权重
    virtual double field_weight(uint8_t field_index) const = 0;
};

// 默认实现
class BM25FScorer : public Scorer { ... };

// V2 可扩展 LTR、LambdaMART
```

## 12. WAL（Write-Ahead Log）

### 12.1 二进制格式

JSON 行格式在吞吐量上不可接受。改为二进制：

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

    // 追加（线程安全），写入内存 buffer，不立刻 fsync
    void append_add(uint64_t internal_id, const Document& doc);
    void append_remove(uint64_t internal_id);

    // 强制刷盘（定期或阈值触发）
    Status sync();

    // flush 后截断
    Status truncate();

    // 崩溃恢复
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
    uint32_t max_buf_size_ = 64 * 1024;  // 64KB 积攒阈值
};
```

Group commit 策略：
- 写入 buffer，buffer 满 64KB 或距上次 sync 超过 10ms → fsync
- 崩溃恢复时重放全部记录（WAL 只在两次 flush 之间存在，数据量 < 1GB）

## 13. 数据完整性

### 13.1 文件尾校验

所有数据文件（.fst, .doc, .store, .fwd）末尾 8 字节：

```
┌──────────────────┬──────────────────┐
│  Data            │  xxHash64 (8B)   │
└──────────────────┴──────────────────┘
```

### 13.2 Segment Manifest 校验

segments.manifest 包含每个段的文件列表 + 预期 checksum：

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

加载段时逐文件校验，任何不匹配都拒绝加载并报告 CORRUPT_INDEX。

## 14. 文件组织

```
index_dir/
├── wal.log              ← 二进制 WAL
├── segments.manifest    ← 段清单 + checksums (JSON)
├── _0.fst / _0.doc / _0.store / _0.fwd / _0.meta / _0.del
├── _1.fst / _1.doc / _1.store / _1.fwd / _1.meta / _1.del
└── ...
```

### 14.1 段元信息

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

## 15. 可观测性

第一天就内建埋点：

```cpp
struct IndexStats {
    // 索引统计
    uint64_t total_docs;
    uint64_t total_terms;
    uint64_t segment_count;
    uint64_t memory_bytes;     // 内存段当前占用
    uint64_t disk_bytes;       // 磁盘文件总大小
    double avgdl;

    // WAL
    uint64_t wal_bytes;
    uint32_t wal_sync_count;

    // 写入
    uint64_t docs_added_total;
    uint64_t docs_removed_total;
    uint64_t flush_count;
    Histogram flush_latency_ms;
    Histogram doc_add_latency_us;
};

struct QueryStats {
    // 查询
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

Histogram 使用固定 bucket 的整数计数数组，不分配内存。

## 16. Query 模型

```cpp
enum class QueryType : uint8_t { TERM, AND, OR, NOT };

struct Query {
    QueryType type;
    std::string term;               // TERM
    std::vector<Query> sub_queries; // AND/OR/NOT

    // 工厂方法
    static Query Term(std::string t);
    static Query And(std::vector<Query> sub);
    static Query Or(std::vector<Query> sub);
    static Query Not(Query q);
};

struct ScoredDoc {
    uint32_t internal_doc_id;
    float score;
    uint64_t segment_id;  // 定位段
};

struct SearchResult {
    std::vector<ScoredDoc> docs;
    uint64_t total_hits;  // 近似值（等于 matched docs 数）
    uint64_t elapsed_us;
};
```

## 17. API 入口

```cpp
namespace vortex {

// 创建或打开索引
Result<std::shared_ptr<IndexWriter>> open_index(
    const std::string& index_dir,
    const Schema& schema);

// 写入
writer->add_document(doc);           // → Status
writer->remove_document(doc_id);     // → Status
writer->flush();                     // → Status

// 读取（获取当前快照 reader）
auto reader = writer->get_reader();  // → shared_ptr<IndexReader>
auto result = reader->search(query, topk);  // → Result<SearchResult>

// 统计
writer->stats();   // → IndexStats
reader->stats();   // → QueryStats

}  // namespace vortex
```

## 18. 代码目录

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
└── types.h              // 基础类型别名

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
├── term_dict_builder.cpp   // FST 增量最小化构建
├── posting_codec.cpp       // SIMD-BP128 (SSE4.2 + AVX2 分发)
├── posting_list.cpp
├── skip_list.cpp
├── scorer.cpp
├── analyzer.cpp
├── tokenizer.cpp
├── filter.cpp
├── wal.cpp
└── unicode.cpp
```

## 19. 构建 & CI 要求

### 19.1 编译器

```
最低: GCC 11+, Clang 18+
推荐: GCC 14+, Clang 20+
SIMD: 编译期检测，SSE4.2 最低，AVX2 可选加速
```

### 19.2 CMake 补充

```cmake
# 严格警告
target_compile_options(vortex PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -Wno-unused-parameter    # 接口预留参数
)

# Sanitizer 构建类型
option(VORTEX_ASAN ON)
option(VORTEX_UBSAN ON)

# SIMD 选项
option(VORTEX_USE_AVX2 ON)
option(VORTEX_USE_ICU OFF)  # 可选 ICU 依赖

# clang-tidy
set(CMAKE_CXX_CLANG_TIDY clang-tidy)
```

### 19.3 CI 流水线

```
Matrix: {gcc-13, gcc-14, clang-18, clang-20} × {Debug, Release, ASan+UBSan}

每个 job:
  cmake -B build -DCMAKE_BUILD_TYPE=$TYPE -DVORTEX_BUILD_TESTS=ON
  cmake --build build -j$(nproc)
  cd build && ctest --output-on-failure

Release 额外:
  cd build && ./benchmarks/vortex_index_benchmarks

clang-tidy 检查（单独 job）
clang-format 检查 --dry-run（单独 job）
```

### 19.4 覆盖率

```cmake
# Coverage build
option(VORTEX_COVERAGE OFF)
if(VORTEX_COVERAGE)
    target_compile_options(vortex PRIVATE --coverage -O0)
    target_link_options(vortex PUBLIC --coverage)
endif()
```

目标：行覆盖率 > 85%。

## 20. 实现顺序

| 阶段 | 内容 | 产出文件 | 预估工时 |
|------|------|---------|---------|
| 0 | 基础设施：Status, Arena, Checksum, Stats | core/ 全 6 个头文件 | 基准 |
| 1 | Unicode 规范化 + Analyzer + Tokenizer | analyzer/, unicode/ | 3d |
| 2 | PostingList Codec (SIMD-BP128) + SkipList | posting_codec, skip_list | 3d |
| 3 | FST 词典构建 + 查询 | term_dict, term_dict_builder | 5d |
| 4 | Segment (内存 + 磁盘) + 序列化 | segment | 3d |
| 5 | BM25F Scorer | scorer | 2d |
| 6 | WAL (二进制 + group commit) | wal | 2d |
| 7 | SegmentList (RCU) | segment_list | 2d |
| 8 | IndexWriter + IndexReader 串联 | index_writer, index_reader | 3d |
| 9 | 单元测试全覆盖 | tests/ (per module) | 每阶段并行 |
| 10 | Benchmark + 性能调优 | benchmarks/ | 3d |
| 11 | CI/CD 配置 | .github/workflows/ | 1d |

阶段 1-4 依赖链：1 → 2/3 可并行 → 4 依赖 2+3
阶段 5-8：5+6 并行 → 7 → 8
阶段 9：全程并行，每完成一个模块就写测试

## 21. 设计原则总结

1. **数据结构决定性能天花板** — FST, SIMD-BP128, SkipList 每个选择都是经过基准工业验证的
2. **分配器可控** — 不使用裸 new/malloc，Arena 分层隔离
3. **零拷贝读取** — mmap + FST 直接映射，查询路径不复制 posting data
4. **数据安全** — 每个文件带 checksum，加载时校验
5. **可观测** — 统计埋点第一天就做，不是事后追加
6. **API 不抛异常** — Status/Result\<T\>，干净的错误处理
7. **编译期分派** — SIMD 根据 CPU 能力选择最优路径，不运行时判断
