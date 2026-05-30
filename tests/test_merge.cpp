#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "gtest/gtest.h"
#include "vortex/core/arena.h"
#include "vortex/core/document.h"
#include "vortex/core/query.h"
#include "vortex/core/schema.h"
#include "vortex/inverted/index_reader.h"
#include "vortex/inverted/segment.h"
#include "vortex/inverted/segment_list.h"
#include "vortex/inverted/segment_merger.h"
#include "vortex/inverted/segment.h"
#include "vortex/inverted/term_dict.h"

namespace vortex {
namespace {

class MergeTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = std::filesystem::temp_directory_path() /
                    ("vortex_merge_test_" + std::to_string(rand()));
        std::filesystem::create_directories(test_dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(test_dir_);
    }

    std::string test_dir_;
};

// Helper: create a MemorySegment with given docs and flush it
static Result<std::shared_ptr<const Segment>> make_segment(
    const std::string& seg_dir,
    uint64_t seg_id,
    uint16_t indexed_field_count,
    uint16_t stored_field_count,
    const std::vector<std::tuple<std::string, std::string, std::string>>& docs, // (id, title, content)
    Arena& arena) {

    MemorySegment seg(seg_id, indexed_field_count);
    uint32_t internal_id = 0;

    for (auto& [id, title, content] : docs) {
        uint32_t doc_length = 0;
        std::vector<uint32_t> field_lengths(indexed_field_count, 0);
        std::unordered_map<std::string, uint32_t> term_freq;

        // Simple analyzer sim: split on whitespace, lowercase
        auto count_terms = [&](const std::string& text, uint16_t field_pos) {
            std::string current;
            for (size_t i = 0; i <= text.size(); i++) {
                if (i < text.size() && text[i] != ' ') {
                    current += static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
                } else if (!current.empty()) {
                    doc_length++;
                    field_lengths[field_pos]++;
                    current.clear();
                }
            }
        };

        count_terms(title, 0);
        count_terms(content, 1);

        // Re-tokenize to get per-document term frequencies (aggregated)
        {
            std::string current;
            auto tokenize = [&](const std::string& text) {
                for (size_t i = 0; i <= text.size(); i++) {
                    if (i < text.size() && text[i] != ' ') {
                        current += static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
                    } else if (!current.empty()) {
                        term_freq[current]++;
                        current.clear();
                    }
                }
            };
            tokenize(title);
            tokenize(content);
        }

        // Add aggregated terms — once per term per doc (like real IndexWriter)
        for (auto& pair : term_freq) {
            seg.add_term(internal_id, pair.first, pair.second);
        }

        seg.add_doc_info(doc_length, field_lengths);
        seg.add_external_id(id);

        std::vector<std::string> stored_values;
        stored_values.push_back(id);
        stored_values.push_back(title);
        seg.add_stored_values(std::move(stored_values));

        internal_id++;
    }

    return seg.flush(seg_dir, arena);
}

TEST_F(MergeTest, MergeTwoSegments) {
    Arena arena;
    uint16_t indexed_fc = 2;  // title, content
    uint16_t stored_fc = 2;   // id, title

    auto r1 = make_segment(test_dir_, 1, indexed_fc, stored_fc, {
        {"1", "Apple pie recipe", "Best apple pie content"},
        {"2", "Apple juice",      "Fresh apple juice"},
    }, arena);
    ASSERT_TRUE(r1.ok());
    auto seg1 = r1.move_value();

    auto r2 = make_segment(test_dir_, 2, indexed_fc, stored_fc, {
        {"3", "Banana bread",   "Easy banana bread"},
        {"4", "Banana smoothie","Healthy banana smoothie"},
    }, arena);
    ASSERT_TRUE(r2.ok());
    auto seg2 = r2.move_value();

    // ── Merge ──
    auto merge_result = SegmentMerger::merge(
        test_dir_, {seg1, seg2}, 100, stored_fc, arena);
    ASSERT_TRUE(merge_result.ok()) << merge_result.status().message();
    auto merged = merge_result.move_value();

    // Verify merged segment properties
    EXPECT_EQ(merged->id(), 100u);
    EXPECT_EQ(merged->doc_count(), 4u);

    // Verify term dictionary is well-formed
    EXPECT_NE(merged->term_dict(), nullptr);
    EXPECT_NE(merged->lookup_term("apple"), nullptr);
    EXPECT_NE(merged->lookup_term("banana"), nullptr);
    EXPECT_NE(merged->lookup_term("juice"), nullptr);
    EXPECT_NE(merged->lookup_term("bread"), nullptr);
    EXPECT_EQ(merged->lookup_term("nonexistent"), nullptr);

    // Verify term stats
    EXPECT_EQ(merged->lookup_term("apple")->doc_freq, 2u);
    EXPECT_EQ(merged->lookup_term("banana")->doc_freq, 2u);
    EXPECT_EQ(merged->lookup_term("juice")->doc_freq, 1u);
    EXPECT_EQ(merged->lookup_term("bread")->doc_freq, 1u);

    // Verify external ID resolution
    EXPECT_EQ(merged->resolve_external_id(0), "1");
    EXPECT_EQ(merged->resolve_external_id(1), "2");
    EXPECT_EQ(merged->resolve_external_id(2), "3");
    EXPECT_EQ(merged->resolve_external_id(3), "4");
    EXPECT_EQ(merged->find_doc_id("1"), 0u);
    EXPECT_EQ(merged->find_doc_id("2"), 1u);
    EXPECT_EQ(merged->find_doc_id("3"), 2u);
    EXPECT_EQ(merged->find_doc_id("4"), 3u);
    EXPECT_EQ(merged->find_doc_id("nonexistent"), kInvalidDocId);

    // Verify stored values
    {
        std::vector<std::string> values;
        merged->get_stored_values(0, values, stored_fc);
        ASSERT_EQ(values.size(), 2u);
        EXPECT_EQ(values[0], "1");
        EXPECT_EQ(values[1], "Apple pie recipe");
    }
    {
        std::vector<std::string> values;
        merged->get_stored_values(2, values, stored_fc);
        ASSERT_EQ(values.size(), 2u);
        EXPECT_EQ(values[0], "3");
        EXPECT_EQ(values[1], "Banana bread");
    }
    {
        std::vector<std::string> values;
        merged->get_stored_values(3, values, stored_fc);
        ASSERT_EQ(values.size(), 2u);
        EXPECT_EQ(values[0], "4");
        EXPECT_EQ(values[1], "Banana smoothie");
    }
}

TEST_F(MergeTest, MergePreservesSearchResults) {
    Arena arena;
    uint16_t indexed_fc = 2;
    uint16_t stored_fc = 2;

    auto r1 = make_segment(test_dir_, 1, indexed_fc, stored_fc, {
        {"1", "Apple pie recipe", "Best apple pie content"},
        {"2", "Apple juice",      "Fresh apple juice"},
    }, arena);
    ASSERT_TRUE(r1.ok());

    auto r2 = make_segment(test_dir_, 2, indexed_fc, stored_fc, {
        {"3", "Banana bread",   "Easy banana bread"},
        {"4", "Banana smoothie","Healthy banana smoothie"},
    }, arena);
    ASSERT_TRUE(r2.ok());

    // Merge
    auto mr = SegmentMerger::merge(test_dir_, {r1.value(), r2.value()}, 100, stored_fc, arena);
    ASSERT_TRUE(mr.ok());
    auto merged = mr.move_value();

    // Build a SegmentList with only the merged segment
    SegmentList seg_list;
    seg_list.publish_segment(merged);

    // Build a schema for the reader
    Schema schema;
    schema.add_field({"id", FieldType::KEYWORD, true, false});
    schema.add_field({"title", FieldType::TEXT, true, true});
    schema.add_field({"content", FieldType::TEXT, false, true});

    auto* snap = seg_list.acquire_snapshot();
    IndexReader reader(const_cast<SegmentsSnapshot*>(snap), &seg_list, &schema);

    // Search "apple" → 2 hits (docs 1, 2)
    auto r = reader.search(Query::Term("apple"), 10);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().total_hits, 2u);
    for (auto& d : r.value().docs) {
        EXPECT_GT(d.score, 0.0f);
    }

    // Search "banana" → 2 hits (docs 3, 4)
    r = reader.search(Query::Term("banana"), 10);
    EXPECT_EQ(r.value().total_hits, 2u);

    // Search "pie" → 1 hit (doc 1)
    r = reader.search(Query::Term("pie"), 10);
    EXPECT_EQ(r.value().total_hits, 1u);
    EXPECT_EQ(r.value().docs[0].external_id, "1");

    // Search "smoothie" → 1 hit (doc 4)
    r = reader.search(Query::Term("smoothie"), 10);
    EXPECT_EQ(r.value().total_hits, 1u);
    EXPECT_EQ(r.value().docs[0].external_id, "4");

    // AND query across merged terms
    r = reader.search(Query::And({Query::Term("apple"), Query::Term("juice")}), 10);
    EXPECT_EQ(r.value().total_hits, 1u);

    // OR query
    r = reader.search(Query::Or({Query::Term("apple"), Query::Term("banana")}), 10);
    EXPECT_EQ(r.value().total_hits, 4u);

    // get_document on merged segment
    auto doc_opt = reader.get_document("3").move_value();
    ASSERT_TRUE(doc_opt.has_value());
    bool found_title = false, found_id = false;
    for (auto& fv : doc_opt.value().fields) {
        if (fv.name == "id") { EXPECT_EQ(fv.value, "3"); found_id = true; }
        if (fv.name == "title") { EXPECT_EQ(fv.value, "Banana bread"); found_title = true; }
    }
    EXPECT_TRUE(found_id);
    EXPECT_TRUE(found_title);

    seg_list.release_snapshot(const_cast<SegmentsSnapshot*>(snap));
}

TEST_F(MergeTest, MergeThreeSegments) {
    Arena arena;
    uint16_t indexed_fc = 2;
    uint16_t stored_fc = 2;

    auto r1 = make_segment(test_dir_, 1, indexed_fc, stored_fc, {
        {"1", "Alpha", "First letter"},
    }, arena);
    ASSERT_TRUE(r1.ok());

    auto r2 = make_segment(test_dir_, 2, indexed_fc, stored_fc, {
        {"2", "Beta", "Second letter"},
        {"3", "Beta plus", "Another beta"},
    }, arena);
    ASSERT_TRUE(r2.ok());

    auto r3 = make_segment(test_dir_, 3, indexed_fc, stored_fc, {
        {"4", "Gamma", "Third letter"},
    }, arena);
    ASSERT_TRUE(r3.ok());

    auto mr = SegmentMerger::merge(
        test_dir_, {r1.value(), r2.value(), r3.value()}, 200, stored_fc, arena);
    ASSERT_TRUE(mr.ok());
    auto merged = mr.move_value();

    EXPECT_EQ(merged->doc_count(), 4u);

    // Verify external IDs in order
    EXPECT_EQ(merged->resolve_external_id(0), "1");
    EXPECT_EQ(merged->resolve_external_id(1), "2");
    EXPECT_EQ(merged->resolve_external_id(2), "3");
    EXPECT_EQ(merged->resolve_external_id(3), "4");

    // Verify terms: "alpha" in doc 0, "beta" in docs 1,2, "gamma" in doc 3
    EXPECT_NE(merged->lookup_term("alpha"), nullptr);
    EXPECT_EQ(merged->lookup_term("alpha")->doc_freq, 1u);
    EXPECT_NE(merged->lookup_term("beta"), nullptr);
    EXPECT_EQ(merged->lookup_term("beta")->doc_freq, 2u);
    EXPECT_NE(merged->lookup_term("gamma"), nullptr);
    EXPECT_EQ(merged->lookup_term("gamma")->doc_freq, 1u);

    // Verify "first" is only in doc 0
    EXPECT_NE(merged->lookup_term("first"), nullptr);
    EXPECT_EQ(merged->lookup_term("first")->doc_freq, 1u);

    // Search verification via SegmentList + IndexReader
    SegmentList seg_list;
    seg_list.publish_segment(merged);
    Schema schema;
    schema.add_field({"id", FieldType::KEYWORD, true, false});
    schema.add_field({"title", FieldType::TEXT, true, true});
    schema.add_field({"content", FieldType::TEXT, false, true});

    auto* snap = seg_list.acquire_snapshot();
    IndexReader reader(const_cast<SegmentsSnapshot*>(snap), &seg_list, &schema);

    auto r = reader.search(Query::Or({Query::Term("alpha"), Query::Term("beta"), Query::Term("gamma")}), 10);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().total_hits, 4u);

    // Search term that only appears in one segment's doc
    r = reader.search(Query::Term("third"), 10);
    EXPECT_EQ(r.value().total_hits, 1u);
    EXPECT_EQ(r.value().docs[0].external_id, "4");

    seg_list.release_snapshot(const_cast<SegmentsSnapshot*>(snap));
}

TEST_F(MergeTest, MergeSegmentWithOverlappingTerms) {
    Arena arena;
    uint16_t indexed_fc = 2;
    uint16_t stored_fc = 2;

    // Both segments contain the term "common"
    auto r1 = make_segment(test_dir_, 1, indexed_fc, stored_fc, {
        {"1", "Common apple", "Common fruit apple"},
    }, arena);
    ASSERT_TRUE(r1.ok());

    auto r2 = make_segment(test_dir_, 2, indexed_fc, stored_fc, {
        {"2", "Common banana", "Common fruit banana"},
    }, arena);
    ASSERT_TRUE(r2.ok());

    auto mr = SegmentMerger::merge(
        test_dir_, {r1.value(), r2.value()}, 300, stored_fc, arena);
    ASSERT_TRUE(mr.ok());
    auto merged = mr.move_value();

    // "common" appears in both docs
    ASSERT_NE(merged->lookup_term("common"), nullptr);
    EXPECT_EQ(merged->lookup_term("common")->doc_freq, 2u);

    // "apple" only in doc 0
    ASSERT_NE(merged->lookup_term("apple"), nullptr);
    EXPECT_EQ(merged->lookup_term("apple")->doc_freq, 1u);

    // "banana" only in doc 1
    ASSERT_NE(merged->lookup_term("banana"), nullptr);
    EXPECT_EQ(merged->lookup_term("banana")->doc_freq, 1u);
}

}  // namespace
}  // namespace vortex