#include <vector>

#include "gtest/gtest.h"
#include "vortex/inverted/skip_list.h"

namespace vortex {
namespace {

TEST(SkipListTest, EmptyList) {
    SkipList sl;
    EXPECT_TRUE(sl.empty());
    EXPECT_EQ(sl.level_count(), 0u);
}

TEST(SkipListTest, SingleLevelBuild) {
    SkipList sl;
    std::vector<uint64_t> offsets = {0, 100, 200, 300, 400};
    std::vector<uint32_t> max_docs = {50, 100, 150, 200, 250};
    sl.build(offsets, max_docs, 2);

    EXPECT_FALSE(sl.empty());
    // 2 levels: step=2 and step=4 (both < 5 blocks)
    EXPECT_EQ(sl.level_count(), 2u);

    // Advance within first block range
    uint64_t pos = sl.advance(10, 0);
    EXPECT_EQ(pos, 0u);  // first group (blocks 0-1, offset 0) has max_doc >= 10

    // Advance to second group
    pos = sl.advance(120, 0);
    EXPECT_EQ(pos, 200u);  // second group (blocks 2-3, offset 200) has max_doc >= 120

    // Advance beyond all
    pos = sl.advance(999, 0);
    EXPECT_EQ(pos, UINT64_MAX);
}

TEST(SkipListTest, MultiLevelBuild) {
    SkipList sl;
    // 20 blocks, interval 4 → level 0: 5 entries, level 1: 1-2 entries
    std::vector<uint64_t> offsets;
    std::vector<uint32_t> max_docs;
    for (int i = 0; i < 20; i++) {
        offsets.push_back(static_cast<uint64_t>(i) * 100);
        max_docs.push_back((i + 1) * 30);
    }
    sl.build(offsets, max_docs, 4);

    EXPECT_GE(sl.level_count(), 2u);
}

TEST(SkipListTest, SerializeRoundtrip) {
    SkipList sl;
    std::vector<uint64_t> offsets = {0, 50, 100, 150, 200, 250, 300, 350};
    std::vector<uint32_t> max_docs = {10, 20, 30, 40, 50, 60, 70, 80};
    sl.build(offsets, max_docs, 3);

    auto data = sl.serialize();
    EXPECT_FALSE(data.empty());

    auto restored = SkipList::deserialize(data.data(), data.size());
    EXPECT_FALSE(restored.empty());

    // Check that advance gives the same results
    EXPECT_EQ(sl.advance(5, 0), restored.advance(5, 0));
    EXPECT_EQ(sl.advance(35, 0), restored.advance(35, 0));
    EXPECT_EQ(sl.advance(55, 0), restored.advance(55, 0));
    EXPECT_EQ(sl.advance(999, 0), restored.advance(999, 0));
}

TEST(SkipListTest, AdvanceWithCurrentOffset) {
    SkipList sl;
    std::vector<uint64_t> offsets = {0, 100, 200, 300, 400, 500};
    std::vector<uint32_t> max_docs = {50, 100, 150, 200, 250, 300};
    sl.build(offsets, max_docs, 2);

    // Advance starting from the middle
    uint64_t pos = sl.advance(180, 200);
    EXPECT_EQ(pos, 200u);  // current block already has max_doc=150, need one with >=180

    pos = sl.advance(999, 400);
    EXPECT_EQ(pos, UINT64_MAX);
}

}  // namespace
}  // namespace vortex