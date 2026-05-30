# Vortex — 倒排索引引擎

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
![C++17](https://img.shields.io/badge/C++-17-blue.svg)
[![CMake](https://img.shields.io/badge/CMake-3.16%2B-blue.svg)](CMakeLists.txt)

Vortex 是一个高性能倒排索引（全文搜索）引擎，使用现代 C++17 编写。
定位为**轻量嵌入库**，可直接 link 到你的应用中，无需独立部署服务。

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

## 示例详解

[examples/basic_usage.cpp](examples/basic_usage.cpp) 演示了完整搜索流程：

- **Step 1**: 定义 Schema（TEXT/KEYWORD 字段类型），构建索引（6 条文档）
- **Step 2**: 搜索 —— 支持 **TERM / AND / OR / NOT / 嵌套布尔查询**
- **Step 3**: 查看索引统计（文档数、刷盘次数、内存/磁盘用量）

## 在自己项目中使用

Vortex 是静态库，link 即可：

```cmake
target_link_libraries(your_app vortex)
target_include_directories(your_app PRIVATE /path/to/vortex/include)
```

核心 API 就几个类：

```cpp
Schema schema;
schema.add_field({"title", FieldType::TEXT, true, true});

// 建索引
auto writer = IndexWriter::open({dir, std::move(schema), 64, "id"}).move_value();
writer->add_document(doc);
writer->flush();

// 搜索
auto reader = writer->get_reader().move_value();
auto result = reader->search(Query::Term("hello"), 10).move_value();
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

- [倒排索引 V1 设计](docs/inverted_index_design_v1.md) — 段架构、FST、SIMD-BP128、WAL、BM25F、RCU 并发模型

## License

MIT — 见 [LICENSE](LICENSE)。