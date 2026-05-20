#include <cstdio>
#include <filesystem>
#include <string>

#include <unistd.h>

#include <benchmark/benchmark.h>

#include "vortex/core/document.h"
#include "vortex/core/query.h"
#include "vortex/core/schema.h"
#include "vortex/inverted/index_reader.h"
#include "vortex/inverted/index_writer.h"

using namespace vortex;

class SearchBench : public benchmark::Fixture {
public:
    void SetUp(const ::benchmark::State&) override {
        // Create temp directory
        char dir_template[] = "/tmp/vortex_bench_XXXXXX";
        char* dir = mkdtemp(dir_template);
        index_dir_ = dir;

        // Build schema
        Schema schema;
        schema.add_field({"title", FieldType::TEXT, true, true});
        schema.add_field({"content", FieldType::TEXT, false, true});
        schema.add_field({"doc_id", FieldType::KEYWORD, true, false});

        // Open writer
        IndexWriterOptions opts;
        opts.index_dir = index_dir_;
        opts.schema = std::move(schema);
        opts.external_id_field = "doc_id";
        opts.ram_buffer_mb = 64;

        auto writer_result = IndexWriter::open(std::move(opts));
        writer_ = writer_result.move_value();

        // Add documents
        for (int i = 0; i < kNumDocs; i++) {
            Document doc;
            doc.fields.push_back({"doc_id", std::to_string(i)});
            doc.fields.push_back({"title", "Document " + std::to_string(i) +
                                  " has some searchable content here"});
            doc.fields.push_back({"content", "This is the body of document " +
                                  std::to_string(i) +
                                  ". It contains terms like apple banana cherry."});
            writer_->add_document(doc);
        }

        // Flush and get reader
        writer_->flush();
        auto reader_result = writer_->get_reader();
        reader_ = reader_result.move_value();
    }

    void TearDown(const ::benchmark::State&) override {
        reader_.reset();
        writer_.reset();
        std::filesystem::remove_all(index_dir_);
    }

    static constexpr int kNumDocs = 5000;
    std::string index_dir_;
    std::unique_ptr<IndexWriter> writer_;
    std::shared_ptr<IndexReader> reader_;
};

BENCHMARK_DEFINE_F(SearchBench, TermSearchCommon)(benchmark::State& state) {
    Query q = Query::Term("document");
    for (auto _ : state) {
        auto result = reader_->search(q, 10);
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK_REGISTER_F(SearchBench, TermSearchCommon)->Iterations(100);

BENCHMARK_DEFINE_F(SearchBench, TermSearchRare)(benchmark::State& state) {
    Query q = Query::Term("apple");
    for (auto _ : state) {
        auto result = reader_->search(q, 10);
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK_REGISTER_F(SearchBench, TermSearchRare)->Iterations(100);

BENCHMARK_DEFINE_F(SearchBench, TermSearchNonexistent)(benchmark::State& state) {
    Query q = Query::Term("zzzznonexistent");
    for (auto _ : state) {
        auto result = reader_->search(q, 10);
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK_REGISTER_F(SearchBench, TermSearchNonexistent)->Iterations(100);

BENCHMARK_MAIN();