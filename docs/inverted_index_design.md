# 倒排索引 V1 设计

> **本文档记录 Vortex 倒排索引引擎的原始设计蓝图。**
>
> ⚠️ **设计与实现的差异：**
> 实际实现做了以下简化和未完成项，阅读时请注意区分：
>
> **已实现的设计特性（原标注为未实现）：**
> - `IndexReader::get_document()` — 已实现，通过 `.store` 正排文件按 external_id 检索存储字段
> - `.store` 正排文件 — 已实现，段刷盘时写入 `_N.store`，存储 `stored=true` 的字段原始值
>
> **未实现的设计特性：**
> - xxHash64 文件校验和 — 未写入也未验证
> - 原子刷盘 tmp → rename 协议 — 直接写入最终文件
> - SIMD 运行时调度 — `codec_init()` 从未被调用，实际固定使用 SSE4.2 路径
> - WAL CRC32 校验 / 组提交 — WAL 实现简化，无 CRC32
> - Roaring Bitmap 删除位图 — 实际使用 `std::vector<bool>`
> - BM25F 每字段权重 — 实际只有全局 `{k1, b}` 参数
> - `ForwardIndexBuilder` — 类存在但未被实际使用
>
> **已做简化（降低外部依赖）：**
> - `absl::flat_hash_map` → `std::unordered_map`
> - `absl::InlinedVector` → `std::vector`
> - `absl::flat_hash_set` → `std::unordered_set`
> - `absl::Span` → 直接传 `std::vector`
>
> API 签名以实际代码为准。设计文档中的代码示例仅供参考。

## 修订历史

| 版本 | 日期 | 变更内容 |
|------|------|----------|
| V1 | 初始 | 原始设计蓝图 |
| V1.1 | 2026-05 | 精简内容，删除重复代码示例和过时章节；文件名去掉 V1 后缀 |
| V1.2 | 2026-06 | 标注 `get_document()` 和 `.store` 正排存储已实现 |

## 1. 目标与范围

实现一个生产级单节点倒排索引。核心架构遵循 Lucene 的 Segment 设计。

### 1.1 V1 交付物

| 特性 | 标准 |
|------|------|
| 布尔查询 | Term / AND / OR / NOT |
| 评分 | BM25F |
| 逐字段索引 | 每个 TEXT 字段独立分词和索引 |
| 分段架构 | 内存写入 → 刷盘为不可变磁盘分段 |
| 快照隔离 | RCU 保护的读路径，写入从不阻塞读取 |
| WAL | 二进制格式，崩溃恢复 |
| 磁盘 I/O | mmap 零拷贝读取 |
| 词典 | FST（有限状态转移器） |
| 倒排链 | SIMD-BP128 块编码 + 跳表 |
| 内存 | 基于 Arena 的分层分配，热路径无裸 new/malloc |
| 可观测性 | 统计信息内置 |
| 错误处理 | `Status` / `Result<T>` — API 边界无异常 |

### 1.2 V1 不包含

段合并、短语查询、近实时搜索、分布式、通过配置文件的自定义分析器。

## 2. 段架构

### 2.1 整体结构

```
┌──────────────────────────────────────┐
│              IndexWriter              │
│  ┌──────────────┐  ┌──────────────┐  │
│  │ MemorySegment│  │ WAL          │  │
│  │ （可变）     │  │ （二进制）   │  │
│  └──────┬───────┘  └──────────────┘  │
│         │ 刷盘                        │
│         ▼                             │
│  ┌──────────────┐  ┌──────────────┐  │
│  │ Segment 0    │  │ Segment 1    │  │
│  │ （磁盘 mmap）│  │ （磁盘 mmap）│  │
│  └──────────────┘  └──────────────┘  │
└──────────────────────────────────────┘
                    ▲
        ┌───────────┴───────────┐
        │     IndexReader       │
        │  （RCU 保护的时间点视图）│
        │  拥有自己的 BM25 统计  │
        └───────────────────────┘
```

### 2.2 段磁盘文件

```
Segment N（刷盘后）：
├── _N.fst       FST 词典：term → {doc_freq, posting_offset, posting_len}
├── _N.doc       倒排数据：[block_header][SIMD-packed deltas][freqs]
├── _N.fwd       前向索引：doc_id → {doc_length, field_lengths[]}
├── _N.idm       外部 ID 映射：internal_doc_id → external_id
├── _N.store     正排存储：doc_id → {stored_field_values[]}（stored=true 的字段）
├── _N.meta      JSON 元数据
└── _N.del       删除位图
```

### 2.3 写入路径

```
add_document(doc)：
  1. 分配 internal_doc_id = next_id_++
  2. WAL::append_add(internal_doc_id, doc)    ← 崩溃安全
  3. 对每个 indexed=true 的 TEXT 字段：
       a. MixedTokenizer → LowercaseFilter → StopwordFilter
       b. 将每个词条插入 MemorySegment
       c. 记录 field_lengths
  4. 计算 doc_length = sum(field_lengths)
  5. 写入 external_id → internal_doc_id 映射
  6. 如果 arena 用量 >= flush_threshold → flush()
```

### 2.4 读取路径

```
IndexReader::search(query, topk)：
  1. 从 SegmentList 获取 SegmentsSnapshot（RCU，无锁）
  2. 对每个段执行查询 → (doc_id, per_term_tfs[]) 列表
  3. 过滤已删除文档
  4. 使用读取器本地统计信息计算 BM25F 分数
  5. 跨段合并 topk 结果
  6. 从 .idm 解析 external_id
```

## 3. 错误处理

API 边界返回 `Status` 或 `Result<T>`，不抛异常。内部不变量违反应使用 `VORTEX_PANIC`（逻辑 bug，非运行时错误）。

## 4. 内存管理

### 4.1 分层 Arena

索引期间所有内存分配都经过 Arena。读写热路径中不允许裸 `new`/`malloc`。

```
IndexWriter 拥有：
  ├── active_arena_      ← 当前内存段（刷盘后批量重置）
  └── flush_arena_       ← 双缓冲

按请求（线程局部）：
  └── ThreadLocalArena   ← 256 KB 块链，分析器分词输出使用

FST 构建：
  ├── Pass 1 Arena       ← 词条收集
  └── Pass 2 Arena       ← 序列化
```

### 4.2 刷盘阈值

`active_arena_.allocated()` 超过 64 MB 时触发刷盘。可通过 `ram_buffer_mb` 配置（范围：16–256 MB）。

## 5. 数据模型

### 5.1 Schema

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
```

### 5.2 Document

```cpp
struct FieldValue {
    std::string name;
    std::string value;
};

struct Document {
    std::vector<FieldValue> fields;
};
```

Document 中不携带外部 ID。内部 ID 由 IndexWriter 分配（段内密集递增的 uint32）。外部 ID 必须作为 KEYWORD 存储字段提供。

## 6. Unicode 文本分析

### 6.1 分词器

```
StandardTokenizer   — 拉丁语系词边界分词
CJKBigramTokenizer  — "我爱北京" → ["我爱","爱北","北京"]
MixedTokenizer      — 自动检测 CJK 比例，内部分派
```

### 6.2 过滤器链

```
LowercaseFilter  — ASCII 小写
StopwordFilter   — 停用词过滤
```

## 7. FST 词典

搜索引擎词词典具大量字符串键集和自然排序——FST 是明确的选择。Lucene 从 4.0 开始使用 FST 作为词词典。

| | 哈希表 | FST |
|------|--------|-----|
| 内存（100 万词条） | ~80–120 MB | ~4–8 MB |
| 有序迭代 | 需额外排序 | 自然字典序 |
| 前缀查询 | O(k × N) | O(k) |

```
索引阶段：term → {doc_freq, posting_offset, posting_len}
  → TermDictBuilder（增量最小化）→ 序列化 FST → _N.fst

查询阶段：lookup("hello") → 遍历 FST 弧 → TermInfo
```

## 8. 倒排链编解码

### 8.1 SIMD-BP128 块编码

块大小 = 128 个文档，匹配 SIMD 寄存器宽度。

```
块布局：
┌────────────┬─────────────────────┬─────────────────────┐
│ BlockHeader│ 打包的 DocID 差值   │ 打包的词频          │
│ (4 字节)   │ (可变字节)          │ (可变字节)          │
└────────────┴─────────────────────┴─────────────────────┘
```

### 8.2 多层跳表

跳表条目在**块级别**，加速 `advance_to()`。

```
每个块一个 SkipEntry：{last_doc_id, block_offset}
levels_[0]：每 skip_interval 个块（默认 4）
levels_[1]：每 skip_interval² 个块
levels_[2]：每 skip_interval³ 个块
```

## 9. 前向索引 (.fwd)

映射 `doc_id → (doc_length, field_lengths[])`。BM25F 评分需要这些值进行词频归一化。

格式：密集数组，按 doc_id 顺序，每个条目包含 doc_length(uint32) + field_length[0..N-1](uint32)。

## 10. 外部 ID 映射 (.idm)

`remove_document(external_id)` 需要定位哪个 `(segment_id, internal_doc_id)` 持有该外部 ID。

写时维护 `external_id → Location` 映射。刷盘时按 doc_id 排序写入 `.idm` 文件。

## 11. 删除处理

删除使用位图（实际实现使用 `std::vector<bool>`，设计原定 Roaring Bitmap）。

- 活跃段删除：内存中位图，刷盘时写入 `.del`
- 已刷盘段删除：持久化到 `.del`，读取器打开时加载

## 12. BM25F 评分

### 12.1 公式

```
score(term, doc, field) = weight[field]
  × IDF(doc_freq)
  × tf × (k1 + 1)
  / (tf + k1 × (1 - b + b × field_length / field_avg_length))
```

默认 `k1=1.2`，`b=0.75`。全局参数，实际实现无每字段权重。

### 12.2 每读取器统计

每个 IndexReader 从其持有的 SegmentsSnapshot 计算自己的聚合统计。不同读取器看到不同的段集合，这对正确性至关重要。

## 13. WAL（预写日志）

### 13.1 格式

```
每条记录：
┌──────────┬──────────┬──────────┬───────────┬───────────┐
│ op_type  │ body_len │ id_len   │ external  │ payload   │
│ (1B)     │ (4B)     │ (2B)     │ _id       │ (var)     │
└──────────┴──────────┴──────────┴───────────┴───────────┘

op_type：0x01 = ADD，0x02 = REMOVE
```

### 13.2 恢复

崩溃恢复时：删除所有 `.tmp` 文件（部分刷盘，丢弃），从 segments.manifest 重新加载已提交段，回放 WAL 重建内存段。

## 14. 并发模型

### 14.1 RCU 段列表

```
SegmentsSnapshot — 所有已提交段的时间点视图。
通过 epoch 计数器引用计数。没有读取器持有该 epoch 时释放。

SegmentList 管理：
  读取：acquire_snapshot() → 无锁，acquire 顺序
  写入：publish_segment() → 安装带新增段的新快照
  清理：reclaim_retired_snapshots() → 释放无活跃读取器的快照
```

### 14.2 mmap 生命周期

mmap 区域绑定到 Segment 对象，Segment 绑定到 SegmentsSnapshot 的 epoch。epoch 释放后 munmap。

### 14.3 单写入器

```cpp
std::mutex write_mutex_;  // 序列化所有写入操作
```
单写入器并发是行业标准假设（Lucene、Tantivy）。多写入器场景使用外部分片。

## 15. 查询模型

```cpp
enum class QueryType : uint8_t {
    TERM,
    AND,
    OR,
    NOT,   // AND 减去否定子查询
};

struct Query {
    QueryType type;
    std::string term;               // 仅用于 TERM
    std::vector<Query> sub_queries; // AND / OR / NOT
};
```

## 16. 文件组织

```
index_dir/
├── wal.log              ← 二进制 WAL
├── segments.manifest    ← 权威段列表（JSON）
├── _0.fst / _0.doc / _0.fwd / _0.idm / _0.store / _0.meta / _0.del
├── _1.fst / _1.doc / ...
└── ...
```

### segments.manifest

```json
{
  "version": 1,
  "segments": [
    {"id": 0, "doc_count": 5123,  "files": {...}},
    {"id": 1, "doc_count": 2048,  "files": {...}}
  ]
}
```

## 17. 设计原则

1. **数据结构决定性能上限** — FST、SIMD-BP128、SkipList、Roaring，每个选择都有行业基准验证
2. **分配显式可控** — Arena 层次结构确保热路径无 malloc，内存预算可预测
3. **零拷贝读取** — mmap + FST 直接映射，查询路径从不复制倒排数据到中间缓冲区
4. **从第一天起可观测** — 统计信息内置于第一次提交，非事后添加
5. **API 返回错误，从不抛出** — 每层边界 `Status` / `Result<T>`，仅逻辑 bug 触发 PANIC
6. **单写入器，多读取器** — 符合行业标准模型（Lucene、Tantivy）