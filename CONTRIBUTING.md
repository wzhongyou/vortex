# Contributing to Vortex

感谢你对 Vortex 的关注！项目目前处于**规划阶段**，欢迎参与早期讨论和设计。

## 参与方式

- **设计讨论** — 通过 GitHub Issues 参与架构设计和 API 讨论
- **代码贡献** — Fork 仓库，在 feature 分支开发，提交 PR 到 `main`
- **文档** — 改进或翻译文档同样欢迎

## 开发环境

- C++17 编译器（GCC 9+ / Clang 10+ / Apple Clang 14+）
- CMake 3.16+
- GoogleTest（测试）/ Google Benchmark（性能测试）
- FlatBuffers（序列化）

```bash
# 配置并构建
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DVORTEX_BUILD_TESTS=ON
cmake --build build -j$(nproc)

# 运行测试
cd build && ctest --output-on-failure
```

## 代码风格

- C++17 标准
- `#pragma once` 头文件保护
- 命名空间 `vortex::`
- 模板代码优先 header-only；非模板逻辑使用 `.cpp`
- 提交信息使用英文

## License

MIT — 所有贡献均在此协议下授权。
