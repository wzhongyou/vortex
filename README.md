# Vortex — 倒排索引引擎

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
![C++17](https://img.shields.io/badge/C++-17-blue.svg)
[![CMake](https://img.shields.io/badge/CMake-3.16%2B-blue.svg)](CMakeLists.txt)

Vortex 是一个高性能倒排索引（全文搜索）引擎，使用现代 C++17 编写。
定位为**轻量嵌入库**，可直接 link 到你的应用中，无需独立部署服务。

## 功能特性

- **布尔查询** — TERM / AND / OR / NOT / 嵌套组合
- **BM25F 评分** — 基于词频和文档长度的相关性排序
- **分段架构** — 内存写入 → 刷盘为不可变磁盘分段，RCU 快照隔离
- **FST 词典** — 有限状态转移器，百万词条仅需 ~4–8 MB
- **SIMD-BP128 编码** — 128 位 SIMD 块压缩倒排链，多层跳表加速
- **WAL 崩溃恢复** — 预写日志，崩溃后回放未刷盘数据
- **Arena 内存管理** — 分层分配，热路径无裸 malloc
- **逐字段索引** — 每个 TEXT 字段独立分词和索引
- **单写入器，多读取器** — 符合行业标准模型（Lucene、Tantivy）

## 架构

```
┌──────────────────────────────────┐
│    IndexWriter / IndexReader     │  ← 读写 API
├──────────────────────────────────┤
│  Segment │ PostingList │ Scorer  │  ← 倒排索引核心数据结构
├──────────────────────────────────┤
│  FST Dict │ SIMD-BP128 │ SkipList│  ← 高性能编码
├──────────────────────────────────┤
│   Analyzer  │  Arena  │  WAL     │  ← 共享基础设施
└──────────────────────────────────┘
```

| 层 | 职责 |
|------|------|
| **读写 API** | IndexWriter（索引、刷盘）、IndexReader（查询、评分） |
| **核心结构** | Segment 生命周期、倒排链遍历、BM25F 评分 |
| **高性能编码** | FST 词典、SIMD-BP128 块压缩、跳表加速 |
| **基础设施** | Analyzer（分词）、Arena 内存分配、WAL 崩溃恢复 |

## 快速开始

三步体验全文搜索：

```bash
# 1. 构建
cmake -B build -DCMAKE_BUILD_TYPE=Release -DVORTEX_BUILD_EXAMPLES=ON
cmake --build build -j$(sysctl -n hw.ncpu)

# 2. 运行
./build/examples/vortex_example /tmp/vortex_demo

# 3. 查看输出 —— TERM / AND / OR / NOT / 嵌套查询结果 + 统计
```

示例程序演示了完整的六条文档索引、五种查询类型和统计输出。
完整源码见 [examples/basic_usage.cpp](examples/basic_usage.cpp)。

## API 指南

### 1. Schema — 定义字段

每个字段指定类型、是否索引、是否存储：

```cpp
Schema schema;
schema.add_field({"title",    FieldType::TEXT,    true,  true});   // 分词索引 + 存储原始值
schema.add_field({"content",  FieldType::TEXT,    false, true});   // 分词索引，不存储
schema.add_field({"doc_id",   FieldType::KEYWORD, true,  false});  // 精确匹配，不索引
schema.add_field({"category", FieldType::KEYWORD, true,  false});  // 精确匹配，不索引
```

| 参数 | 含义 |
|------|------|
| `type = TEXT` | 全文索引，写入时经分词器处理 |
| `type = KEYWORD` | 精确匹配，不分词（如 ID、分类） |
| `stored = true` | 保留原始字段值（当前仅在内存中，不持久化到磁盘） |
| `indexed = true` | 加入倒排索引，可被搜索 |

### 2. Document — 构建文档

```cpp
Document doc;
doc.fields.push_back({"doc_id",   "1"});
doc.fields.push_back({"title",    "Vortex search engine"});
doc.fields.push_back({"content",  "A fast inverted index written in C++"});
doc.fields.push_back({"category", "tech"});
```

字段名必须与 Schema 定义一致。外部 ID 需作为 KEYWORD 存储字段提供。

### 3. IndexWriter — 建索引

打开索引（目录不存在则自动创建）：

```cpp
auto writer = IndexWriter::open({
    .index_dir         = "/tmp/my_index",
    .schema            = std::move(schema),
    .ram_buffer_mb     = 64,
    .external_id_field = "doc_id"
}).move_value();
```

| 参数 | 默认 | 说明 |
|------|------|------|
| `index_dir` | — | 索引文件存放目录（必填） |
| `schema` | — | 字段定义（必填） |
| `ram_buffer_mb` | 64 | 内存段刷盘阈值（16–256 MB） |
| `external_id_field` | `"doc_id"` | 文档唯一标识对应的字段名 |

添加和刷盘：

```cpp
writer->add_document(doc);        // 添加文档（自动写入 WAL）
writer->flush();                  // 强制内存段刷入磁盘
```

达到 `ram_buffer_mb` 阈值时自动刷盘，通常无需手动调用。

获取读取器：

```cpp
auto reader = writer->get_reader().move_value();
```

每次调用 `get_reader()` 创建时间点快照，后续写入不影响已有读取器。

删除文档：

```cpp
writer->remove_document("2");     // 按外部 ID 删除
```

### 4. 查询 — 搜索

Vortex 支持五种查询方式：

```cpp
// TERM: 包含词条"python"的文档
Query::Term("python")

// AND: 同时包含"neural"和"networks"
Query::And({Query::Term("neural"), Query::Term("networks")})

// OR: 包含"search"或"learning"
Query::Or({Query::Term("search"), Query::Term("learning")})

// NOT: 包含"learning"但不包含"neural"
Query::Not(Query::Term("learning"), Query::Term("neural"))

// 嵌套: "(learning OR search) AND guide"
Query::And({
    Query::Or({Query::Term("learning"), Query::Term("search")}),
    Query::Term("guide")
})
```

执行搜索：

```cpp
auto result = reader->search(query, 10).move_value();
//                                   ↑ topk = 返回前 10 条
```

### 5. 搜索结果

```cpp
struct SearchResult {
    std::vector<ScoredDoc> docs;   // 排序后的结果列表
    uint64_t total_hits;           // 匹配文档总数
    uint64_t elapsed_us;           // 查询耗时（微秒）
};

struct ScoredDoc {
    float    score;                // BM25F 相关性分数
    std::string external_id;       // 文档外部 ID
    uint64_t segment_id;           // 所在段编号（调试用）
    uint32_t internal_doc_id;      // 段内文档编号（调试用）
};
```

遍历结果：

```cpp
auto r = reader->search(Query::Term("python"), 10).move_value();
printf("命中 %zu 条，耗时 %llu us\n", r.total_hits, r.elapsed_us);
for (auto& d : r.docs)
    printf("  [%s] score=%.2f\n", d.external_id.c_str(), d.score);
```

### 6. 统计信息

```cpp
auto& s = writer->stats();
printf("docs=%llu flushes=%llu segments=%llu memory=%llu disk=%llu\n",
       s.docs_added.get(), s.flushes.get(), s.segment_count.get(),
       s.memory_bytes.get(), s.disk_bytes.get());
```

| 计数器 | 说明 |
|--------|------|
| `total_docs` | 索引中的有效文档总数 |
| `docs_added` | 累计添加的文档数 |
| `docs_removed` | 累计删除的文档数 |
| `flushes` | 累计刷盘次数 |
| `segment_count` | 磁盘段数量 |
| `memory_bytes` | 当前内存段用量 |
| `disk_bytes` | 所有段文件总大小 |

## 在自己项目中使用

Vortex 是 CMake 静态库，在项目的 CMakeLists.txt 中引用：

```cmake
# 通过 FetchContent 引入
include(FetchContent)
FetchContent_Declare(
    vortex
    GIT_REPOSITORY https://github.com/wzhongyou/vortex.git
    GIT_TAG        main
)
FetchContent_MakeAvailable(vortex)

target_link_libraries(your_app vortex)
target_include_directories(your_app PRIVATE ${vortex_SOURCE_DIR}/include)
```

或手动构建后 link：

```cmake
target_link_libraries(your_app vortex)
target_include_directories(your_app PRIVATE /path/to/vortex/include)
```

核心头文件：

```cpp
#include "vortex/core/document.h"
#include "vortex/core/query.h"
#include "vortex/core/schema.h"
#include "vortex/inverted/index_writer.h"
#include "vortex/inverted/index_reader.h"
```

## 构建选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `VORTEX_BUILD_TESTS` | OFF | 单元测试（GoogleTest） |
| `VORTEX_BUILD_EXAMPLES` | OFF | 示例程序 |
| `VORTEX_BUILD_BENCHMARKS` | OFF | 基准测试（Google Benchmark） |
| `VORTEX_ASAN` | OFF | AddressSanitizer |
| `VORTEX_UBSAN` | OFF | UndefinedBehaviorSanitizer |
| `VORTEX_USE_AVX2` | x86_64 自动开启 | 编译 AVX2 版 SIMD-BP128（当前未运行时接入） |
| `VORTEX_USE_ICU` | OFF | ICU 国际化分词 |

## 设计原则

1. **数据结构决定性能上限** — FST、SIMD-BP128、SkipList、Roaring，每个选择都有行业基准验证
2. **显式可控的内存分配** — Arena 分层确保热路径无 malloc，内存预算可预测
3. **零拷贝读取** — 段数据写入 `std::vector<uint8_t>` 后直接内存读取，无需额外拷贝
4. **WAL 崩溃恢复** — 写入先写 WAL，刷盘后截断，崩溃时回放恢复未刷盘数据
5. **从第一天起可观测** — 计数器、统计信息内置，非事后添加
6. **API 返回错误，从不抛出** — `Status` / `Result<T>` 在每层边界，仅逻辑 bug 触发 PANIC
7. **单写入器，多读取器** — 符合行业标准模型（Lucene、Tantivy），多写入器使用外部分片

## 详细设计

- [倒排索引 V1 设计](docs/inverted_index_design.md) — 段架构、FST、SIMD-BP128、WAL、BM25F、RCU 并发模型

## 协议

MIT — 见 [LICENSE](LICENSE)。