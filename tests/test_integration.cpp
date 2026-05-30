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
    Schema schema;
    ASSERT_TRUE(schema.add_field({"title", FieldType::TEXT, true, true}).ok());
    ASSERT_TRUE(schema.add_field({"content", FieldType::TEXT, false, true}).ok());
    ASSERT_TRUE(schema.add_field({"doc_id", FieldType::KEYWORD, true, false}).ok());

    IndexWriterOptions opts;
    opts.index_dir = test_dir_;
    opts.schema = std::move(schema);
    opts.external_id_field = "doc_id";
    opts.ram_buffer_mb = 64;

    auto writer_result = IndexWriter::open(std::move(opts));
    ASSERT_TRUE(writer_result.ok()) << writer_result.status().message();
    auto writer = writer_result.move_value();

    for (int i = 1; i <= 5; i++) {
        Document doc;
        doc.fields.push_back({"doc_id", std::to_string(i)});
        doc.fields.push_back({"title", "Document number " + std::to_string(i)});
        doc.fields.push_back({"content", "This is the content of document " + std::to_string(i)});
        ASSERT_TRUE(writer->add_document(doc).ok());
    }

    {
        Document doc;
        doc.fields.push_back({"doc_id", "6"});
        doc.fields.push_back({"title", "Special hello world document"});
        doc.fields.push_back({"content", "Hello world from vortex search engine"});
        ASSERT_TRUE(writer->add_document(doc).ok());
    }

    ASSERT_TRUE(writer->flush().ok());

    auto reader_result = writer->get_reader();
    ASSERT_TRUE(reader_result.ok());
    auto reader = reader_result.move_value();

    Query q = Query::Term("document");
    auto result = reader->search(q, 10);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().total_hits, 6u);
    for (auto& d : result.value().docs) {
        EXPECT_GT(d.score, 0.0f);
    }

    Query q2 = Query::Term("hello");
    auto result2 = reader->search(q2, 10);
    ASSERT_TRUE(result2.ok());
    EXPECT_EQ(result2.value().total_hits, 1u);
    EXPECT_GT(result2.value().docs[0].score, 0.0f);

    Query q3 = Query::Term("nonexistent98765");
    auto result3 = reader->search(q3, 10);
    ASSERT_TRUE(result3.ok());
    EXPECT_EQ(result3.value().total_hits, 0u);

    EXPECT_GT(writer->stats().docs_added.get(), 0);
    EXPECT_GT(reader->stats().queries.get(), 0);
}

TEST_F(IntegrationTest, FlushAndSearchAcrossSegments) {
    Schema schema;
    ASSERT_TRUE(schema.add_field({"text", FieldType::TEXT, true, true}).ok());
    ASSERT_TRUE(schema.add_field({"id", FieldType::KEYWORD, true, false}).ok());

    IndexWriterOptions opts;
    opts.index_dir = test_dir_;
    opts.schema = std::move(schema);
    opts.external_id_field = "id";
    opts.ram_buffer_mb = 1;

    auto writer_result = IndexWriter::open(std::move(opts));
    ASSERT_TRUE(writer_result.ok());
    auto writer = writer_result.move_value();

    for (int i = 1; i <= 100; i++) {
        Document doc;
        doc.fields.push_back({"id", std::to_string(i)});
        doc.fields.push_back({"text", "repeating term in doc " + std::to_string(i)});
        Status s = writer->add_document(doc);
        if (!s.ok()) continue;
    }

    writer->flush();

    auto reader_result = writer->get_reader();
    ASSERT_TRUE(reader_result.ok());
    auto reader = reader_result.move_value();

    Query q = Query::Term("repeating");
    auto result = reader->search(q, 20);
    ASSERT_TRUE(result.ok());
    EXPECT_GT(result.value().total_hits, 0);
}

TEST_F(IntegrationTest, AndQuery) {
    Schema schema;
    ASSERT_TRUE(schema.add_field({"text", FieldType::TEXT, true, true}).ok());
    ASSERT_TRUE(schema.add_field({"id", FieldType::KEYWORD, true, false}).ok());

    IndexWriterOptions opts;
    opts.index_dir = test_dir_;
    opts.schema = std::move(schema);
    opts.external_id_field = "id";
    opts.ram_buffer_mb = 64;

    auto writer_result = IndexWriter::open(std::move(opts));
    ASSERT_TRUE(writer_result.ok());
    auto writer = writer_result.move_value();

    {
        Document doc;
        doc.fields.push_back({"id", "1"});
        doc.fields.push_back({"text", "hello world"});
        ASSERT_TRUE(writer->add_document(doc).ok());
    }
    {
        Document doc;
        doc.fields.push_back({"id", "2"});
        doc.fields.push_back({"text", "hello everyone"});
        ASSERT_TRUE(writer->add_document(doc).ok());
    }
    {
        Document doc;
        doc.fields.push_back({"id", "3"});
        doc.fields.push_back({"text", "world peace"});
        ASSERT_TRUE(writer->add_document(doc).ok());
    }
    {
        Document doc;
        doc.fields.push_back({"id", "4"});
        doc.fields.push_back({"text", "foo bar"});
        ASSERT_TRUE(writer->add_document(doc).ok());
    }

    ASSERT_TRUE(writer->flush().ok());
    auto reader_result = writer->get_reader();
    ASSERT_TRUE(reader_result.ok());
    auto reader = reader_result.move_value();

    // AND(hello, world) → only doc 1
    Query q = Query::And({Query::Term("hello"), Query::Term("world")});
    auto result = reader->search(q, 10);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().total_hits, 1u);
    EXPECT_EQ(result.value().docs[0].external_id, "1");

    // AND(hello, foo) → no docs (hello in 1,2; foo in 4; no overlap)
    Query q2 = Query::And({Query::Term("hello"), Query::Term("foo")});
    auto result2 = reader->search(q2, 10);
    ASSERT_TRUE(result2.ok());
    EXPECT_EQ(result2.value().total_hits, 0u);
}

TEST_F(IntegrationTest, OrQuery) {
    Schema schema;
    ASSERT_TRUE(schema.add_field({"text", FieldType::TEXT, true, true}).ok());
    ASSERT_TRUE(schema.add_field({"id", FieldType::KEYWORD, true, false}).ok());

    IndexWriterOptions opts;
    opts.index_dir = test_dir_;
    opts.schema = std::move(schema);
    opts.external_id_field = "id";
    opts.ram_buffer_mb = 64;

    auto writer_result = IndexWriter::open(std::move(opts));
    ASSERT_TRUE(writer_result.ok());
    auto writer = writer_result.move_value();

    {
        Document doc;
        doc.fields.push_back({"id", "1"});
        doc.fields.push_back({"text", "hello world"});
        ASSERT_TRUE(writer->add_document(doc).ok());
    }
    {
        Document doc;
        doc.fields.push_back({"id", "2"});
        doc.fields.push_back({"text", "hello everyone"});
        ASSERT_TRUE(writer->add_document(doc).ok());
    }
    {
        Document doc;
        doc.fields.push_back({"id", "3"});
        doc.fields.push_back({"text", "world peace"});
        ASSERT_TRUE(writer->add_document(doc).ok());
    }

    ASSERT_TRUE(writer->flush().ok());
    auto reader_result = writer->get_reader();
    ASSERT_TRUE(reader_result.ok());
    auto reader = reader_result.move_value();

    // OR(hello, world) → docs 1, 2, 3
    Query q = Query::Or({Query::Term("hello"), Query::Term("world")});
    auto result = reader->search(q, 10);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().total_hits, 3u);

    // OR(hello, nonexistent) → docs 1, 2
    Query q2 = Query::Or({Query::Term("hello"), Query::Term("nonexistent999")});
    auto result2 = reader->search(q2, 10);
    ASSERT_TRUE(result2.ok());
    EXPECT_EQ(result2.value().total_hits, 2u);
}

TEST_F(IntegrationTest, NotQuery) {
    Schema schema;
    ASSERT_TRUE(schema.add_field({"text", FieldType::TEXT, true, true}).ok());
    ASSERT_TRUE(schema.add_field({"id", FieldType::KEYWORD, true, false}).ok());

    IndexWriterOptions opts;
    opts.index_dir = test_dir_;
    opts.schema = std::move(schema);
    opts.external_id_field = "id";
    opts.ram_buffer_mb = 64;

    auto writer_result = IndexWriter::open(std::move(opts));
    ASSERT_TRUE(writer_result.ok());
    auto writer = writer_result.move_value();

    {
        Document doc;
        doc.fields.push_back({"id", "1"});
        doc.fields.push_back({"text", "hello world"});
        ASSERT_TRUE(writer->add_document(doc).ok());
    }
    {
        Document doc;
        doc.fields.push_back({"id", "2"});
        doc.fields.push_back({"text", "hello everyone"});
        ASSERT_TRUE(writer->add_document(doc).ok());
    }
    {
        Document doc;
        doc.fields.push_back({"id", "3"});
        doc.fields.push_back({"text", "world peace"});
        ASSERT_TRUE(writer->add_document(doc).ok());
    }

    ASSERT_TRUE(writer->flush().ok());
    auto reader_result = writer->get_reader();
    ASSERT_TRUE(reader_result.ok());
    auto reader = reader_result.move_value();

    // NOT(hello, world) → docs with "hello" but not "world" → doc 2
    Query q = Query::Not(Query::Term("hello"), Query::Term("world"));
    auto result = reader->search(q, 10);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().total_hits, 1u);
    EXPECT_EQ(result.value().docs[0].external_id, "2");

    // Unary NOT returns nothing
    Query q2 = Query::Not(Query::Term("hello"));
    auto result2 = reader->search(q2, 10);
    ASSERT_TRUE(result2.ok());
    EXPECT_EQ(result2.value().total_hits, 0u);
}

TEST_F(IntegrationTest, NestedBooleanQuery) {
    Schema schema;
    ASSERT_TRUE(schema.add_field({"text", FieldType::TEXT, true, true}).ok());
    ASSERT_TRUE(schema.add_field({"id", FieldType::KEYWORD, true, false}).ok());

    IndexWriterOptions opts;
    opts.index_dir = test_dir_;
    opts.schema = std::move(schema);
    opts.external_id_field = "id";
    opts.ram_buffer_mb = 64;

    auto writer_result = IndexWriter::open(std::move(opts));
    ASSERT_TRUE(writer_result.ok());
    auto writer = writer_result.move_value();

    // Doc 1: apple + banana + cherry
    {
        Document doc;
        doc.fields.push_back({"id", "1"});
        doc.fields.push_back({"text", "apple banana cherry"});
        ASSERT_TRUE(writer->add_document(doc).ok());
    }
    // Doc 2: apple + banana
    {
        Document doc;
        doc.fields.push_back({"id", "2"});
        doc.fields.push_back({"text", "apple banana"});
        ASSERT_TRUE(writer->add_document(doc).ok());
    }
    // Doc 3: cherry only
    {
        Document doc;
        doc.fields.push_back({"id", "3"});
        doc.fields.push_back({"text", "cherry"});
        ASSERT_TRUE(writer->add_document(doc).ok());
    }
    // Doc 4: nothing
    {
        Document doc;
        doc.fields.push_back({"id", "4"});
        doc.fields.push_back({"text", "nothing"});
        ASSERT_TRUE(writer->add_document(doc).ok());
    }

    ASSERT_TRUE(writer->flush().ok());
    auto reader_result = writer->get_reader();
    ASSERT_TRUE(reader_result.ok());
    auto reader = reader_result.move_value();

    // AND(OR(apple, cherry), banana) → docs with (apple OR cherry) AND banana
    // Doc 1: match, Doc 2: match, Doc 3: no banana, Doc 4: no
    Query inner = Query::Or({Query::Term("apple"), Query::Term("cherry")});
    Query q = Query::And({std::move(inner), Query::Term("banana")});
    auto result = reader->search(q, 10);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().total_hits, 2u);

    // AND(apple, OR(banana, cherry))
    // Doc 1: match, Doc 2: match, Doc 3: no apple, Doc 4: no
    Query inner2 = Query::Or({Query::Term("banana"), Query::Term("cherry")});
    Query q2 = Query::And({Query::Term("apple"), std::move(inner2)});
    auto result2 = reader->search(q2, 10);
    ASSERT_TRUE(result2.ok());
    EXPECT_EQ(result2.value().total_hits, 2u);
}

}  // namespace
}  // namespace vortex