#include "gtest/gtest.h"
#include "vortex/inverted/term_dict.h"

namespace vortex {
namespace {

TEST(TermDictTest, BuildAndLookup) {
    TermDictBuilder builder;
    builder.insert("apple", TermInfo{5, 100, 20});
    builder.insert("banana", TermInfo{3, 200, 30});
    builder.insert("cherry", TermInfo{7, 300, 40});

    auto fst = builder.finish();
    EXPECT_FALSE(fst.empty());

    auto dict = TermDict::from_memory(fst.data(), fst.size());
    ASSERT_TRUE(dict.ok());

    EXPECT_EQ(dict.value()->term_count(), 3u);

    // Exact lookups
    const TermInfo* info = dict.value()->lookup("apple");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->doc_freq, 5u);
    EXPECT_EQ(info->posting_offset, 100u);
    EXPECT_EQ(info->posting_len, 20u);

    info = dict.value()->lookup("banana");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->doc_freq, 3u);

    info = dict.value()->lookup("cherry");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->doc_freq, 7u);

    // Non-existent term
    info = dict.value()->lookup("grape");
    EXPECT_EQ(info, nullptr);
}

TEST(TermDictTest, NotFound) {
    TermDictBuilder builder;
    builder.insert("alpha", TermInfo{1, 10, 5});
    auto fst = builder.finish();

    auto dict = TermDict::from_memory(fst.data(), fst.size());
    ASSERT_TRUE(dict.ok());

    EXPECT_EQ(dict.value()->lookup("beta"), nullptr);
    EXPECT_EQ(dict.value()->lookup("alph"), nullptr);    // prefix
    EXPECT_EQ(dict.value()->lookup("alphaa"), nullptr);  // no exact
    EXPECT_EQ(dict.value()->lookup(""), nullptr);
}

TEST(TermDictTest, EmptyDict) {
    TermDictBuilder builder;
    auto fst = builder.finish();
    EXPECT_FALSE(fst.empty());

    auto dict = TermDict::from_memory(fst.data(), fst.size());
    ASSERT_TRUE(dict.ok());
    EXPECT_EQ(dict.value()->term_count(), 0u);
    EXPECT_EQ(dict.value()->lookup("anything"), nullptr);
}

TEST(TermDictTest, PrefixRange) {
    TermDictBuilder builder;
    builder.insert("dog", TermInfo{2, 100, 10});
    builder.insert("dolphin", TermInfo{1, 200, 10});
    builder.insert("donkey", TermInfo{3, 300, 10});
    builder.insert("door", TermInfo{4, 400, 10});
    builder.insert("cat", TermInfo{5, 500, 10});

    auto fst = builder.finish();
    auto dict = TermDict::from_memory(fst.data(), fst.size());
    ASSERT_TRUE(dict.ok());

    int count = 0;
    dict.value()->prefix_range("do", [&](std::string_view term, const TermInfo& info) {
        count++;
        EXPECT_TRUE(term.substr(0, 2) == "do");
        return true;
    });
    EXPECT_EQ(count, 4);  // dog, dolphin, donkey, door
}

TEST(TermDictTest, PrefixRangeEarlyStop) {
    TermDictBuilder builder;
    builder.insert("a", TermInfo{1, 10, 5});
    builder.insert("b", TermInfo{2, 20, 5});
    builder.insert("c", TermInfo{3, 30, 5});
    builder.insert("d", TermInfo{4, 40, 5});

    auto fst = builder.finish();
    auto dict = TermDict::from_memory(fst.data(), fst.size());
    ASSERT_TRUE(dict.ok());

    // All terms have empty prefix ""
    int count = 0;
    dict.value()->prefix_range("", [&](std::string_view, const TermInfo&) {
        count++;
        return count < 2;  // stop after 2
    });
    EXPECT_EQ(count, 2);
}

TEST(TermDictTest, LargeDict) {
    TermDictBuilder builder;
    const int N = 10000;
    for (int i = 0; i < N; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "term_%06d", i);
        builder.insert(buf, TermInfo{static_cast<uint32_t>(i % 100),
                                      static_cast<uint64_t>(i) * 100,
                                      50});
    }

    auto fst = builder.finish();
    auto dict = TermDict::from_memory(fst.data(), fst.size());
    ASSERT_TRUE(dict.ok());
    EXPECT_EQ(dict.value()->term_count(), static_cast<size_t>(N));

    // Lookup first, middle, last
    ASSERT_NE(dict.value()->lookup("term_000000"), nullptr);
    ASSERT_NE(dict.value()->lookup("term_005000"), nullptr);
    ASSERT_NE(dict.value()->lookup("term_009999"), nullptr);
    ASSERT_EQ(dict.value()->lookup("term_010000"), nullptr);
}

}  // namespace
}  // namespace vortex