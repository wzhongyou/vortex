#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>

#include "vortex/core/document.h"
#include "vortex/core/query.h"
#include "vortex/core/schema.h"
#include "vortex/core/types.h"
#include "vortex/inverted/index_reader.h"
#include "vortex/inverted/index_writer.h"
#include "httplib.h"
#include "search_page.h"

using namespace vortex;

// ── JSON 转义 ──────────────────────────────────────────────────────────────

static std::string escape_json(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// ── 示例数据 ───────────────────────────────────────────────────────────────

struct SampleDoc {
    const char* id;
    const char* title;
    const char* content;
    const char* url;
    const char* site;
    const char* author;
    const char* timestamp;
    const char* description;
    const char* category;
};

static const SampleDoc kSampleDocs[] = {
    {"1",  "Vortex Search Engine",
     "Vortex is a fast inverted index engine written in C++17 with BM25F scoring and roaring bitmaps",
     "https://github.com/wzhongyou/vortex", "github.com", "wzhongyou", "2026-06-01",
     "A fast C++17 inverted index engine", "Technology"},
    {"2",  "Introduction to Python",
     "Python is a versatile programming language popular for data science web development and automation",
     "https://docs.python.org/3/tutorial/", "docs.python.org", "Python Software Foundation", "2026-05-15",
     "Official Python tutorial for beginners", "Programming"},
    {"3",  "Machine Learning Fundamentals",
     "Machine learning enables computers to learn from data without explicit programming using neural networks",
     "https://scikit-learn.org/stable/tutorial/", "scikit-learn.org", "sklearn Team", "2026-04-20",
     "Core concepts of machine learning", "AI"},
    {"4",  "Deep Learning with Convolutional Networks",
     "Convolutional neural networks revolutionized computer vision and image recognition tasks",
     "https://pytorch.org/tutorials/", "pytorch.org", "PyTorch Team", "2026-03-10",
     "CNN architectures for computer vision", "AI"},
    {"5",  "Healthy Cooking Recipes",
     "Delicious and nutritious meal ideas for everyday cooking with fresh ingredients",
     "https://cooking.example.com/healthy", "cooking.example.com", "Chef Maria", "2026-05-28",
     "Easy and healthy meal ideas", "Lifestyle"},
    {"6",  "Learning Guitar for Beginners",
     "A beginner friendly guide to playing acoustic guitar with chords and strumming patterns",
     "https://music.example.com/guitar", "music.example.com", "Music Academy", "2026-02-14",
     "Start playing guitar today", "Music"},
    {"7",  "Rust Programming Language",
     "Rust provides memory safety without garbage collection making it ideal for systems programming",
     "https://doc.rust-lang.org/book/", "doc.rust-lang.org", "Rust Team", "2026-05-01",
     "The Rust programming language guide", "Programming"},
    {"8",  "Natural Language Processing",
     "NLP techniques process and analyze human language data for translation sentiment analysis and chatbots",
     "https://huggingface.co/learn/nlp-course", "huggingface.co", "HuggingFace", "2026-04-05",
     "Modern NLP with transformers", "AI"},
    {"9",  "Mountain Hiking Adventures",
     "Explore breathtaking mountain trails and outdoor adventures in national parks",
     "https://travel.example.com/hiking", "travel.example.com", "Outdoor Weekly", "2026-01-30",
     "Best mountain trails guide", "Lifestyle"},
    {"10", "Web Development with JavaScript",
     "Modern JavaScript frameworks enable building interactive web applications with React and Vue",
     "https://developer.mozilla.org/en-US/docs/Web", "developer.mozilla.org", "MDN Contributors", "2026-05-20",
     "Modern JS frameworks overview", "Programming"},
    {"11", "Database Design Principles",
     "Relational database design normalization indexing and query optimization techniques",
     "https://postgres.example.com/design", "postgres.example.com", "DB Experts", "2026-03-25",
     "Database normalization and optimization", "Technology"},
    {"12", "Introduction to Reinforcement Learning",
     "Reinforcement learning trains agents through rewards and penalties in interactive environments",
     "https://spinningup.openai.com/", "openai.com", "OpenAI", "2026-02-08",
     "RL fundamentals and algorithms", "AI"},
    {"13", "Digital Photography Tips",
     "Master composition lighting and post processing to take stunning digital photographs",
     "https://photo.example.com/tips", "photo.example.com", "Photo Magazine", "2026-04-18",
     "Improve your photography skills", "Lifestyle"},
    {"14", "C++ Template Metaprogramming",
     "Advanced C++ template techniques for compile time computation and type safe generic programming",
     "https://en.cppreference.com/w/cpp/language/templates", "cppreference.com", "CPP Community", "2026-01-15",
     "Advanced template techniques in C++", "Programming"},
    {"15", "Cloud Computing with Kubernetes",
     "Container orchestration with Kubernetes enables scalable and resilient cloud deployments",
     "https://kubernetes.io/docs/tutorials/", "kubernetes.io", "K8s SIG Docs", "2026-05-10",
     "Kubernetes orchestration guide", "Technology"},
    {"16", "Jazz Music Theory",
     "Understanding jazz harmony improvisation chord progressions and modal scales",
     "https://musictheory.example.com/jazz", "musictheory.example.com", "Jazz Institute", "2026-03-01",
     "Jazz theory and improvisation", "Music"},
    {"17", "Data Visualization with Python",
     "Create insightful charts and graphs using matplotlib seaborn and plotly libraries",
     "https://matplotlib.org/stable/tutorials/", "matplotlib.org", "Matplotlib Devs", "2026-04-12",
     "Python data visualization libraries", "Programming"},
    {"18", "Cybersecurity Best Practices",
     "Protect your systems with encryption authentication and vulnerability assessment strategies",
     "https://security.example.com/best-practices", "security.example.com", "Security Weekly", "2026-05-22",
     "Essential cybersecurity measures", "Technology"},
    {"19", "Yoga and Mindfulness",
     "Combine physical postures breathing exercises and meditation for holistic wellness",
     "https://wellness.example.com/yoga", "wellness.example.com", "Wellness Today", "2026-02-20",
     "Yoga practice for beginners", "Lifestyle"},
    {"20", "Building Search Engines",
     "Inverted indexes tokenization relevance scoring and distributed search architecture",
     "https://search.example.com/architecture", "search.example.com", "Search Eng Blog", "2026-03-30",
     "How search engines work internally", "Technology"},
    {"21", "搜索引擎原理与实践",
     "倒排索引是搜索引擎的核心数据结构，通过词项到文档的映射实现快速全文检索",
     "https://search.example.cn/principle", "search.example.cn", "搜索技术组", "2026-05-08",
     "搜索引擎核心原理详解", "Technology"},
    {"22", "深度学习与自然语言处理",
     "深度学习模型在自然语言处理领域取得了突破性进展，包括机器翻译和文本生成",
     "https://ai.example.cn/dl-nlp", "ai.example.cn", "AI 研究院", "2026-04-15",
     "深度学习在 NLP 中的应用", "AI"},
    {"23", "Python 数据科学入门",
     "Python 是数据科学领域最流行的编程语言，拥有丰富的数值计算和机器学习库",
     "https://datascience.example.cn/python", "datascience.example.cn", "数据科学社区", "2026-03-20",
     "Python 数据科学生态概览", "Programming"},
    {"24", "云原生架构设计",
     "云原生应用采用微服务架构，结合容器化和持续交付实现敏捷开发和弹性伸缩",
     "https://cloudnative.example.cn/design", "cloudnative.example.cn", "云原生社区", "2026-05-25",
     "云原生架构设计模式", "Technology"},
};

// ── 索引加载 ───────────────────────────────────────────────────────────────

struct EngineHolder {
    std::unique_ptr<IndexWriter> writer;
    std::shared_ptr<IndexReader> reader;
    std::mutex mu;

    void refresh_reader() {
        std::lock_guard<std::mutex> lock(mu);
        writer->flush();
        reader = writer->get_reader().move_value();
    }

    std::shared_ptr<IndexReader> get_reader() {
        std::lock_guard<std::mutex> lock(mu);
        return reader;
    }
};

static std::unique_ptr<EngineHolder> load_sample_data(const std::string& index_dir) {
    Schema schema;
    schema.add_field({"title",       FieldType::TEXT,    true,  true});
    schema.add_field({"content",     FieldType::TEXT,    true,  true});
    schema.add_field({"url",         FieldType::KEYWORD, true,  false});
    schema.add_field({"site",        FieldType::KEYWORD, true,  false});
    schema.add_field({"author",      FieldType::TEXT,    true,  true});
    schema.add_field({"timestamp",   FieldType::KEYWORD, true,  false});
    schema.add_field({"description", FieldType::TEXT,    true,  true});
    schema.add_field({"doc_id",      FieldType::KEYWORD, true,  false});
    schema.add_field({"category",    FieldType::KEYWORD, true,  false});

    auto writer = IndexWriter::open({index_dir, std::move(schema), 64, "doc_id"}).move_value();

    for (const auto& sd : kSampleDocs) {
        Document doc;
        doc.fields = {
            {"title",       sd.title},
            {"content",     sd.content},
            {"url",         sd.url},
            {"site",        sd.site},
            {"author",      sd.author},
            {"timestamp",   sd.timestamp},
            {"description", sd.description},
            {"doc_id",      sd.id},
            {"category",    sd.category},
        };
        auto s = writer->add_document(doc);
        if (!s.ok()) {
            std::cerr << "add_document " << sd.id << " failed: " << s.message() << std::endl;
        }
    }
    writer->flush();

    auto reader = writer->get_reader().move_value();
    std::cout << "Indexed " << sizeof(kSampleDocs) / sizeof(kSampleDocs[0])
              << " documents, segments: " << writer->stats().segment_count.get() << std::endl;

    auto holder = std::make_unique<EngineHolder>();
    holder->writer = std::move(writer);
    holder->reader = std::move(reader);
    return holder;
}

// ── 辅助：从 Document 取字段 ──────────────────────────────────────────────

static std::string get_field(const Document& doc, const std::string& name) {
    for (const auto& f : doc.fields) {
        if (f.name == name) return f.value;
    }
    return "";
}

// ── CJK bigram 查询构建 ──────────────────────────────────────────────────

static bool is_cjk_codepoint(uint32_t cp) {
    return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
           (cp >= 0x3040 && cp <= 0x309F) || (cp >= 0x30A0 && cp <= 0x30FF) ||
           (cp >= 0xAC00 && cp <= 0xD7AF);
}

static int utf8_len(unsigned char c) {
    return (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
}

static uint32_t utf8_decode(const std::string& s, size_t i) {
    auto c = static_cast<unsigned char>(s[i]);
    int len = utf8_len(c);
    if (i + len > s.size()) return 0;
    uint32_t cp = (c & ((1 << (7 - len)) - 1));
    for (int j = 1; j < len; j++)
        cp = (cp << 6) | (static_cast<unsigned char>(s[i + j]) & 0x3F);
    return cp;
}

static Query build_query(const std::string& text) {
    int cjk = 0, total = 0;
    for (size_t i = 0; i < text.size(); ) {
        int len = utf8_len(static_cast<unsigned char>(text[i]));
        if (is_cjk_codepoint(utf8_decode(text, i))) cjk++;
        total++;
        i += len;
    }

    if (total == 0 || cjk * 2 <= total) return Query::Term(text);

    std::vector<Query> sub_queries;
    std::string cjk_run;

    auto flush_run = [&]() {
        if (cjk_run.size() < 6) {
            // 不足一个 bigram（2 个 CJK 字符 = 6 字节），直接 Term
            if (!cjk_run.empty()) sub_queries.push_back(Query::Term(cjk_run));
            cjk_run.clear();
            return;
        }
        // 滑动 bigram：每次前进 3 字节（1 个 CJK 字符），取 6 字节（2 个 CJK 字符）
        for (size_t i = 0; i + 6 <= cjk_run.size(); i += 3)
            sub_queries.push_back(Query::Term(cjk_run.substr(i, 6)));
        cjk_run.clear();
    };

    for (size_t i = 0; i < text.size(); ) {
        int len = utf8_len(static_cast<unsigned char>(text[i]));
        if (is_cjk_codepoint(utf8_decode(text, i))) {
            cjk_run.append(text, i, static_cast<size_t>(len));
        } else {
            flush_run();
        }
        i += static_cast<size_t>(len);
    }
    flush_run();

    if (sub_queries.empty()) return Query::Term(text);
    if (sub_queries.size() == 1) return sub_queries[0];
    return Query::And(std::move(sub_queries));
}

// ── 简易 JSON 数组解析 ──────────────────────────────────────────────────────

struct JsonDoc {
    std::string title;
    std::string content;
    std::string url;
    std::string site;
    std::string author;
    std::string timestamp;
    std::string description;
    std::string category;
};

static std::string json_extract_string(const std::string& json, size_t from, const std::string& key) {
    std::string pattern = "\"" + key + "\"";
    auto pos = json.find(pattern, from);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + pattern.size());
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos);
    if (pos == std::string::npos) return "";
    auto end = pos + 1;
    while (end < json.size()) {
        if (json[end] == '\\' && end + 1 < json.size()) { end += 2; continue; }
        if (json[end] == '"') break;
        end++;
    }
    std::string val;
    for (size_t i = pos + 1; i < end; i++) {
        if (json[i] == '\\' && i + 1 < end) {
            char next = json[i + 1];
            if (next == '"')       { val += '"';  i++; }
            else if (next == '\\') { val += '\\'; i++; }
            else if (next == 'n')  { val += '\n'; i++; }
            else if (next == 't')  { val += '\t'; i++; }
            else if (next == '/')  { val += '/';  i++; }
            else                    val += json[i];
        } else {
            val += json[i];
        }
    }
    return val;
}

static std::vector<JsonDoc> parse_json_docs(const std::string& json) {
    std::vector<JsonDoc> docs;
    size_t pos = 0;
    while ((pos = json.find('{', pos)) != std::string::npos) {
        auto end = json.find('}', pos);
        if (end == std::string::npos) break;
        JsonDoc d;
        d.title = json_extract_string(json, pos, "title");
        d.content = json_extract_string(json, pos, "content");
        d.url = json_extract_string(json, pos, "url");
        d.site = json_extract_string(json, pos, "site");
        d.author = json_extract_string(json, pos, "author");
        d.timestamp = json_extract_string(json, pos, "timestamp");
        d.description = json_extract_string(json, pos, "description");
        d.category = json_extract_string(json, pos, "category");
        docs.push_back(std::move(d));
        pos = end + 1;
    }
    return docs;
}

// ── Main ───────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    int port = 8080;
    std::string index_dir =
        (std::filesystem::temp_directory_path() / "vortex_search_demo").string();
    std::string import_file;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc)   port = std::stoi(argv[++i]);
        if (arg == "--dir" && i + 1 < argc)    index_dir = argv[++i];
        if (arg == "--import" && i + 1 < argc) import_file = argv[++i];
    }

    std::filesystem::remove_all(index_dir);
    auto engine = load_sample_data(index_dir);

    // --import: 从 JSON 文件批量导入
    if (!import_file.empty()) {
        std::ifstream ifs(import_file);
        if (!ifs.is_open()) {
            std::cerr << "Cannot open import file: " << import_file << std::endl;
            return 1;
        }
        std::string json((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
        auto docs = parse_json_docs(json);
        int ok = 0;
        for (auto& d : docs) {
            if (d.title.empty() && d.content.empty()) continue;
            static uint64_t import_id = 10000;
            Document doc;
            doc.fields = {
                {"title",       d.title.empty() ? "Untitled" : d.title},
                {"content",     d.content},
                {"url",         d.url},
                {"site",        d.site},
                {"author",      d.author},
                {"timestamp",   d.timestamp},
                {"description", d.description},
                {"doc_id",      std::to_string(import_id++)},
                {"category",    d.category.empty() ? "General" : d.category},
            };
            auto s = engine->writer->add_document(doc);
            if (s.ok()) ok++;
        }
        engine->refresh_reader();
        std::cout << "Imported " << ok << "/" << docs.size()
                  << " documents from " << import_file << std::endl;
    }

    httplib::Server svr;

    // GET / — 搜索页面
    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(kSearchPageHTML, "text/html; charset=utf-8");
    });

    // GET /api/search?q=xxx&page=N — 搜索
    svr.Get("/api/search", [&engine](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Content-Type", "application/json; charset=utf-8");

        std::string query_text = req.get_param_value("q");
        if (query_text.empty()) {
            res.set_content("{\"error\":\"missing query parameter q\"}", "application/json");
            return;
        }

        int page = 1;
        if (req.has_param("page")) {
            try { page = std::stoi(req.get_param_value("page")); } catch (...) {}
        }
        if (page < 1) page = 1;

        const int page_size = 10;
        int topk = page * page_size;

        auto reader = engine->get_reader();
        auto result = reader->search(build_query(query_text), static_cast<size_t>(topk));
        if (!result.ok()) {
            res.status = 500;
            res.set_content("{\"error\":\"search failed\"}", "application/json");
            return;
        }

        auto sr = result.move_value();
        int start = (page - 1) * page_size;
        int end = std::min(static_cast<int>(sr.docs.size()), page * page_size);

        std::string json = "{";
        json += "\"query\":\"" + escape_json(query_text) + "\",";
        json += "\"page\":" + std::to_string(page) + ",";
        json += "\"page_size\":" + std::to_string(page_size) + ",";
        json += "\"total_hits\":" + std::to_string(sr.total_hits) + ",";
        char elapsed_buf[32];
        snprintf(elapsed_buf, sizeof(elapsed_buf), "%.2f", sr.elapsed_us / 1000.0);
        json += "\"elapsed_ms\":" + std::string(elapsed_buf) + ",";
        json += "\"results\":[";

        bool first = true;
        for (int i = start; i < end; i++) {
            auto& sd = sr.docs[static_cast<size_t>(i)];
            auto doc_opt = reader->get_document(sd.external_id);
            std::string title = sd.external_id;
            std::string url;
            std::string site;
            std::string description;
            std::string category;

            if (doc_opt.ok()) {
                auto doc_val = doc_opt.move_value();
                if (doc_val.has_value()) {
                    auto& doc = doc_val.value();
                    title = get_field(doc, "title");
                    url = get_field(doc, "url");
                    site = get_field(doc, "site");
                    description = get_field(doc, "description");
                    category = get_field(doc, "category");
                }
            }

            if (!first) json += ",";
            first = false;
            json += "{";
            json += "\"id\":\"" + escape_json(sd.external_id) + "\",";
            json += "\"title\":\"" + escape_json(title) + "\",";
            json += "\"url\":\"" + escape_json(url) + "\",";
            json += "\"site\":\"" + escape_json(site) + "\",";
            json += "\"description\":\"" + escape_json(description) + "\",";
            json += "\"category\":\"" + escape_json(category) + "\",";
            char score_buf[32];
            snprintf(score_buf, sizeof(score_buf), "%.4f", sd.score);
            json += "\"score\":" + std::string(score_buf);
            json += "}";
        }

        json += "]}";
        res.set_content(json, "application/json; charset=utf-8");
    });

    // POST /api/document — 添加文档
    svr.Post("/api/document", [&engine](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Content-Type", "application/json; charset=utf-8");
        if (req.body.empty()) {
            res.status = 400;
            res.set_content("{\"error\":\"empty body\"}", "application/json");
            return;
        }

        // 从 body 中提取字段：title, content, category
        auto extract = [&](const std::string& key) -> std::string {
            std::string pattern = "\"" + key + "\"";
            auto pos = req.body.find(pattern);
            if (pos == std::string::npos) return "";
            pos = req.body.find(':', pos + pattern.size());
            if (pos == std::string::npos) return "";
            pos = req.body.find('"', pos);
            if (pos == std::string::npos) return "";
            auto end = req.body.find('"', pos + 1);
            if (end == std::string::npos) return "";
            // 简易反转义
            std::string val;
            for (size_t i = pos + 1; i < end; i++) {
                if (req.body[i] == '\\' && i + 1 < end) {
                    char next = req.body[i + 1];
                    if (next == '"')      { val += '"';  i++; }
                    else if (next == '\\') { val += '\\'; i++; }
                    else if (next == 'n')  { val += '\n'; i++; }
                    else if (next == 't')  { val += '\t'; i++; }
                    else                    val += req.body[i];
                } else {
                    val += req.body[i];
                }
            }
            return val;
        };

        std::string title = extract("title");
        std::string content = extract("content");
        std::string url = extract("url");
        std::string site = extract("site");
        std::string author = extract("author");
        std::string timestamp = extract("timestamp");
        std::string description = extract("description");
        std::string category = extract("category");

        if (title.empty() && content.empty()) {
            res.status = 400;
            res.set_content("{\"error\":\"title or content required\"}", "application/json");
            return;
        }

        // 生成 doc_id
        static std::atomic<uint64_t> next_id{1000};
        std::string doc_id = std::to_string(next_id.fetch_add(1));

        Document doc;
        doc.fields = {
            {"title",       title.empty() ? "Untitled" : title},
            {"content",     content},
            {"url",         url},
            {"site",        site},
            {"author",      author},
            {"timestamp",   timestamp},
            {"description", description},
            {"doc_id",      doc_id},
            {"category",    category.empty() ? "General" : category},
        };

        auto s = engine->writer->add_document(doc);
        if (!s.ok()) {
            res.status = 400;
            res.set_content("{\"error\":\"" + escape_json(std::string(s.message())) + "\"}", "application/json");
            return;
        }

        engine->refresh_reader();

        res.set_content("{\"id\":\"" + doc_id + "\",\"status\":\"ok\"}", "application/json");
    });

    // GET /api/document/:id — 文档详情
    svr.Get(R"(/api/document/([^/]+))", [&engine](const httplib::Request& req, httplib::Response& res) {
        std::string doc_id = req.matches[1];
        auto reader = engine->get_reader();
        auto doc_opt = reader->get_document(doc_id);

        if (!doc_opt.ok()) {
            res.status = 500;
            res.set_content("{\"error\":\"internal error\"}", "application/json");
            return;
        }

        auto opt_val = doc_opt.move_value();
        if (!opt_val.has_value()) {
            res.status = 404;
            res.set_content("{\"error\":\"not found\"}", "application/json");
            return;
        }

        auto& doc = opt_val.value();
        std::string json = "{\"id\":\"" + escape_json(doc_id) + "\",\"fields\":{";
        bool first = true;
        for (const auto& f : doc.fields) {
            if (!first) json += ",";
            first = false;
            json += "\"" + escape_json(f.name) + "\":\"" + escape_json(f.value) + "\"";
        }
        json += "}}";
        res.set_content(json, "application/json; charset=utf-8");
    });

    std::cout << "Vortex Search Demo running at http://localhost:" << port << std::endl;
    if (!svr.listen("0.0.0.0", port)) {
        std::cerr << "Failed to bind port " << port << std::endl;
        return 1;
    }
    return 0;
}
