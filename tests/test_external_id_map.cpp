#include "gtest/gtest.h"
#include "vortex/core/arena.h"
#include "vortex/inverted/external_id_map.h"

namespace vortex {
namespace {

TEST(ExternalIdMapTest, InsertAndFind) {
    ExternalIdMap map;
    map.insert("doc_001", 0, 42);
    map.insert("doc_002", 0, 43);
    map.insert("doc_003", 1, 7);

    EXPECT_EQ(map.size(), 3u);

    auto* loc = map.find("doc_001");
    ASSERT_NE(loc, nullptr);
    EXPECT_EQ(loc->segment_id, 0u);
    EXPECT_EQ(loc->internal_doc_id, 42u);

    loc = map.find("doc_003");
    ASSERT_NE(loc, nullptr);
    EXPECT_EQ(loc->segment_id, 1u);
    EXPECT_EQ(loc->internal_doc_id, 7u);
}

TEST(ExternalIdMapTest, FindMissing) {
    ExternalIdMap map;
    map.insert("existing", 0, 1);
    EXPECT_EQ(map.find("nonexistent"), nullptr);
}

TEST(ExternalIdMapTest, Remove) {
    ExternalIdMap map;
    map.insert("to_remove", 0, 100);
    EXPECT_EQ(map.size(), 1u);
    EXPECT_NE(map.find("to_remove"), nullptr);

    map.remove("to_remove");
    EXPECT_EQ(map.size(), 0u);
    EXPECT_EQ(map.find("to_remove"), nullptr);
}

TEST(ExternalIdMapTest, ResolveReverseMap) {
    ExternalIdMap map;
    map.set_reverse_map({"ext_0", "ext_1", "ext_2"});

    EXPECT_EQ(map.resolve(0), "ext_0");
    EXPECT_EQ(map.resolve(1), "ext_1");
    EXPECT_EQ(map.resolve(2), "ext_2");
}

TEST(ExternalIdMapTest, UpdateExisting) {
    ExternalIdMap map;
    map.insert("doc", 0, 1);
    map.insert("doc", 1, 99);  // same key, update location

    auto* loc = map.find("doc");
    ASSERT_NE(loc, nullptr);
    EXPECT_EQ(loc->segment_id, 1u);
    EXPECT_EQ(loc->internal_doc_id, 99u);
}

TEST(ExternalIdMapTest, FlushAndFromFile) {
    ExternalIdMap map;
    map.insert("a", 0, 0);
    map.insert("b", 0, 1);
    map.insert("c", 1, 0);

    Arena arena(1024);
    char tmpname[] = "/tmp/vortex_idm_test_XXXXXX";
    int fd = mkstemp(tmpname);
    ASSERT_GT(fd, -1);

    auto s = map.flush(fd, arena);
    ASSERT_TRUE(s.ok());
    close(fd);

    // Read back
    auto loaded = ExternalIdMap::from_file(tmpname);
    ASSERT_TRUE(loaded.ok());
    EXPECT_EQ(loaded.value()->size(), 3u);
    EXPECT_NE(loaded.value()->find("a"), nullptr);
    EXPECT_NE(loaded.value()->find("b"), nullptr);
    EXPECT_NE(loaded.value()->find("c"), nullptr);
    EXPECT_EQ(loaded.value()->find("d"), nullptr);

    unlink(tmpname);
}

}  // namespace
}  // namespace vortex