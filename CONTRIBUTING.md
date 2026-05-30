# 贡献指南

感谢你的关注！Vortex 是一个活跃维护的 C++17 倒排索引引擎。

## 如何贡献

- **报告 Bug / 提需求** — 提交 GitHub Issue
- **代码** — Fork 仓库，在功能分支开发，PR 提交到 `main`
- **文档** — 欢迎改进和翻译

## 开发环境

- C++17 编译器（GCC 9+ / Clang 10+ / Apple Clang 14+）
- CMake 3.16+

```bash
# 配置和构建
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DVORTEX_BUILD_TESTS=ON
cmake --build build -j$(sysctl -n hw.ncpu)

# 运行测试
cd build && ctest --output-on-failure
```

## 代码风格

- C++17 标准
- `#pragma once` 头文件防护
- `vortex::` 命名空间
- 模板密集代码优先头文件，非模板逻辑放 `.cpp`
- 提交信息用英文

## 协议

MIT — 所有贡献均按此协议授权。