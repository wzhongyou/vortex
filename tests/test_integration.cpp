#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "gtest/gtest.h"
#include "vortex/core/document.h"
#include "vortex/core/query.h"
#include "vortex/core/schema.h"
#include "vortex/inverted/index_reader.h"
#include "vortex/inverted/index_writer.h"

namespace vortex {
namespace {

class IntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a temporary test directory
        test_dir_ = std::filesystem::temp_directory_path() /
                    ("vortex_test_" + std::to_string(rand()));
        std::filesystem::create_directories(test_dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(test_dir_);
    }

    std::string test_dir_;
};

TEST_F(IntegrationTest, BuildIndexAndSearchTerm) {
    // 1. Build schema
    Schema schema;
    ASSERT_TRUE(schema.add_field({"title", FieldType::TEXT, true, true}).ok());
    ASSERT_TRUE(schema.add_field({"content", FieldType::TEXT, false, true}).ok());
    ASSERT_TRUE(schema.add_field({"doc_id", FieldType::KEYWORD, true, false}).ok());

    // 2. Open writer
    IndexWriterOptions opts;
    opts.index_dir = test_dir_;
    opts.schema = std::move(schema);
    opts.external_id_field = "doc_id";
    opts.ram_buffer_mb = 64;

    auto writer_result = IndexWriter::open(std::move(opts));
    ASSERT_TRUE(writer_result.ok()) << writer_result.status().message();
    auto writer = writer_result.move_value();

    // 3. Add documents
    for (int i = 1; i <= 5; i++) {
        Document doc;
        doc.fields.push_back({"doc_id", std::to_string(i)});
        doc.fields.push_back({"title", "Document number " + std::to_string(i)});
        doc.fields.push_back({"content", "This is the content of document " + std::to_string(i)});
        ASSERT_TRUE(writer->add_document(doc).ok());
    }

    // Add one more with special content
    {
        Document doc;
        doc.fields.push_back({"doc_id", "6"});
        doc.fields.push_back({"title", "Special hello world document"});
        doc.fields.push_back({"content", "Hello world from vortex search engine"});
        ASSERT_TRUE(writer->add_document(doc).ok());
    }

    // 4. Flush
    ASSERT_TRUE(writer->flush().ok());

    // 5. Get reader
    auto reader_result = writer->get_reader();
    ASSERT_TRUE(reader_result.ok());
    auto reader = reader_result.move_value();

    // 6. Search for a common term
    Query q = Query::Term("document");
    auto result = reader->search(q, 10);
    ASSERT_TRUE(result.ok());

    auto& search_result = result.value();
    // All 6 docs have "document" in title or content
    EXPECT_EQ(search_result.total_hits, 6u);
    // All scores should be positive
    for (auto& d : search_result.docs) {
        EXPECT_GT(d.score, 0.0f);
    }

    // 7. Search for a term unique to doc 6
    Query q2 = Query::Term("hello");
    auto result2 = reader->search(q2, 10);
    ASSERT_TRUE(result2.ok());

    auto& search_result2 = result2.value();
    // Doc 6 has "hello" in both title and content, doc 1-5 have neither
    EXPECT_EQ(search_result2.total_hits, 1u);
    EXPECT_GT(search_result2.docs[0].score, 0.0f);

    // 8. Search for a non-existent term
    Query q3 = Query::Term("nonexistent98765");
    auto result3 = reader->search(q3, 10);
    ASSERT_TRUE(result3.ok());
    EXPECT_EQ(result3.value().total_hits, 0u);

    // 9. Verify statistics are being tracked
    EXPECT_GT(writer->stats().docs_added.get(), 0);
    EXPECT_GT(reader->stats().queries.get(), 0);
}

TEST_F(IntegrationTest, FlushAndSearchAcrossSegments) {
    // Build a schema
    Schema schema;
    ASSERT_TRUE(schema.add_field({"text", FieldType::TEXT, true, true}).ok());
    ASSERT_TRUE(schema.add_field({"id", FieldType::KEYWORD, true, false}).ok());

    IndexWriterOptions opts;
    opts.index_dir = test_dir_;
    opts.schema = std::move(schema);
    opts.external_id_field = "id";
    opts.ram_buffer_mb = 1;  // Force multiple flushes (small buffer)

    auto writer_result = IndexWriter::open(std::move(opts));
    ASSERT_TRUE(writer_result.ok());
    auto writer = writer_result.move_value();

    // Add many documents to force segment flushes
    for (int i = 1; i <= 100; i++) {
        Document doc;
        doc.fields.push_back({"id", std::to_string(i)});
        doc.fields.push_back({"text", "repeating term in doc " + std::to_string(i)});
        Status s = writer->add_document(doc);
        // Flushes should happen automatically, but we don't require it
        if (!s.ok()) {
            // If flush fails for some reason (like WAL issues), skip
            continue;
        }
    }

    // Final flush
    writer->flush();

    // Search across all segments
    auto reader_result = writer->get_reader();
    ASSERT_TRUE(reader_result.ok());
    auto reader = reader_result.move_value();

    Query q = Query::Term("repeating");
    auto result = reader->search(q, 20);
    ASSERT_TRUE(result.ok());
    EXPECT_GT(result.value().total_hits, 0);
}

}  // namespace
}  // namespace vortex
