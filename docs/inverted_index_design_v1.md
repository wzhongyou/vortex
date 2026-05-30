# 倒排索引 V1 设计

> **本文档记录 Vortex 倒排索引引擎的原始设计蓝图的完整翻译。**
>
> ⚠️ **设计与实现的差异：**
> 实际实现做了以下简化和未完成项，阅读设计文档时请注意区分：
>
> **未实现的设计特性：**
> - xxHash64 文件校验和（第15章） — 未写入也未验证
> - 原子刷盘 tmp → rename 协议（第5.3章） — 直接写入最终文件
> - `.store` 正排文件（第5.2章） — 未实现，段只写 `.fst/.doc/.fwd/.idm/.meta`
> - SIMD 运行时调度（第9.2章） — `codec_init()` 从未被调用，实际固定使用 SSE4.2 路径
> - WAL CRC32 校验 / 组提交（第14章） — WAL 实现简化，无 CRC32
> - Roaring Bitmap 删除位图（第12章） — 实际使用 `std::vector<bool>`
> - BM25F 每字段权重（第13章） — 实际只有全局 `{k1, b}` 参数
> - `IndexReader::get_document()`（第20章） — 不存在
> - `ForwardIndexBuilder`（第10.3章） — 类存在但未被实际使用
>
> **已做简化（降低外部依赖）：**
> - `absl::flat_hash_map` → `std::unordered_map`
> - `absl::InlinedVector` → `std::vector`
> - `absl::flat_hash_set` → `std::unordered_set`
> - `absl::Span` → 直接传 `std::vector`
>
> API 签名以实际代码为准。设计文档中的代码示例仅供参考。

## 1. 目标与范围

实现一个生产级单节点倒排索引。核心架构遵循 Lucene 的 Segment 设计。所有数据结构和算法从一开始就面向生产交付设计，没有一次性原型代码。

### 1.1 V1 交付物

| 特性 | 标准 |
|------|------|
| 布尔查询 | Term / AND / OR / NOT |
| 评分 | BM25F，支持每字段权重，可插拔 Scorer 接口 |
| 逐字段索引 | 每个 TEXT 字段独立分词和索引 |
| 分段架构 | 内存写入 → 原子刷盘为不可变磁盘分段 |
| 快照隔离 | RCU 保护的读路径，写入从不阻塞读取 |
| WAL | 二进制格式，每条记录 CRC32，组提交 |
| 磁盘 I/O | mmap 零拷贝读取，fallocate 预分配 |
| 词典 | FST（有限状态转移器），基于字节的增量最小化 |
| 倒排链 | SIMD-BP128 块编码，多层跳表 |
| 内存 | 基于 Arena 的分层分配，热路径无裸 new/malloc |
| 数据完整性 | 每个数据文件末尾 xxHash64 校验，WAL 记录 CRC32 |
| 可观测性 | 延迟直方图、吞吐计数器、内存/磁盘用量 |
| 错误处理 | `Status` / `Result<T>` — API 边界无异常 |

### 1.2 V1 不包含

段合并、短语查询、近实时搜索、分布式、通过配置文件的自定义分析器。

## 2. 错误处理模型

```
规则：API 边界返回 Status 或 Result<T>，不抛异常。
      内部不变量违反应使用 VORTEX_PANIC（逻辑 bug，非运行时错误）。
      构造函数仅使用 VORTEX_PANIC；工厂方法返回 Result<T>。
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
    std::string msg_;  // SSO — 短消息无堆分配
};

template<typename T>
class Result {
public:
    // 成功
    static Result<T> Ok(T value) { return {std::move(value), Status::OK()}; }
    // 失败
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

## 3. 内存管理

### 3.1 分层 Arena

索引期间的所有内存分配都经过 Arena。读写热路径中不允许裸 `new`/`malloc`。

```
Arena 层次结构：

IndexWriter 拥有：
  ├── active_arena_      ← 当前内存段（刷盘后批量重置）
  └── flush_arena_       ← 双缓冲：刷盘时，新写入使用 active_arena_

按请求（线程局部）：
  └── ThreadLocalArena   ← 256 KB 块链，由分析器分词输出使用
                           ScopedThreadArena 绑定/解绑，作用域退出时自动重置

FST 构建（独立）：
  ├── Pass 1 Arena       ← 词条收集，节点图构建
  └── Pass 2 Arena       ← 序列化；然后释放 Pass 1 Arena
```

```cpp
class Arena {
public:
    explicit Arena(size_t chunk_size = 256 * 1024);

    void* allocate(size_t size, size_t alignment = alignof(std::max_align_t));

    // 快速重置：保留当前块，重置使用游标
    void reset();

    // 完全释放：释放所有块
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

// 线程局部 Arena 绑定
class ScopedThreadArena {
public:
    explicit ScopedThreadArena(Arena& arena);
    ~ScopedThreadArena();  // 解绑 + 重置 arena
    ScopedThreadArena(const ScopedThreadArena&) = delete;
    ScopedThreadArena& operator=(const ScopedThreadArena&) = delete;
};

// 线程局部注册表
Arena& thread_arena();
```

### 3.2 磁盘预分配

```cpp
// 在写入 .doc / .fst / .store 之前调用
// 使用 posix_fallocate（Linux）或 fcntl F_PREALLOCATE（macOS）
Status preallocate_file(int fd, off_t estimated_size);
```

### 3.3 刷盘阈值

当 `active_arena_.allocated()` 超过 64 MB 时触发刷盘。可通过 `IndexWriterOptions::ram_buffer_mb` 配置（范围：16–256 MB）。64 MB 的默认值在写放大和查询延迟之间取得平衡（更少的段 = 更少的跨段合并工作）。

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
    bool stored;     // 保留原始值用于检索
    bool indexed;    // 加入倒排索引
};

struct Schema {
    Status add_field(FieldSchema field);
    const FieldSchema* field(std::string_view name) const;
    uint16_t field_index(std::string_view name) const;  // 未找到返回 UINT16_MAX

    std::vector<FieldSchema> fields;
    absl::flat_hash_map<std::string, uint16_t> name_to_index;
    uint16_t stored_field_count;   // stored=true 的字段数量缓存
    uint16_t indexed_field_count;  // indexed=true, type=TEXT 的字段数量缓存
};
```

### 4.2 Document

```cpp
struct FieldValue {
    std::string name;
    std::string value;       // V1：仅文本
};

struct Document {
    std::vector<FieldValue> fields;
};
```

Document 结构体中不携带外部 ID。内部 ID 由 IndexWriter 分配（每个段内密集递增的 uint32）。外部 ID 必须作为 KEYWORD 存储字段提供。

### 4.3 内部文档表示

```cpp
struct InternalDoc {
    uint64_t segment_id;
    uint32_t doc_id;         // 段内密集
    uint32_t doc_length;     // 所有字段的词条总数

    // 每字段词条数，按 field_index 索引。
    // 仅为索引的 TEXT 字段填充（长度 = schema.indexed_field_count）。
    // 使用 small_vector：<= 8 个字段内联存储，更多则堆分配。
    absl::InlinedVector<uint32_t, 8> field_lengths;

    // 存储字段值，按 stored_field_index（非 field_index）索引。
    // 长度 = schema.stored_field_count。
    absl::InlinedVector<std::string, 4> stored_values;
};
```

`absl::InlinedVector` 提供了此处所需的小尺寸优化——典型 schema 有 1–8 个索引字段，常见情况避免了堆分配。

## 5. 段架构

### 5.1 整体结构

```
┌──────────────────────────────────────────────────────┐
│                    IndexWriter                        │
│                                                       │
│  ┌──────────────┐    ┌──────────────┐                │
│  │ MemorySegment│    │ WAL          │                │
│  │ （可变）     │    │ （二进制，   │                │
│  │ Arena 后端   │    │  CRC32）     │                │
│  └──────┬───────┘    └──────────────┘                │
│         │ 原子刷盘 (.tmp → 重命名)                    │
│         ▼                                             │
│  ┌──────────────┐    ┌──────────────┐                │
│  │ Segment 0    │    │ Segment 1    │  （不可变，    │
│  │ （磁盘）     │    │ （磁盘）     │   mmap'd）     │
│  └──────────────┘    └──────────────┘                │
│         │                   │                         │
│         └─────────┬─────────┘                         │
│                   ▼                                   │
│         ┌──────────────────┐                          │
│         │  SegmentMerge    │ (V2)                     │
│         └──────────────────┘                          │
└──────────────────────────────────────────────────────┘
                    ▲
        ┌───────────┴───────────┐
        │     IndexReader       │
        │  （时间点视图）       │
        │  RCU 保护             │
        │  拥有自己的 BM25      │
        │  统计数据快照         │
        └───────────────────────┘
```

### 5.2 段磁盘文件

```
Segment N（原子刷盘后）：
├── _N.fst       FST 词典：term → {doc_freq, posting_offset, posting_len}
├── _N.doc       倒排数据：[block_header][SIMD-packed deltas][freqs]
├── _N.pos       位置数据（V2 短语查询 — V1 不写入）
├── _N.store     行式文档存储：doc_id → stored_field_values
├── _N.fwd       前向索引：doc_id → {doc_length, field_lengths[]}
├── _N.idm       外部 ID 映射：internal_doc_id → external_id（字符串）
├── _N.meta      JSON 元数据，包含每个文件的 xxHash64 校验和
└── _N.del       删除位图（Roaring Bitmap）
```

每个数据文件末尾有 8 字节的 xxHash64 校验，覆盖文件主体（不包括校验本身）。

### 5.3 原子刷盘协议

刷盘是最危险的操作——刷盘中崩溃绝不能损坏索引。

```
flush() 协议：

1. 计算估计文件大小，fallocate .tmp 文件：
   _N.fst.tmp, _N.doc.tmp, _N.store.tmp, _N.fwd.tmp, _N.idm.tmp, _N.meta.tmp

2. 将所有数据写入 .tmp 文件，计算 xxHash64 校验

3. fsync 所有 .tmp 文件（顺序：数据文件优先，.meta.tmp 最后）

4. 原子重命名（同一文件系统，POSIX 重命名是原子的）：
   对每个 .tmp：rename(_N.xxx.tmp → _N.xxx)

5. 将段条目追加到 segments.manifest，fsync manifest

6. 在 SegmentList 中注册段（RCU 交换）

7. WAL::truncate()

8. 释放 flush_arena_，在 active_arena_ 上创建新的空 MemorySegment

崩溃恢复：
  - 列出 index_dir 中的 *.tmp 文件 → 删除（部分刷盘，丢弃）
  - 从 manifest 重新加载段 → 每个列出的段都是完整的
  - 从上次截断点回放 WAL → 重建内存段
```

关键不变量：**段只有在出现在 segments.manifest 后才可见。** Manifest 是哪些段存在的权威真相源。

### 5.4 写入路径

```
add_document(doc)：
  1. 分配 internal_doc_id = next_id_++
  2. WAL::append_add(internal_doc_id, doc)    ← 在索引更新前崩溃安全
  3. 对每个 indexed=true 的 TEXT 字段：
       a. MixedTokenizer → LowercaseFilter → StopwordFilter
       b. 输出：在 thread_arena() 上分配的 vector<TermWithFreq>
       c. 将每个词条插入 MemorySegment：
            FST 构建器：term → 累积的 PostingListBuilder
            PostingListBuilder.append(doc_id, tf)
       d. 记录该字段的 field_lengths
  4. 计算 doc_length = sum(field_lengths)
  5. 将存储字段值写入 MemorySegment 的文档存储
  6. 写入 external_id → internal_doc_id 映射
  7. 如果 active_arena_.allocated() >= flush_threshold_ → flush()

remove_document(external_id)：
  1. WAL::append_remove(external_id)
  2. 在全局 ID 映射中查找 external_id → (segment_id, internal_doc_id)
  3. 如果在活跃 MemorySegment 中：在内存位图中标记删除
  4. 如果在已刷盘段中：写入 _N.del（Roaring Bitmap）
  5. 更新全局 ID 映射（删除条目）

update_document(external_id, new_doc)：
  = remove_document(external_id) + add_document(new_doc)
  （WAL 在单个组中原子记录两个操作）
```

### 5.5 读取路径（无锁快照）

```
IndexReader 构造：
  1. 从 SegmentList 获取 SegmentsSnapshot（RCU 加载，无锁）
  2. 计算读取器本地的聚合统计：
       total_docs = sum(seg.doc_count)
       avgdl = sum(seg.total_term_count) / total_docs
       field_avg_lengths[f] = sum(seg.field_total_terms[f]) / total_docs
  3. 使用这些统计信息构造 BM25FScorer
  4. 对每个段：mmap .fst, .doc, .fwd, .del 文件（懒加载，首次使用时）
  5. 验证 xxHash64 校验

IndexReader::search(query, topk)：
  1. 对每个段（如果段 > 1 则并行）：
       a. 在段上执行查询 → (doc_id, per_term_tfs[]) 列表
       b. 针对段的 .del 位图过滤 → 丢弃已删除文档
       c. 对每个候选文档：
            读取 .fwd 获取 doc_length, field_lengths[]
            使用读取器本地统计信息计算 BM25F 分数
       d. 生成段本地的 topk 堆
  2. 跨段合并：大小为 topk 的最小堆，从每个段堆中弹出
  3. 对每个 topk 结果：从 .idm 解析 external_id
  4. 返回 SearchResult

IndexReader 析构：
  1. 减少 SegmentsSnapshot 上的 epoch 引用计数
```

## 6. 并发模型

### 6.1 RCU 段列表

```cpp
// SegmentsSnapshot — 所有已提交段的时间点视图。
// 通过 epoch 计数器引用计数。当没有读取器持有该 epoch 时释放。
struct SegmentsSnapshot {
    std::atomic<uint32_t> active_readers{0};  // epoch 引用计数
    uint64_t epoch_id;
    std::vector<std::shared_ptr<const Segment>> segments;

    // 聚合统计信息，在快照创建时计算
    uint64_t total_docs;
    double avgdl;
    std::vector<double> field_avg_lengths;  // 按 field_index 索引
    std::vector<uint64_t> field_total_terms;
};

class SegmentList {
public:
    // 读取路径：无锁，acquire 顺序
    // 返回的原始指针在 exit_epoch() 之前有效
    const SegmentsSnapshot* acquire_snapshot();

    // 读取路径：释放快照
    void release_snapshot(const SegmentsSnapshot* snap);

    // 写入路径：安装带有新增段的新快照
    void publish_segment(std::shared_ptr<const Segment> seg);

    // 由写入器定期触发或在关闭时触发
    // 释放 active_readers == 0 的快照
    void reclaim_retired_snapshots();

private:
    std::atomic<SegmentsSnapshot*> current_{nullptr};

    // 仅由写入器线程访问（单写入器假设）
    std::vector<SegmentsSnapshot*> retired_;
    uint64_t next_epoch_{1};
};
```

### 6.2 mmap 生命周期

mmap 区域必须比引用它们的最后一个读取器存活更久。设计将 mmap 生命周期绑定到 Segment 对象，并将 Segment 生命周期绑定到 SegmentsSnapshot 的 epoch：

```
段生命周期协议：

1. 活跃段：由 SegmentsSnapshot::segments 引用（shared_ptr）
2. 当发布新快照时（刷盘后）：
   − 旧快照进入 retired_ 列表
   − 它的 shared_ptr<Segment> 引用保持文件存活
3. reclaim_retired_snapshots() 检查 active_readers == 0
   − 如果为零：析构快照 → shared_ptr 引用释放 → munmap
   − 不在任何活跃快照中的段可以安全删除

V2 合并：合并的段通过相同的退役管道。
仅在 munmap 完成后取消链接文件（不可能出现 SIGBUS）。
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

### 6.3 写入器锁定

```cpp
class IndexWriter {
    std::mutex write_mutex_;  // 序列化所有写入操作
    // 单写入器并发是行业标准假设
    //（Lucene、Tantivy 等都序列化写入）。
    // 多写入器场景使用外部分片。
};
```

## 7. Unicode 文本分析

### 7.1 规范化

全角→半角、变音符号折叠、大小写折叠。这些是质量基础——搜索"café"必须命中"cafe"。

```cpp
// 内置：ASCII 小写 + CJK 全角/半角映射表
// 覆盖常见中文输入法符号、数字和拉丁字母。
// 表在编译时生成，约 8 KB。
std::string nfkc_normalize(std::string_view input);
std::string lowercase_ascii(std::string_view input);

// 可选的 ICU 集成：完整 NFKC + 本地感知的大小写折叠
// 由 VORTEX_USE_ICU 编译标志控制
```

### 7.2 流式分词器

推送模型消除了大文本的中间 `vector<Token>` 分配：

```cpp
class TokenConsumer {
public:
    virtual void on_token(Token token) = 0;
    virtual ~TokenConsumer() = default;
};

struct Token {
    std::string_view text;  // 指向规范化文本缓冲区
    uint16_t position;      // V1：为 V2 短语查询保留
    uint16_t start_offset;
    uint16_t end_offset;
};

class Tokenizer {
public:
    virtual void tokenize(std::string_view text,
                          TokenConsumer& consumer) = 0;
    virtual ~Tokenizer() = default;
};

// 拉丁语系词边界分词器
class StandardTokenizer : public Tokenizer { ... };

// CJK 二元分词："我爱北京" → ["我爱","爱北","北京"]
// 最小可行方案——无需外部词典依赖
class CJKBigramTokenizer : public Tokenizer { ... };

// 自动调度：检测文本中的 CJK 字符比例，
// 内部分派给 Standard 或 CJKBigram
class MixedTokenizer : public Tokenizer { ... };
```

CJK Bigram 是经过验证的最小解决方案（Elasticsearch CJK Bigram Token Filter，相同方法）。V2 升级为基于词典的分词，API 不变。

### 7.3 过滤器链

过滤器也是推送式，可组合成序列：

```cpp
class TokenFilter {
public:
    // 接收一个 token，可能向下游发射 0..N 个 token
    virtual void process(Token token, TokenConsumer& downstream) = 0;
    virtual ~TokenFilter() = default;
};

class LowercaseFilter : public TokenFilter { ... };
class StopwordFilter : public TokenFilter {
    explicit StopwordFilter(std::shared_ptr<absl::flat_hash_set<std::string>> words);
};
class NFKCFilter : public TokenFilter { ... };
```

### 7.4 分析器 API

```cpp
class Analyzer {
public:
    Analyzer(std::unique_ptr<Tokenizer> tokenizer,
             std::vector<std::unique_ptr<TokenFilter>> filters);

    // 输出 term → 文档内词频。
    // 在当前线程 arena 上分配。
    struct TermWithFreq {
        std::string_view term;  // 指向 arena
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

## 8. FST 词典

### 8.1 为什么选择 FST

| | 哈希表 | FST |
|------|--------|-----|
| 前缀压缩 | 无 | 自动前缀/后缀共享 |
| 内存（100 万词条） | ~80–120 MB | ~4–8 MB |
| 有序迭代 | 需要额外排序 | 自然字典序 |
| 前缀查询 | O(k × N) | O(k) |
| 构建代价 | 在线 O(N) | 离线 O(N log N)（需排序输入） |

搜索引擎词词典具有大量字符串键集和自然排序——FST 是明确的选择。Lucene 从 4.0 开始使用 FST 作为词词典。

### 8.2 FST 数据流

```
索引（构建阶段，按词条排序）：
  term → {doc_freq, posting_offset, posting_len}
    │
    ▼
  TermDictBuilder（增量最小化）
    │ finish()
    ▼
  字节数组（序列化 FST）
    │ 写入 _N.fst + xxHash64 校验
    ▼
  TermDict（直接 mmap，零拷贝）

查询：
  lookup("hello")  → 遍历 FST 弧 → TermInfo*
  prefix_range("he") → 迭代匹配的子树
```

### 8.3 API

```cpp
struct TermInfo {
    uint32_t doc_freq;        // 包含该词条的文档数
    uint64_t posting_offset;  // 在 .doc 文件中的字节偏移
    uint32_t posting_len;     // 在 .doc 文件中的字节长度
};

// ── 构建阶段 ──
class TermDictBuilder {
public:
    // 词条必须按字典序插入
    void insert(std::string_view term, const TermInfo& info);

    // 完成：运行增量最小化，生成序列化 FST 字节
    std::vector<uint8_t> finish();

private:
    // 增量最小化：维护一个未冻结节点栈。
    // 当前缀与下一个词条分歧时，冻结分歧的后缀
    // 并重用等效子树。算法遵循 Lucene 的
    // FST.Builder，关键差异在实现中记录。
    struct UnfrozenNode {
        std::string_view prefix;
        std::vector<std::pair<uint8_t, uint32_t>> arcs; // label → target_node
        uint64_t output;  // 累积的输出值
    };
    std::vector<UnfrozenNode> prefix_stack_;
    // [arc_labels + targets + output] 的哈希 → 规范节点 ID，
    // 用于检测等效子树（最小化的核心）。
    absl::flat_hash_map<size_t, uint32_t> canonical_nodes_;
    std::vector<uint8_t> serialized_;
};

// ── 查询阶段 ──
class TermDict {
public:
    // 从 mmap'd 缓冲区加载（零拷贝—引用数据，不复制）
    static Result<std::unique_ptr<TermDict>> from_mmap(
        const uint8_t* data, size_t len, uint64_t xxhash64_expected);

    const TermInfo* lookup(std::string_view term) const;

    // 迭代所有具有给定前缀的词条。
    // 回调签名：bool(std::string_view term, const TermInfo&)
    // 从回调返回 false 以提前停止。
    void prefix_range(std::string_view prefix,
                      std::function<bool(std::string_view, const TermInfo&)> fn) const;

    size_t term_count() const;
    size_t memory_usage() const { return data_len_; }

private:
    const uint8_t* data_;
    size_t data_len_;
};
```

### 8.4 FST 构建算法草图

增量最小化（核心 FST 构建算法）：

```
insert(term, info)：
  // 找到与前一词条的公共前缀长度
  cpl = common_prefix_len(term, prev_term)

  // 冻结不再在共享路径上的 cpl 以下节点
  while prefix_stack_.size() > cpl + 1：
    node = prefix_stack_.pop_back()
    frozen = try_minimize(node)
    序列化 frozen 到输出

  // 为 term 的非共享部分添加新的后缀节点
  for i = cpl .. term.size()：
    prefix_stack_.push(new UnfrozenNode{term[0..i]})

  // 将输出附加到终端节点
  prefix_stack_.back().output = encode(info)

finish()：
  // 从底部到顶部冻结剩余栈
  while prefix_stack_.size() > 1：
    node = prefix_stack_.pop_back()
    序列化 try_minimize(node)
  // 根节点是最后一个剩余节点
  root_node = prefix_stack_[0]
  写入根节点，并指向它作为入口点

try_minimize(node)：
  hash = hash_arcs(node.arcs, node.output)
  if existing = canonical_nodes_.find(hash)：
    return existing  // 重用等效子树
  canonical_nodes_[hash] = node
  return node
```

实现说明：这是算法上最复杂的组件。计划 5 天开发 + 2 天边界情况测试（空词条、单字符词条、非常长的公共前缀、100 万以上词条的压力测试）。

## 9. 倒排链编解码

### 9.1 SIMD-BP128 块编码

块大小 = 128 个文档，选择匹配 SIMD 寄存器宽度：

- 128 位 SIMD（SSE4.2）：处理 128 个文档为 128×4 = 512 位 = 4 × __m128i
- 256 位 SIMD（AVX2）：处理 128 个文档为 128×2 = 256 位 = 2 × __m256i

块内编码：

```
块布局：
┌────────────┬─────────────────────┬─────────────────────┬──────────┐
│ BlockHeader│ 打包的 DocID 差值   │ 打包的词频          │ 填充     │
│ (4 字节)   │ (可变字节)          │ (可变字节)          │ (到 4B)  │
└────────────┴─────────────────────┴─────────────────────┴──────────┘

BlockHeader（4 字节）：
  num_docs  : uint8   (1–128)
  doc_bits  : uint8   （最大 docID 差值所需位数）
  freq_bits : uint8   （最大词频所需位数）
  flags     : uint8   （保留）

打包的差值（SIMD-BP128）：
  输入：  128 个 docID 差值，每个 ≤ 2^doc_bits - 1
  输出：ceil(128 × doc_bits / 8) 字节，按位平面存储
  位平面 i 存储所有 128 个差值的第 i 位（128 位 = 16 字节，自然对齐）

打包的词频：
  布局与差值相同，使用 freq_bits 位宽。
```

### 9.2 SIMD 运行时调度

库编译为单个二进制文件，可在有或没有 AVX2 的 CPU 上运行：

```cpp
// posting_codec.cpp — 编译时 WITHOUT -mavx2（基准 SSE4.2 路径）
size_t decode_block(const uint8_t* input, uint32_t* doc_ids,
                    uint32_t* freqs, uint8_t& num_docs);

// posting_codec_avx2.cpp — 编译时 WITH -mavx2
size_t decode_block_avx2(const uint8_t* input, uint32_t* doc_ids,
                          uint32_t* freqs, uint8_t& num_docs);

// 函数指针，在初始化时设置一次
using DecodeFunc = size_t (*)(const uint8_t*, uint32_t*, uint32_t*, uint8_t&);
extern DecodeFunc g_decode_block;

// 在 IndexWriter::open() 中调用
void codec_init() {
    if (__builtin_cpu_supports("avx2")) {
        g_decode_block = decode_block_avx2;
    } else {
        g_decode_block = decode_block;
    }
}
```

SSE4.2 基准始终可用（x86-64 要求 SSE4.2），覆盖所有部署目标。

### 9.3 多层跳表

跳表条目在**块级别**，而不是倒排级别：

```cpp
// 每层每个块一个跳表条目
struct SkipEntry {
    uint32_t last_doc_id;     // 跳过的块中的最大 doc_id
    uint64_t block_offset;    // 目标块的文件偏移
};

class SkipList {
public:
    // 从块元数据构建多层跳表。
    // skip_interval：第 0 层跳表条目的块间隔（默认 4）
    void build(const std::vector<uint64_t>& block_offsets,
               const std::vector<uint32_t>& block_max_doc,
               int skip_interval = 4);

    // 找到 `target` 之前的块，从 `current_offset` 开始。
    // 返回 block_offset（文件位置）。由 advance_to() 使用。
    uint64_t advance(uint32_t target, uint64_t current_offset) const;

    // 序列化为字节（追加在 .doc 的倒排数据末尾）
    std::vector<uint8_t> serialize() const;

    // 从 .doc 尾部反序列化
    static SkipList deserialize(const uint8_t* data, size_t len);

private:
    // levels_[0]：每 skip_interval 个块
    // levels_[1]：每 skip_interval^2 个块
    // levels_[2]：每 skip_interval^3 个块，等等
    std::vector<std::vector<SkipEntry>> levels_;
};
```

### 9.4 PostingList API

```cpp
class PostingListBuilder {
public:
    void append(uint32_t doc_id, uint32_t term_freq);

    // 返回 {offset_in_doc_file, byte_count}
    // 倒排数据 + 跳表脚注连续写入
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
    // data 指向 mmap'd .doc 文件

    // 迭代所有倒排条目。Fn：void(uint32_t doc_id, uint32_t tf)
    template<typename Fn>
    void for_each(Fn&& fn) const;

    // 前进到第一个 doc_id >= target。
    // 如果没有这样的文档（已耗尽），返回 false。
    // 使用跳表跳过不相关的块。
    bool advance_to(uint32_t target, uint32_t& doc_id, uint32_t& tf) const;

    size_t doc_freq() const { return total_docs_; }

private:
    const uint8_t* data_;
    size_t data_len_;
    size_t total_docs_;
    SkipList skip_list_;
    size_t block_count_;

    // 当前解码位置
    uint32_t current_block_;
    const uint8_t* next_block_ptr_;
};
```

### 9.5 稀疏倒排优化

出现在 > 10% 文档中的词条 IDF 贡献接近零。对于这些词条：

- 分数计算使用近似 IDF（预计算常数），跳过精确 doc_freq 查找
- `advance_to()` 支持提前终止：如果剩余倒排的最大可能分数贡献低于当前 topk 堆最小值，则停止遍历
- 在词条元数据中标记为 SPARSE

## 10. 前向索引 (.fwd)

### 10.1 目的

前向索引映射 `doc_id → (doc_length, field_lengths[])`。BM25F 评分需要 `doc_length` 和每字段长度用于词频归一化。将这些存储在倒排链本身中会使其臃肿；一个单独的密集数组是标准解决方案。

### 10.2 二进制格式

```
前向索引文件 (_N.fwd)：

头部（16 字节）：
  doc_count        : uint32   （此段中的文档数）
  field_count      : uint16   （索引的 TEXT 字段数）
  entry_size       : uint16   （每个条目字节数，固定）
  reserved         : uint64   （填充到 16B 对齐）

主体（doc_count × entry_size 字节）：
  对每个 doc_id（密集，0..doc_count-1）：
    doc_length      : uint32   （所有字段的总词条数）
    field_length[0] : uint32   （字段 0 的词条数）
    field_length[1] : uint32   （字段 1 的词条数）
    ...
    field_length[field_count-1] : uint32

尾部（8 字节）：
  xxHash64 校验 [Header + Body]
```

每个条目为 `4 + 4 × field_count` 字节。对于典型的 3 个文本字段的 schema：每个文档 16 字节。100 万文档 = 每段 16 MB——足够紧凑以完全 mmap。

### 10.3 API

```cpp
class ForwardIndexBuilder {
public:
    ForwardIndexBuilder(uint16_t field_count);

    // 每个文档调用一次，按 doc_id 顺序
    void append(uint32_t doc_length, const uint32_t* field_lengths);

    Status flush(int fd);

    size_t doc_count() const;

private:
    uint16_t field_count_;
    std::vector<uint8_t> buffer_;  // 线性缓冲区，增长到 doc_count * entry_size
    size_t doc_count_ = 0;
};

class ForwardIndex {
public:
    static Result<std::unique_ptr<ForwardIndex>> from_mmap(
        const uint8_t* data, size_t len, uint64_t xxhash64_expected);

    struct DocInfo {
        uint32_t doc_length;
        const uint32_t* field_lengths;  // 指向 mmap，长度 = field_count
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

## 11. 外部 ID 映射 (.idm)

### 11.1 问题

`remove_document(external_id)` 必须定位哪个 `(segment_id, internal_doc_id)` 持有该外部 ID。扫描所有段的存储字段是 O(total_docs) 的。需要专用映射。

### 11.2 内存中映射

```cpp
// IndexWriter 维护的全局映射
class ExternalIdMap {
public:
    struct Location {
        uint64_t segment_id;
        uint32_t internal_doc_id;
    };

    // 如果未找到返回 nullptr
    const Location* find(std::string_view external_id) const;

    // 在 add_document 时插入
    void insert(std::string_view external_id, uint64_t seg_id, uint32_t doc_id);

    // 在删除时移除
    void remove(std::string_view external_id);

    // 刷盘时保存到 .idm 文件（用于活跃内存段）
    Status flush(int fd, Arena& scratch);  // 按 doc_id 排序

    // 从 .idm 文件加载
    static Result<std::unique_ptr<ExternalIdMap>> from_file(
        const std::string& path, uint64_t xxhash64_expected);

    size_t size() const;

private:
    // external_id → 活跃内存段或已刷盘段中的位置
    absl::flat_hash_map<std::string, Location> map_;
    // 也存储反向：对于给定段，doc_id → external_id 字符串
    // 用于构建 .idm 文件（按 doc_id 排序）
    std::vector<std::string> pending_external_ids_;  // 按 internal_doc_id 索引
};
```

### 11.3 .idm 文件格式

```
_ID 映射文件 (_N.idm)：

头部（12 字节）：
  doc_count   : uint32
  reserved    : uint64

主体（可变，按 internal_doc_id 排序）：
  对每个 doc_id（0 .. doc_count-1）：
    external_id_len : uint16
    external_id     : char[external_id_len]（不以 null 结尾）

尾部（8 字节）：
  xxHash64 校验 [Header + Body]
```

规模估计：假设平均 external_id 20 字节，100 万文档 = 每段约 22 MB。对于 mmap'd 文件是可接受的。

### 11.4 搜索结果解析

```cpp
// 在 IndexReader::search() 中，获取 topk 后：
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
    std::string external_id;   // 从 .idm 解析
};
```

## 12. 删除处理

### 12.1 删除位图

删除使用 Roaring Bitmap（压缩位图，广泛用于搜索引擎）：

```cpp
class DeleteBitmap {
public:
    bool is_deleted(uint32_t doc_id) const;
    void mark_deleted(uint32_t doc_id);
    void mark_deleted_bulk(const std::vector<uint32_t>& doc_ids);

    size_t deleted_count() const;
    size_t total_count() const { return total_; }
    void set_total(uint32_t total);

    // 序列化
    std::vector<uint8_t> serialize() const;  // Roaring 格式
    static DeleteBitmap deserialize(const uint8_t* data, size_t len);

private:
    Roaring roaring_;
    uint32_t total_ = 0;  // 此段的 doc_count
};
```

### 12.2 删除流程

```
remove_document(external_id)：
  1. WAL::append_remove(external_id)
  2. Location* loc = external_id_map_.find(external_id)
  3. 如果 loc->segment_id == active_segment_id：
       memory_segment_.deletes.mark_deleted(loc->internal_doc_id)
  4. 否则：
       加载段 loc->segment_id 的 _N.del
       bitmap.mark_deleted(loc->internal_doc_id)
       保存 _N.del
  5. external_id_map_.remove(external_id)

查询过滤（index_reader.cpp）：
  从倒排链交集/并集收集候选 doc_id 后：
    对每个候选：
      segment = segments_[candidate.segment_id]
      if segment->deletes().is_deleted(candidate.doc_id)：
        continue  // 跳过已删除文档
      score = bm25f.score(...)
      推入堆
```

### 12.3 删除生命周期

- 活跃段删除：内存中位图，刷盘时随段写入
- 已刷盘段删除：持久化到 `.del` 文件，在读取器打开时加载
- V2 段合并：已删除文档被物理移除（不复制到合并段）

## 13. BM25F 评分

### 13.1 字段加权 BM25

```cpp
struct FieldBM25Params {
    double k1 = 1.2;
    double b = 0.75;
    double weight = 1.0;  // 最终分数中字段的相对权重
};

struct BM25Params {
    std::vector<FieldBM25Params> fields;  // 按 field_index 索引
};
```

默认：所有字段 `k1=1.2`，`b=0.75`，`weight=1.0`。字段权重通过 `BM25Params` 调整。

### 13.2 每读取器统计

每个 IndexReader 从其持有的 SegmentsSnapshot 计算自己的聚合统计信息。这对正确性至关重要——不同的读取器看到不同的段集合。

```cpp
// IndexReader 本地，在 open() 时计算
struct ReaderStatistics {
    uint64_t total_docs;
    double avgdl;                         // 跨所有文档
    std::vector<double> field_avg_lengths; // 按 field_index
};
```

### 13.3 API

```cpp
class Scorer {
public:
    virtual ~Scorer() = default;

    // 使用读取器本地的聚合统计信息初始化
    virtual void init(const ReaderStatistics& stats, const BM25Params& params) = 0;

    // 对单个 (term, field) 贡献评分。
    // tf：该词条在此字段中的词频
    // doc_freq：包含该词条的文档总数（来自 TermDict）
    // field_length：此文档的字段长度（来自 .fwd）
    // field_index：此分数对应的字段
    virtual double term_score(uint32_t tf, uint32_t doc_freq,
                              uint32_t field_length,
                              uint8_t field_index) const = 0;

    // 将文档的每个词条分数合并为最终分数
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
        // 加权词条分数之和
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

BM25F 每个词条每个字段的公式：

```
score(term, doc, field) = weight[field]
  × IDF(doc_freq)
  × tf × (k1 + 1)
  / (tf + k1 × (1 - b + b × field_length / field_avg_length))
```

## 14. WAL（预写日志）

### 14.1 二进制格式

```
WAL 记录：
┌──────────┬──────────┬──────────┬───────────┬───────────┬──────────┐
│ CRC32    │ op_type  │ body_len │ id_len    │ external  │ payload  │
│ (4B)     │ (1B)     │ (4B)     │ (2B)      │ _id       │ (var)    │
└──────────┴──────────┴──────────┴───────────┴───────────┴──────────┘

op_type：0x01 = ADD，0x02 = REMOVE

ADD 负载：
  field_count : uint16
  对每个字段：
    name_len   : uint16
    name       : char[name_len]
    value_len  : uint32
    value      : char[value_len]

REMOVE：无负载（头部中的 external_id 足够）
```

### 14.2 API

```cpp
class WAL {
public:
    explicit WAL(const std::string& path);

    Status append_add(uint32_t internal_id, std::string_view external_id,
                       const Document& doc);
    Status append_remove(std::string_view external_id);

    // 强制 fsync（在组提交计时器或阈值时调用）
    Status sync();

    // 成功刷盘后截断
    Status truncate();

    // 崩溃恢复：回放自上次截断以来的所有记录
    struct RecoveryState {
        uint32_t next_internal_id;
        // 映射：internal_id → (external_id, Document)
        absl::flat_hash_map<uint32_t, std::pair<std::string, Document>> active_docs;
        absl::flat_hash_set<std::string> removed_ids;
    };
    Result<RecoveryState> recover(const Schema& schema);

    // 统计信息
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

### 14.3 组提交

```
append()：
  1. 将记录序列化到 write_buf_
  2. 如果 write_buf_.size() >= kMaxBufferSize 或
        time_since_last_sync >= kMaxSyncIntervalMs：
       → write(fd_, write_buf_)
       → fdatasync(fd_)
       → write_buf_.clear()
       → last_sync_ = now
```

### 14.4 恢复协议

```
recover()：
  1. 如果 wal.log 不存在或为空 → 返回空状态
  2. 顺序验证每条记录的 CRC32
  3. 如果 CRC32 不匹配 → 在最后一个有效记录处截断，记录警告
  4. 回放：将 ADD/REMOVE 记录应用到内存状态
  5. 返回 RecoveryState

在 IndexWriter::open() 中：
  1. 读取 segments.manifest → 加载已提交的段
  2. 删除所有 *.tmp 文件（崩溃导致的部分刷盘）
  3. 调用 WAL::recover() → 重建内存段状态
  4. 设置 next_internal_id = recovery.next_internal_id
  5. WAL::truncate()  // WAL 已回放，安全地重新开始
```

## 15. 数据完整性

### 15.1 文件尾部

每个数据文件（.fst, .doc, .store, .fwd, .idm）末尾带有：

```
┌────────────────────┬────────────────┐
│  文件主体          │  xxHash64 (8B) │
└────────────────────┴────────────────┘
```

加载时验证：
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

### 15.2 Manifest 作为真相源

`segments.manifest` 是已提交段的权威列表。崩溃恢复清理任何未被 manifest 引用的文件（或 .tmp 文件）。

## 16. 文件组织

```
index_dir/
├── wal.log              ← 二进制 WAL（每次打开时重新创建）
├── segments.manifest    ← 权威段列表 + 校验和（JSON）
├── _0.fst               ← Segment 0 FST 词典
├── _0.doc               ← Segment 0 倒排数据
├── _0.store             ← Segment 0 文档存储
├── _0.fwd               ← Segment 0 前向索引
├── _0.idm               ← Segment 0 外部 ID 映射
├── _0.meta              ← Segment 0 元数据
├── _0.del               ← Segment 0 删除位图
├── _1.fst / _1.doc / ...
└── ...
```

临时文件（刷盘期间）：`_N.*.tmp` — 崩溃恢复时清理。

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

### 16.2 段元数据 (_N.meta)

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

## 17. AND / OR 交集算法

### 17.1 AND

```
intersect(readers, topk, scorer, fwd_index, del_bitmaps)：
  1. 按 doc_freq 升序排序读取器（最短优先）
  2. driver = readers[0]；others = readers[1..]

  3. 对 driver 中的每个倒排条目：
       doc_id = posting.doc_id

       // 检查删除
       if del_bitmaps.is_deleted(doc_id)：continue

       // 尝试在所有其他读取器中查找 doc_id
       found = true
       all_tfs = [driver.tf]
       for each r in others：
         if !r.advance_to(doc_id, matched_doc, matched_tf)：
           found = false；break
         all_tfs.append(matched_tf)

       if found：
         // 从前向索引查找文档信息
         doc_info = fwd_index.get(doc_id)
         // 跨所有字段命中计算 BM25F
         score = scorer.combine(scorer.term_score(all_tfs[i], ...))
         if heap.size() < topk or score > heap.min()：
           heap.push({doc_id, score})

  4. 可选的提前终止：
       max_remaining_score = scorer.max_possible_term_contribution(driver.next_doc_id)
       if heap.size() == topk and heap.min() > max_remaining_score：
         break
```

### 17.2 OR

```
union_merge(readers, topk, scorer, fwd_index, del_bitmaps)：
  1. 最小堆（reader_idx, doc_id, tf）
  2. 当堆不为空时：
       // 弹出具有相同 doc_id 的所有条目
       doc_id = heap.min().doc_id
       all_tfs = []
       while heap.min().doc_id == doc_id：
         entry = heap.pop()
         all_tfs.append({entry.reader_idx, entry.tf})
         前进 entry.reader；如果未耗尽：推入堆

       // 检查删除
       if del_bitmaps.is_deleted(doc_id)：continue

       // 评分
       doc_info = fwd_index.get(doc_id)
       score = scorer.combine(...)
       推入 topk_heap

  3. 提前终止：与 AND 相同
```

## 18. 可观测性

从第一天起就内置。不是事后添加的仪表化。

```cpp
// 使用 std::atomic 的线程安全计数器
struct AtomicCounter {
    void inc() { val_.fetch_add(1, std::memory_order_relaxed); }
    void add(uint64_t n) { val_.fetch_add(n, std::memory_order_relaxed); }
    uint64_t get() const { return val_.load(std::memory_order_relaxed); }
private:
    std::atomic<uint64_t> val_{0};
};

// 固定桶整数直方图，构造后无分配
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
    // 索引状态
    AtomicCounter total_docs;
    AtomicCounter total_terms;
    AtomicCounter segment_count;
    AtomicCounter memory_bytes;      // 活跃 arena 使用量
    AtomicCounter disk_bytes;        // 所有段文件大小之和

    // WAL
    AtomicCounter wal_bytes;
    AtomicCounter wal_syncs;

    // 写入吞吐量
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

## 19. 查询模型

```cpp
enum class QueryType : uint8_t {
    TERM,
    AND,
    OR,
    NOT,   // NOT 查询作为 AND 减去否定子查询运行
};

struct Query {
    QueryType type;
    std::string term;               // 仅用于 TERM
    std::vector<Query> sub_queries; // 用于 AND / OR / NOT

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
    std::string external_id;     // 从 .idm 解析
    // 调试用的内部字段
    uint64_t segment_id;
    uint32_t internal_doc_id;
};

struct SearchResult {
    std::vector<ScoredDoc> docs;
    uint64_t total_hits;         // 匹配的文档总数（用于分页）
    uint64_t elapsed_us;
};
```

## 20. API 入口

```cpp
namespace vortex {

struct IndexWriterOptions {
    std::string index_dir;
    Schema schema;
    uint32_t ram_buffer_mb = 64;     // 刷盘阈值
    uint32_t wal_sync_interval_ms = 10;
    BM25Params bm25_params;

    // 外部 ID 字段名（KEYWORD 存储字段，必需）
    std::string external_id_field = "doc_id";
};

// 打开或创建索引
Result<std::unique_ptr<IndexWriter>> IndexWriter::open(IndexWriterOptions opts);

// ── 写入 API ──

// 添加文档。返回 Status；文档在返回后是崩溃安全的。
Status IndexWriter::add_document(const Document& doc);

// 按外部 ID 删除。
Status IndexWriter::remove_document(std::string_view external_id);

// 原子删除后添加。
Status IndexWriter::update_document(std::string_view external_id,
                                     const Document& new_doc);

// 强制将内存段刷入磁盘。
Status IndexWriter::flush();

// 获取时间点读取器。
Result<std::shared_ptr<IndexReader>> IndexWriter::get_reader();

// 统计信息。
const IndexStats& IndexWriter::stats() const;

// ── 读取 API ──

// 使用布尔查询搜索，返回 topk 结果。
Result<SearchResult> IndexReader::search(const Query& query, size_t topk = 10);

// 按外部 ID 获取单个文档（存储字段查找）。
Result<std::optional<Document>> IndexReader::get_document(
    std::string_view external_id);

// 统计信息。
const QueryStats& IndexReader::stats() const;

}  // namespace vortex
```

## 21. 代码目录

```
include/vortex/core/
├── status.h             // Status, Result<T>
├── arena.h              // Arena, ScopedThreadArena
├── document.h           // Document, FieldValue
├── schema.h             // Schema, FieldSchema, FieldType
├── query.h              // Query, QueryType
├── search_result.h      // ScoredDoc, SearchResult
├── stats.h              // IndexStats, QueryStats, Histogram, AtomicCounter
├── checksum.h           // xxHash64 包装器
└── types.h              // u32, u64 等

include/vortex/inverted/
├── index_writer.h       // IndexWriter, IndexWriterOptions
├── index_reader.h       // IndexReader
├── segment.h            // Segment（不可变，磁盘后端），MemorySegment（可变）
├── segment_list.h       // SegmentList, SegmentsSnapshot (RCU)
├── term_dict.h          // TermDict, TermDictBuilder, TermInfo
├── posting_codec.h      // SIMD-BP128 编解码器，运行时调度声明
├── posting_list.h       // PostingListBuilder, PostingListReader
├── skip_list.h          // SkipList
├── forward_index.h      // ForwardIndex, ForwardIndexBuilder, DocInfo
├── external_id_map.h    // ExternalIdMap, IdMapping（每段 .idm）
├── delete_bitmap.h      // DeleteBitmap（Roaring Bitmap 包装器）
├── scorer.h             // Scorer（接口），BM25FScorer
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
├── term_dict_builder.cpp    // FST 增量最小化
├── posting_codec.cpp        // SSE4.2 基准
├── posting_codec_avx2.cpp   // AVX2 加速（用 -mavx2 编译）
├── posting_codec_dispatch.cpp // 运行时调度设置
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

## 22. 构建与 CI 要求

### 22.1 编译器策略

```
最低要求：     GCC 13+, Clang 18+
推荐：         GCC 14+, Clang 20+
SIMD 基准：    SSE4.2（x86-64 强制）
SIMD 可选：    AVX2（运行时调度，独立编译单元）
```

### 22.2 CMake 配置

```cmake
# 严格警告
target_compile_options(vortex PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -Wno-unused-parameter
)

# 使用 AVX2 编译 posting_codec_avx2.cpp
if(VORTEX_USE_AVX2)
    set_source_files_properties(
        src/inverted/posting_codec_avx2.cpp
        PROPERTIES COMPILE_FLAGS "-mavx2"
    )
endif()

# 消毒器构建
option(VORTEX_ASAN ON)
option(VORTEX_UBSAN ON)

# 可选的 ICU
option(VORTEX_USE_ICU OFF)

# 链接依赖
# 实际实现使用 std:: 容器替代了设计中的 absl（flat_hash_map → std::unordered_map,
# InlinedVector → std::vector, Span → std::span），降低外部依赖。
# Roaring Bitmap
include(FetchContent)
FetchContent_Declare(roaring GIT_REPOSITORY ... GIT_TAG v4.0.0)
target_link_libraries(vortex PUBLIC roaring)
```

### 22.3 CI 矩阵

```
矩阵：
  compiler：   [gcc-13, gcc-14, clang-18, clang-20]
  build_type： [Debug, Release, ASan+UBSan]

每个矩阵单元的任务：
  1. cmake -B build -DCMAKE_BUILD_TYPE=$TYPE -DVORTEX_BUILD_TESTS=ON
  2. cmake --build build -j$(nproc)
  3. cd build && ctest --output-on-failure -j$(nproc)

独立任务：
  - clang-tidy（严格，不允许警告）
  - clang-format --dry-run -Werror
  - 覆盖率（Debug + 覆盖率标志 → lcov → > 85% 行覆盖率）
  - 基准测试（Release → 运行基准测试，与之前结果比较）

预提交钩子：
  - 对暂存文件运行 clang-format
  - 对暂存文件运行 clang-tidy（仅更改的行）
```

## 23. 实现顺序

| 阶段 | 内容 | 文件 | 依赖 | 估算 |
|------|------|------|------|------|
| 0 | 基础设施：Status, Arena, Checksum, Stats, Types | core/*.h | — | 2d |
| 1 | Unicode + Analyzer + Tokenizer + Filters | unicode, analyzer, tokenizer, filter | 0 | 3d |
| 2 | Posting Codec (SSE4.2 baseline + AVX2) + SkipList | posting_codec*, skip_list | 0 | 3d |
| 3 | FST 字典构建 + 查询 | term_dict* | 0 | 5d |
| 4 | ForwardIndex + ExternalIdMap + DeleteBitmap | forward_index, external_id_map, delete_bitmap | 0 | 3d |
| 5 | Segment（内存 + 磁盘，原子刷盘，mmap） | segment | 3, 4 | 4d |
| 6 | BM25F Scorer | scorer | 4 | 2d |
| 7 | WAL（二进制 + 组提交 + 恢复） | wal | 1 | 2d |
| 8 | SegmentList（RCU） | segment_list | 5 | 2d |
| 9 | IndexWriter + IndexReader 集成 | index_writer, index_reader | 5, 6, 7, 8 | 3d |
| 10 | 单元测试（每个模块，与上述并行） | tests/ | 每阶段 | ongoing |
| 11 | 基准测试 + 性能调优 | benchmarks/ | 9 | 3d |
| 12 | CI/CD 流水线 | .github/ | — | 1d |

阶段依赖图：
```
0 ──→ 1 ──→ 7
  ──→ 2 ──→ 5 ──→ 9
  ──→ 3 ──→ 5
  ──→ 4 ──→ 5, 6
               8 ──→ 9
```

阶段 1/2/3/4 可并行。阶段 5 是第一个集成点。阶段 9 是最终集成。

## 24. 设计原则

1. **数据结构决定性能上限。** FST、SIMD-BP128、SkipList、Roaring——每个选择都经过行业基准验证。没有"以后再修"的数据结构。

2. **分配是显式且可控的。** Arena 层次结构确保热路径中没有 malloc。内存预算可预测。

3. **零拷贝读取。** mmap + FST 直接映射。查询路径从不将倒排数据复制到中间缓冲区。

4. **默认数据安全。** 每个文件都带有校验和。崩溃恢复经过端到端验证。原子刷盘（tmp → 重命名）防止部分写入损坏已提交状态。

5. **从第一天起可观测。** 直方图、计数器和统计信息内置于第一次提交，而非事后添加。

6. **API 返回错误，从不抛出。** 每个边界的 `Status` / `Result<T>`。只有逻辑 bug 触发 PANIC。

7. **SIMD 运行时调度。** 适用于所有 x86-64 CPU 的单一二进制文件。在初始化时根据 CPU 能力选择最优路径。

8. **删除处理是一等关注点。** 外部 ID 映射实现 O(1) 删除查找。`.del` 位图从第一天起就集成到查询路径中。

9. **单写入器，多读取器。** 符合行业标准模型（Lucene、Tantivy）。多写入器场景使用外部分片。