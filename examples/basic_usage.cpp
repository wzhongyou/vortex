// Vortex — 三步体验倒排索引
//
// 构建:
//   cmake -B build -DCMAKE_BUILD_TYPE=Release -DVORTEX_BUILD_EXAMPLES=ON
//   cmake --build build -j$(nproc)
//
// 运行:
//   ./build/examples/vortex_example /tmp/vortex_demo

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

#include "vortex/core/document.h"
#include "vortex/core/query.h"
#include "vortex/core/schema.h"
#include "vortex/core/types.h"
#include "vortex/inverted/index_reader.h"
#include "vortex/inverted/index_writer.h"

using namespace vortex;

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : "/tmp/vortex_demo";
    std::filesystem::remove_all(dir);

    // ── Step 1: 建索引 ──
    Schema schema;
    schema.add_field({"title", FieldType::TEXT, true, true});
    schema.add_field({"content", FieldType::TEXT, false, true});
    schema.add_field({"doc_id", FieldType::KEYWORD, true, false});
    schema.add_field({"category", FieldType::KEYWORD, true, false});

    auto writer = IndexWriter::open({dir, std::move(schema), 64, "doc_id"}).move_value();

    auto add = [&](std::string id, std::string title, std::string content, std::string cat) {
        Document doc;
        doc.fields.push_back({"doc_id", id});
        doc.fields.push_back({"title", title});
        doc.fields.push_back({"content", content});
        doc.fields.push_back({"category", cat});
        writer->add_document(doc);
    };
    add("1", "Vortex search engine",   "A fast inverted index written in C++",  "tech");
    add("2", "Python programming",      "Modern Python features and best practices", "tech");
    add("3", "Machine learning basics", "Introduction to neural networks",        "ml");
    add("4", "Deep learning with CNN",  "Convolutional neural networks for vision", "ml");
    add("5", "Healthy recipes",         "Delicious and nutritious meals",         "lifestyle");
    add("6", "Learning the guitar",     "Beginner guide to playing guitar",       "lifestyle");
    writer->flush();
    std::cout << "✓ Step 1: 索引构建完成 (" << dir << ")\n\n";

    // ── Step 2: 搜 ──
    auto reader = writer->get_reader().move_value();
    auto print = [&](const std::string& label, Query q) {
        auto r = reader->search(q, 10).move_value();
        printf("── %s ── hits=%zu\n", label.c_str(), r.total_hits);
        for (auto& d : r.docs)
            printf("  [%s] score=%.2f seg=%u doc_id=%u\n", d.external_id.c_str(), d.score, d.segment_id, d.internal_doc_id);
        printf("\n");
    };

    print("TERM: python",                Query::Term("python"));
    print("AND: neural + networks",      Query::And({Query::Term("neural"), Query::Term("networks")}));
    print("OR:  search | learning",      Query::Or({Query::Term("search"), Query::Term("learning")}));
    print("NOT: learning -neural",       Query::Not(Query::Term("learning"), Query::Term("neural")));
    print("嵌套: (learning|search)&guide", Query::And({Query::Or({Query::Term("learning"), Query::Term("search")}), Query::Term("guide")}));

    // ── Step 3: 看统计 ──
    auto& ws = writer->stats();
    printf("── Step 3: 统计 ──\n");
    printf("  docs_added=%llu  flushes=%llu  segments=%llu  memory=%llu bytes  disk=%llu bytes\n",
           ws.docs_added.get(), ws.flushes.get(), ws.segment_count.get(),
           ws.memory_bytes.get(), ws.disk_bytes.get());

    std::filesystem::remove_all(dir);
    printf("\n✓ Done (临时索引已清理)\n");
    return 0;
}