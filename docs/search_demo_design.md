# 搜索演示设计

> Vortex 搜索演示是一个独立的 HTTP 服务，提供搜索引擎风格的 Web 搜索页面，
> 用于直观体验 Vortex 倒排索引引擎的搜索能力。

## 1. 目标

- 提供零配置的搜索体验——启动即可搜索
- 经典搜索引擎 UI，降低认知成本
- 支持中英文搜索
- 支持在线添加文档和批量导入

## 2. 整体架构

```
┌─────────────────────────────────────────────┐
│              浏览器 (HTML/CSS/JS)            │
│   ┌──────────┐    ┌──────────────────────┐  │
│   │ 首页     │    │ 结果页               │  │
│   │ Logo+框  │ ←→ │ 顶栏+列表+分页       │  │
│   └──────────┘    └──────────────────────┘  │
└────────┬──────────────────┬─────────────────┘
         │ HTTP             │ HTTP
         ▼                  ▼
┌─────────────────────────────────────────────┐
│          vortex_search_demo (C++)           │
│                                             │
│  ┌─────────┐  ┌──────────┐  ┌───────────┐ │
│  │ GET /   │  │ /api/    │  │ POST /api/│ │
│  │ (页面)  │  │ search   │  │ document  │ │
│  └─────────┘  └────┬─────┘  └─────┬─────┘ │
│                     │              │        │
│              ┌──────┴──────────────┘        │
│              ▼                              │
│  ┌─────────────────────────────────────┐   │
│  │          EngineHolder                │   │
│  │  IndexWriter  ←─写路径（add/flush） │   │
│  │  IndexReader  ←─读路径（search）    │   │
│  │  mutex        ←─读写锁保护         │   │
│  └─────────────────────────────────────┘   │
│                                             │
│  ┌─────────────────────────────────────┐   │
│  │  cpp-httplib (header-only HTTP)     │   │
│  └─────────────────────────────────────┘   │
└─────────────────────────────────────────────┘
```

## 3. 文件结构

```
examples/search_demo/
├── CMakeLists.txt       构建配置
├── main.cpp             服务端主程序（路由、索引管理、CJK 查询构建）
├── search_page.h        内嵌 HTML/CSS/JS（原始字符串常量）
├── httplib.h            cpp-httplib v0.18.3 单头文件
└── sample_data.json     20 条中文技术文档示例数据
```

所有前端资源（HTML/CSS/JS）编译进二进制，运行时无需外部文件，启动即用。

## 4. C++ 服务端设计

### 4.1 EngineHolder

```cpp
struct EngineHolder {
    unique_ptr<IndexWriter> writer;
    shared_ptr<IndexReader> reader;
    mutex mu;

    void refresh_reader();       // flush + 获取新 reader
    shared_ptr<IndexReader> get_reader();  // 线程安全获取 reader
};
```

- `writer` 在整个生命周期存活，持有索引目录和 WAL
- `reader` 是 `shared_ptr<IndexReader>`，每次写操作后刷新
- `mutex` 保护 reader 刷新的原子性
- cpp-httplib 线程池并发处理请求，`shared_ptr<IndexReader>` 的并发读安全

### 4.2 Schema

```
title    TEXT    stored=true  indexed=true    // 标题，分词索引 + 存储
content  TEXT    stored=true  indexed=true    // 内容，分词索引 + 存储
doc_id   KEYWORD stored=true indexed=false   // 唯一标识，存储
category KEYWORD stored=true indexed=false   // 分类标签，存储
```

`content` 设为 `stored=true` 以便在搜索结果中展示摘要，这是演示场景的取舍。
生产环境应设为 `stored=false`，通过单独的正排存储检索。

### 4.3 路由

| 端点 | 方法 | 处理逻辑 |
|------|------|----------|
| `/` | GET | 返回内嵌 HTML 页面 |
| `/api/search?q=&page=` | GET | 构建查询 → search → 构造 JSON |
| `/api/document/:id` | GET | get_document → 构造 JSON |
| `/api/document` | POST | 解析 JSON body → add_document → refresh_reader |

### 4.4 分页

Vortex `search()` 只有 `topk` 参数，无 offset。实现方式：

```
请求第 N 页（page_size=10）：
  topk = N * 10
  返回结果切片 [start=(N-1)*10, end=N*10)
```

演示数据量小，此方案可行。生产环境需实现游标或段内 offset。

### 4.5 JSON 构造

不引入 JSON 库，手写 `escape_json()` 辅助函数处理转义。
POST body 解析使用简易字符串匹配（`json_extract_string`），支持基本反转义。

## 5. CJK 查询构建

Vortex 的 CJKBigramTokenizer 将中文文本拆为 bigram（如"搜索引擎" → "搜索","索引","引擎"），
但 `Query::Term("搜索引擎")` 不会自动匹配——需要显式构建 AND 查询。

`build_query()` 函数处理这个映射：

```
输入文本
  │
  ├─ 检测 CJK 字符占比
  │
  ├─ CJK 占比 ≤ 50%：直接 Query::Term(text)
  │
  └─ CJK 占比 > 50%：
       │
       ├─ 提取连续 CJK 字符序列
       │
       ├─ 每个序列拆滑动 bigram（步长 1 字符，窗口 2 字符）
       │   "搜索引擎" → ["搜索", "索引", "引擎"]
       │   "搜索"     → ["搜索"]（单 bigram）
       │
       └─ 所有 bigram 做 Query::And()
           单个 bigram → Query::Term()
           空 → 降级 Query::Term(text)
```

UTF-8 字符串处理：
- `utf8_len()` — 从首字节推断 UTF-8 字符长度
- `utf8_decode()` — 解码 UTF-8 为 Unicode 码点
- `is_cjk_codepoint()` — 判断是否为 CJK 统一汉字/平假名/片假名/谚文

## 6. 前端设计

### 6.1 两种页面状态

**首页** — 经典搜索首页：
- Logo 居中偏上（`padding-top: 18vh`）
- 搜索框 584px 宽，圆角 24px，聚焦时微阴影
- 搜索按钮内嵌在输入框右侧

**结果页** — 经典搜索结果页：
- 顶部窄条：小 Logo + 搜索框，sticky 定位
- 结果列表左偏，对齐经典搜索引擎布局
- 每条结果：favicon + cite 行、蓝色标题、灰色摘要、BM25F 分数
- 底部居中分页

### 6.2 文档添加

右下角蓝色圆形 `+` 按钮（FAB），点击弹出模态框表单。
提交后调用 `POST /api/document`，成功后自动关闭。

### 6.3 文档详情

点击搜索结果标题，调用 `GET /api/document/:id`，在模态框中展示所有存储字段。

### 6.4 URL 路由

搜索后 URL 更新为 `?q=关键词&page=N`，支持浏览器前进/后退。
页面加载时从 URL 参数恢复搜索状态。

## 7. 数据加载

### 7.1 内置数据

启动时自动索引 24 条预定义文档（20 条英文 + 4 条中文），覆盖 Technology、AI、Programming、Lifestyle、Music 分类。

### 7.2 JSON 文件导入

```bash
./vortex_search_demo --import data.json
```

文件格式：
```json
[
  {"title": "...", "content": "...", "category": "..."},
  ...
]
```

文档 ID 从 10000 起自增。导入后自动 flush 并刷新 reader。

### 7.3 在线添加

通过 Web 界面或 `POST /api/document` 添加文档，自动 flush 并刷新 reader。
文档 ID 从 1000 起自增（与导入 ID 不冲突）。

## 8. 线程安全

| 操作 | 保护机制 |
|------|----------|
| 搜索（GET /api/search） | `get_reader()` 返回 `shared_ptr`，多线程并发读安全 |
| 添加文档（POST /api/document） | `refresh_reader()` 在 mutex 下 flush + 更新 reader |
| 文档详情（GET /api/document/:id） | 同搜索，使用 `get_reader()` |

读写不冲突：写操作（add + flush）在 mutex 保护下原子替换 reader，
读操作持旧的 `shared_ptr` 继续工作，不受影响。

## 9. 依赖

| 依赖 | 版本 | 方式 | 说明 |
|------|------|------|------|
| cpp-httplib | v0.18.3 | 本地单头文件 | header-only HTTP 库，无需网络 |
| Vortex | — | CMake target | 主库 |
| pthread | 系统自带 | CMake Threads | cpp-httplib 需要 |

cpp-httplib 以源码形式包含在 `examples/search_demo/httplib.h`，
避免首次构建时的网络依赖（FetchContent git clone 超时问题）。

## 10. 构建与运行

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DVORTEX_BUILD_EXAMPLES=ON
cmake --build build -j$(sysctl -n hw.ncpu)

# 基础启动
./build/examples/search_demo/vortex_search_demo

# 导入数据 + 指定端口
./build/examples/search_demo/vortex_search_demo \
    --import examples/search_demo/sample_data.json \
    --port 3000
```
