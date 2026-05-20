#include <unistd.h>

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "gtest/gtest.h"
#include "vortex/core/arena.h"
#include "vortex/core/document.h"
#include "vortex/core/schema.h"
#include "vortex/inverted/analyzer.h"
#include "vortex/inverted/filter.h"
#include "vortex/inverted/index_reader.h"
#include "vortex/inverted/index_writer.h"
#include "vortex/inverted/tokenizer.h"
#include "vortex/inverted/wal.h"

namespace vortex {
namespace {

class WALTest : public ::testing::Test {
protected:
    void SetUp() override {
        char tmpname[] = "/tmp/vortex_wal_test_XXXXXX";
        fd_ = mkstemp(tmpname);
        ASSERT_GT(fd_, -1);
        path_ = tmpname;
        close(fd_);
    }

    void TearDown() override {
        unlink(path_.c_str());
    }

    std::string path_;
    int fd_ = -1;
};

TEST_F(WALTest, AppendAddAndRecover) {
    Schema schema;
    ASSERT_TRUE(schema.add_field({"title", FieldType::TEXT, true, true}).ok());

    WAL wal(path_);
    Document doc;
    doc.fields.push_back({"title", "Hello World"});

    auto s = wal.append_add(1, "ext_1", doc);
    ASSERT_TRUE(s.ok());
    s = wal.append_add(2, "ext_2", doc);
    ASSERT_TRUE(s.ok());
    s = wal.sync();
    ASSERT_TRUE(s.ok());

    EXPECT_GT(wal.bytes_written(), 0u);

    // Recover
    auto recovered = wal.recover(schema);
    ASSERT_TRUE(recovered.ok());
    EXPECT_EQ(recovered.value().next_internal_id, 2u);  // max internal_id
    EXPECT_EQ(recovered.value().removed_ids.size(), 0u);
}

TEST_F(WALTest, AppendRemoveAndRecover) {
    Schema schema;
    ASSERT_TRUE(schema.add_field({"id", FieldType::KEYWORD, true, false}).ok());

    WAL wal(path_);
    Document doc;
    doc.fields.push_back({"id", "ext_1"});

    wal.append_add(1, "ext_1", doc);
    wal.append_remove("ext_1");
    wal.sync();

    auto recovered = wal.recover(schema);
    ASSERT_TRUE(recovered.ok());
    EXPECT_EQ(recovered.value().removed_ids.size(), 1u);
    EXPECT_TRUE(std::find(recovered.value().removed_ids.begin(),
                          recovered.value().removed_ids.end(), "ext_1")
                != recovered.value().removed_ids.end());
}

TEST_F(WALTest, Truncate) {
    Schema schema;
    ASSERT_TRUE(schema.add_field({"title", FieldType::TEXT, true, true}).ok());

    WAL wal(path_);
    Document doc;
    doc.fields.push_back({"title", "Hello"});

    wal.append_add(1, "ext_1", doc);
    wal.sync();
    EXPECT_GT(wal.bytes_written(), 0u);

    auto s = wal.truncate();
    ASSERT_TRUE(s.ok());

    // After truncate, recover should show nothing
    auto recovered = wal.recover(schema);
    ASSERT_TRUE(recovered.ok());
    EXPECT_EQ(recovered.value().active_docs.size(), 0u);
}

TEST_F(WALTest, MultipleRecords) {
    Schema schema;
    ASSERT_TRUE(schema.add_field({"content", FieldType::TEXT, true, true}).ok());

    WAL wal(path_);
    const int N = 100;
    for (int i = 0; i < N; i++) {
        Document doc;
        doc.fields.push_back({"content", std::to_string(i)});
        auto s = wal.append_add(i, "ext_" + std::to_string(i), doc);
        ASSERT_TRUE(s.ok());
    }
    wal.sync();

    auto recovered = wal.recover(schema);
    ASSERT_TRUE(recovered.ok());
    EXPECT_EQ(recovered.value().next_internal_id, static_cast<uint32_t>(N - 1));
}

}  // namespace
}  // namespace vortex