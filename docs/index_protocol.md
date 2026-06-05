# Vortex 索引构建协议

> 本文档定义 Vortex 倒排索引的外部写入协议。
> 适用于 Spider 等外部系统直接构建索引数据，无需依赖 C++ SDK。
>
> ⚠️ 所有多字节整数均为**小端序**（little-endian）。

## 1. 概览

Vortex 索引由三部分组成：

```
index_dir/
├── wal.log              预写日志（可选，崩溃恢复用）
├── _0.fst               段 0 的 FST 词典
├── _0.doc               段 0 的倒排链
├── _0.fwd               段 0 的前向索引（文档长度）
├── _0.idm               段 0 的外部 ID 映射
├── _0.store             段 0 的正排存储（字段原始值）
├── _0.meta              段 0 的元数据（JSON）
├── _1.fst / _1.doc / ...  段 1
└── ...
```

每个段是一个独立的、不可变的倒排索引子集。多段并行搜索后合并 topk 结果。

## 2. 核心数据类型

| 类型 | 大小 | 说明 |
|------|------|------|
| `u8` | 1 字节 | 无符号整数 |
| `u16` | 2 字节 | 无符号整数，小端 |
| `u32` | 4 字节 | 无符号整数，小端 |
| `u64` | 8 字节 | 无符号整数，小端 |
| `f64` | 8 字节 | IEEE 754 双精度，小端 |
| `vbyte` | 变长 | 可变字节编码（见 2.1） |
| `bytes` | 变长 | 原始字节序列 |

### 2.1 VByte 编码

无符号整数变长编码，每个字节低 7 位为数据位（LSB first），最高位为续位标志：

```
值 ≤ 127:      [0xxxxxxx]                        — 1 字节
值 ≤ 16383:    [1xxxxxxx] [0xxxxxxx]              — 2 字节
值 ≤ 2097151:  [1xxxxxxx] [1xxxxxxx] [0xxxxxxx]   — 3 字节
...
```

### 2.2 位打包（Bit Packing）

倒排链中的 doc ID 差值和词频使用位打包。每个值占 `bits` 位，LSB first 紧密排列：

```
值序列 [v0, v1, v2]，每个 bits=5：
  字节 0: [v0 的 5 位][v1 的低 3 位]
  字节 1: [v1 的高 2 位][v2 的 5 位][1 位填充]
```

## 3. Schema 协议

索引开始前需定义 Schema。写入时必须遵循 Schema 的字段定义顺序。

### 3.1 字段定义

| 字段 | 类型 | indexed | stored | 说明 |
|------|------|---------|--------|------|
| `title` | TEXT | ✓ | ✓ | 标题，分词索引 + 存储 |
| `content` | TEXT | ✓ | ✓ 或 ✗ | 内容，分词索引；是否存储视场景 |
| `url` | KEYWORD | ✗ | ✗ | 原始 URL，不存储 |
| `site` | KEYWORD | ✗ | ✗ | 站点域名，不存储 |
| `author` | TEXT | ✓ | ✓ | 作者，分词索引 + 存储（可按作者名搜索） |
| `timestamp` | KEYWORD | ✗ | ✗ | 时间戳，不存储 |
| `description` | TEXT | ✓ | ✓ | 摘要，分词索引 + 存储 |
| `doc_id` | KEYWORD | ✗ | ✓ | 文档唯一标识（外部 ID） |
| `category` | KEYWORD | ✗ | ✗ | 分类标签，不存储 |

- `TEXT + indexed=true` 的字段会被分词并加入倒排索引（title、content、author、description）
- `KEYWORD` 字段不分词、不索引，仅在 `stored=true` 时写入 `.store`
- `stored=true` 的字段写入 `.store` 文件，可通过 `get_document()` 检索
- 外部 ID 字段名默认 `"doc_id"`，全局唯一

### 3.2 文本处理管线

写入倒排索引前，文本需经以下处理：

```
原始文本
  → NFKC 归一化（全角→半角，ASCII 小写）
  → 分词（MixedTokenizer：CJK >50% 用 CJKBigram，否则 Standard）
  → LowercaseFilter
  → 聚合词频（同一文档内，多字段合并）
```

**CJK bigram 规则**：连续 CJK 字符序列生成滑动 bigram：
```
"搜索引擎" → ["搜索", "索引", "引擎"]
"搜索"     → ["搜索"]
```

**Standard 分词规则**：按 Unicode 字母/数字边界切分：
```
"C++17 template" → ["c", "17", "template"]
```

### 3.3 字段索引编号

`indexed_count` 仅计算 `TEXT + indexed=true` 的字段。`.fwd` 中的 `field_lengths` 按此编号排列。

示例 Schema（9 个字段，4 个 indexed）：
```
title       TEXT    stored indexed   → indexed_idx = 0
content     TEXT    stored indexed   → indexed_idx = 1
url         KEYWORD                  → 不计入 indexed
site        KEYWORD                  → 不计入 indexed
author      TEXT    stored indexed   → indexed_idx = 2
timestamp   KEYWORD                  → 不计入 indexed
description TEXT    stored indexed   → indexed_idx = 3
doc_id      KEYWORD stored          → 不计入 indexed
category    KEYWORD                  → 不计入 indexed
```

## 4. 段文件格式

### 4.1 `.meta` — 段元数据

JSON 格式：

```json
{
  "segment_id": 0,
  "doc_count": 5123,
  "total_terms": 98765,
  "avgdl": 19.276
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `segment_id` | u64 | 段编号，全局递增 |
| `doc_count` | u32 | 文档数 |
| `total_terms` | u32 | 总词条数（所有文档的所有词频之和） |
| `avgdl` | f64 | 平均文档长度 = total_terms / doc_count |

### 4.2 `.idm` — 外部 ID 映射

按内部 doc_id 顺序存储外部 ID 字符串。

```
┌──────────┬──────────┬──────────┬─────────────────────┐
│ count    │ id_len_0 │ id_0     │ id_len_1 │ id_1 ... │
│ u32      │ u16      │ bytes    │ u16      │ bytes    │
└──────────┴──────────┴──────────┴─────────────────────┘
```

| 偏移 | 大小 | 字段 | 说明 |
|------|------|------|------|
| 0 | 4 | count | 文档数量 (u32) |
| 4 | 2 | id_len | 第 0 个文档的外部 ID 长度 (u16) |
| 6 | id_len | id | 第 0 个文档的外部 ID (UTF-8) |
| ... | ... | ... | 重复 count 次 |

第 `i` 条记录对应内部 doc_id = `i`。

### 4.3 `.fwd` — 前向索引（文档长度）

```
┌────────────┬──────────────┬──────────────────────────────────┐
│ doc_count  │ field_count  │ doc_0_lengths │ doc_1_lengths ... │
│ u32        │ u16          │ (1+N)*u32     │ (1+N)*u32        │
└────────────┴──────────────┴──────────────────────────────────┘
```

| 偏移 | 大小 | 字段 | 说明 |
|------|------|------|------|
| 0 | 4 | doc_count | 文档数量 |
| 4 | 2 | field_count | indexed TEXT 字段数（N） |
| 6 | 4 | doc_length | 第 0 个文档的总词数 |
| 10 | 4*N | field_lengths | 第 0 个文档各 indexed 字段的词数 |
| ... | ... | ... | 重复 doc_count 次 |

每个文档占 `(1 + field_count) * 4` 字节。

### 4.4 `.store` — 正排存储（字段原始值）

按内部 doc_id 顺序，按 Schema 中 `stored=true` 字段的出现顺序存储。

```
┌────────────┬───────────────────┬──────────────────────────────┐
│ doc_count  │ stored_field_count│ doc_0_values │ doc_1_values...│
│ u32        │ u16               │              │               │
└────────────┴───────────────────┴──────────────────────────────┘
```

每个文档的存储值：

```
┌──────────┬───────────┐
│ value_len│ value     │  ← 重复 stored_field_count 次
│ u32      │ bytes     │
└──────────┴───────────┘
```

| 偏移 | 大小 | 字段 | 说明 |
|------|------|------|------|
| 0 | 4 | doc_count | 文档数量 |
| 4 | 2 | stored_field_count | stored 字段数 |
| 6 | 4 | value_len | 第 0 个文档第 0 个 stored 字段值长度 |
| 10 | value_len | value | UTF-8 字段值 |
| ... | ... | ... | 重复 stored_field_count * doc_count 次 |

### 4.5 `.fst` — FST 词典

```
┌─────────────┬─────────────┬──────────────────────────────────┐
│ term_count  │ root_offset │ node_0 │ node_1 │ ... │ node_N  │
│ u32         │ u32         │        │        │     │         │
└─────────────┴─────────────┴──────────────────────────────────┘
```

| 偏移 | 大小 | 字段 | 说明 |
|------|------|------|------|
| 0 | 4 | term_count | 词条总数 |
| 4 | 4 | root_offset | 根节点字节偏移 |
| 8 | ... | nodes | 节点数据（逆 DFS 序） |

**节点格式：**

```
┌───────┬──────────┬────────────┬───────────────────────┐
│ flags │ num_arcs │ arcs[]     │ term_info (if FINAL)  │
│ u8    │ u8       │ variable   │ 16 bytes              │
└───────┴──────────┴────────────┴───────────────────────┘
```

| 字段 | 大小 | 说明 |
|------|------|------|
| flags | u8 | bit 0 (`0x01`) = F_FINAL，此节点是某个词的结尾 |
| num_arcs | u8 | 出弧数量 |
| arcs | variable | 每条弧：1 字节 label + vbyte 编码的目标节点偏移 |
| term_info | 16 bytes | 仅当 F_FINAL 时存在 |

**弧格式：** `[label: u8][target_offset: vbyte]`

**TermInfo（16 字节，仅 F_FINAL 节点）：**

| 偏移 | 大小 | 字段 | 说明 |
|------|------|------|------|
| 0 | 4 | doc_freq | 包含此词的文档数 (u32) |
| 4 | 8 | posting_offset | `.doc` 文件中的字节偏移 (u64) |
| 12 | 4 | posting_len | 倒排链字节长度 (u32) |

**关键约束：词条必须按字典序严格递增插入。**

### 4.6 `.doc` — 倒排链

整体为连续的倒排数据块。每个词的倒排链通过 `.fst` 中 TermInfo 的 `(posting_offset, posting_len)` 定位。

**单词倒排链格式：**

```
[block_0][block_1]...[block_N][sentinel][skip_list]
```

#### 4.6.1 Block

```
┌──────────────┬─────────────────────┬─────────────────────┐
│ header       │ packed doc deltas   │ packed frequencies  │
│ 4 bytes      │ (variable)          │ (variable)          │
└──────────────┴─────────────────────┴─────────────────────┘
```

**BlockHeader（4 字节）：**

| 字节 | 名称 | 说明 |
|------|------|------|
| 0 | num_docs | 块内文档数 (1–128) |
| 1 | doc_bits | doc ID 差值的位宽 |
| 2 | freq_bits | 词频的位宽 |
| 3 | flags | 保留，当前为 0 |

**doc ID 差值编码：**
- 第一个值 = 绝对 doc_id（从 0 起的差值）
- 后续值 = `doc_ids[i] - doc_ids[i-1]`
- 所有差值用 `doc_bits` 位打包，LSB first

**词频：** 每个 tf 值用 `freq_bits` 位打包，LSB first。

#### 4.6.2 哨兵

4 字节全零 (`num_docs == 0`)，标记块序列结束。

#### 4.6.3 跳表

```
┌────────────┬──────────────────────────────────────────────┐
│ num_levels │ level_0 │ level_1 │ ...                      │
│ u32        │         │         │                          │
└────────────┴──────────────────────────────────────────────┘
```

**每层：**

```
┌──────────┬────────────────────────────────────────────────┐
│ count    │ entries[]                                      │
│ u32      │ count * 12 bytes                               │
└──────────┴────────────────────────────────────────────────┘
```

**SkipEntry（12 字节）：**

| 偏移 | 大小 | 字段 | 说明 |
|------|------|------|------|
| 0 | 4 | last_doc_id | 覆盖块组的最大 doc_id (u32) |
| 4 | 8 | block_offset | 目标块在 `.doc` 文件中的字节偏移 (u64) |

跳表默认 `skip_interval = 4`（每 4 个块一个 level 0 条目）。

## 5. WAL 格式（可选）

WAL 用于崩溃恢复，写完段后可截断。外部构建器如无需崩溃恢复可跳过。

**文件：** `<index_dir>/wal.log`

### 5.1 ADD 记录 (opcode = 0x01)

```
┌────────┬──────────────┬────────┬─────────────┬──────────────┬─────────────────┐
│ opcode │ internal_id  │ id_len │ external_id │ field_count  │ fields[]        │
│ u8     │ u32          │ u16    │ bytes       │ u16          │ (variable)      │
└────────┴──────────────┴────────┴─────────────┴──────────────┴─────────────────┘
```

**每个字段：**

```
┌──────────┬────────┬───────────┬─────────┐
│ name_len │ name   │ value_len │ value   │
│ u16      │ bytes  │ u32       │ bytes   │
└──────────┴────────┴───────────┴─────────┘
```

### 5.2 REMOVE 记录 (opcode = 0x02)

```
┌────────┬────────┬─────────────┐
│ opcode │ id_len │ external_id │
│ u8     │ u16    │ bytes       │
└────────┴────────┴─────────────┘
```

## 6. 构建流程

外部系统（如 Spider）构建索引的完整流程：

```
1. 定义 Schema（字段名、类型、indexed/stored）

2. 收集文档批次，对每个批次：
   a. 对每个文档：
      - 提取 external_id
      - 对 TEXT+indexed 字段：NFKC 归一化 → 分词 → 过滤 → 聚合词频
      - 分配递增 internal_doc_id (0, 1, 2, ...)
   b. 写入 .idm（external_id → internal_doc_id 映射）
   c. 写入 .fwd（每个文档的 doc_length + field_lengths）
   d. 写入 .store（stored 字段的原始值）
   e. 构建 FST 词典（词条按字典序插入）→ 写入 .fst
   f. 写入 .doc（倒排链：Block 位压缩 + 跳表）
   g. 写入 .meta（段元数据 JSON）

3. 更新段列表，通知搜索引擎加载新段
```

### 6.1 最小可用协议（推荐 Spider 使用）

搜索服务同时提供搜索和建库 API，同一端口即可查询也可写入。

通过 REST API 单条添加：

```
POST /api/document
Content-Type: application/json

{
  "title": "网页标题",
  "content": "网页正文内容",
  "url": "https://example.com/page1",
  "site": "example.com",
  "author": "张三",
  "timestamp": "2026-06-05",
  "description": "网页摘要",
  "category": "新闻"
}
```

> **性能提示**：POST 单条添加会触发 flush 刷新索引，高频写入场景请用下面的批量导入。

批量导入 JSON 文件：

```bash
# 生成 JSON 数组文件
cat > data.json << 'EOF'
[
  {"title": "...", "content": "...", "url": "...", "site": "...",
   "author": "...", "timestamp": "...", "description": "...", "category": "..."},
  ...
]
EOF

# 导入
./vortex_search_demo --import data.json
```

### 6.2 直接写段文件（高性能场景）

当 REST API 吞吐不够时，可直接按本协议写段文件。关键步骤：

1. 按词典序排序所有词条
2. 分配 doc_id（段内从 0 递增）
3. 写入 `.idm`、`.fwd`、`.store`
4. 构建 FST 并写入 `.fst`
5. 写入 `.doc` 倒排链
6. 写入 `.meta`

写入后需通知 IndexWriter 加载新段（当前需通过 C++ API `publish_segment()`，暂无独立加载机制）。

## 7. 搜索查询协议

### 7.1 REST API

**搜索：**

```
GET /api/search?q=<query>&page=<N>
```

| 参数 | 必填 | 说明 |
|------|------|------|
| q | 是 | 搜索关键词，URL 编码 |
| page | 否 | 页码，默认 1，每页 10 条 |

**响应：**

```json
{
  "query": "python",
  "page": 1,
  "page_size": 10,
  "total_hits": 3,
  "elapsed_ms": 0.12,
  "results": [
    {
      "id": "2",
      "title": "Introduction to Python",
      "url": "https://docs.python.org/3/tutorial/",
      "site": "docs.python.org",
      "description": "Official Python tutorial for beginners",
      "category": "Programming",
      "score": 2.7779
    }
  ]
}
```

**文档详情：**

```
GET /api/document/<id>
```

**响应：**

```json
{
  "id": "2",
  "fields": {
    "title": "Introduction to Python",
    "content": "Python is a versatile programming language...",
    "url": "https://docs.python.org/3/tutorial/",
    "site": "docs.python.org",
    "author": "Python Software Foundation",
    "timestamp": "2026-05-15",
    "description": "Official Python tutorial for beginners",
    "doc_id": "2",
    "category": "Programming"
  }
}
```

**添加文档：**

```
POST /api/document
Content-Type: application/json

{
  "title": "网页标题",
  "content": "网页正文内容",
  "url": "https://example.com/page1",
  "site": "example.com",
  "author": "张三",
  "timestamp": "2026-06-05",
  "description": "网页摘要",
  "category": "新闻"
}
```

**响应：**

```json
{"id": "1001", "status": "ok"}
```

### 7.2 查询 DSL

Vortex 原生查询 DSL 支持 5 种组合：

```cpp
Query::Term("python")                                           // 包含词条
Query::And({Query::Term("neural"), Query::Term("networks")})   // 交集
Query::Or({Query::Term("search"), Query::Term("learning")})    // 并集
Query::Not(Query::Term("learning"), Query::Term("neural"))     // 差集
Query::And({Query::Or({...}), Query::Term("guide")})            // 嵌套
```

当前 REST API 仅支持 Term 查询（CJK 自动 bigram AND）。高级查询需通过 C++ SDK。

### 7.3 BM25F 评分

```
score = IDF(doc_freq) × tf × (k1 + 1) / (tf + k1 × (1 - b + b × dl / avgdl))
```

默认参数：`k1 = 1.2`，`b = 0.75`。

| 参数 | 说明 |
|------|------|
| `doc_freq` | 包含此词的文档数 |
| `tf` | 词在文档中的频率 |
| `dl` | 文档长度 |
| `avgdl` | 平均文档长度 |

## 8. 编码约束总结

| 约束 | 说明 |
|------|------|
| 字节序 | 所有整数小端序 |
| doc_id | 段内从 0 递增，不可跳跃 |
| 词条排序 | FST 中的词条必须按字典序严格递增 |
| 块大小 | 倒排链每块最多 128 个文档 |
| external_id | 全局唯一，跨段不可重复 |
| NFKC | 文本入库前需 NFKC 归一化 + ASCII 小写 |
| 字段顺序 | `.store` 和 `.fwd` 中字段按 Schema 定义顺序排列 |
