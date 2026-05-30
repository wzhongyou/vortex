# CLAUDE.md

本文档指导 Claude Code 在此仓库中的代码协助工作。

## 项目

Vortex — C++17 倒排索引（全文搜索）引擎。MIT 协议。

## 构建命令

```bash
# 配置（从仓库根目录）
cmake -B build -DCMAKE_BUILD_TYPE=Release

# 构建
cmake --build build -j$(sysctl -n hw.ncpu)

# Debug 构建含测试
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DVORTEX_BUILD_TESTS=ON
cmake --build build -j$(sysctl -n hw.ncpu)

# 运行测试
cd build && ctest --output-on-failure

# 运行单个测试
cd build && ctest -R <test_name> --output-on-failure

# 运行基准测试
cd build && ./benchmarks/vortex_benchmarks
```

## 目录结构

```
include/vortex/core/     # Arena, Document, Query, Schema, Stats, Status, Types
include/vortex/inverted/ # Analyzer, FST Dict, PostingList, BM25F, Segment, WAL
src/                     # 实现文件（与 include 镜像）
tests/                   # GoogleTest 单元测试
examples/                # 使用示例
benchmarks/              # Google Benchmark 基准测试
```

## 代码规范

- C++17，CMake 3.16+
- 模板密集代码优先头文件，非模板逻辑放 `.cpp`
- GoogleTest 测试，Google Benchmark 基准测试
- 自定义二进制序列化（不使用 FlatBuffers）
- 命名空间：`vortex::`
- 头文件防护：`#pragma once`

## 架构

详见 [README.md](README.md) 的四层架构概览和设计原则。