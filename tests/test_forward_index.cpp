#include <unistd.h>

#include <vector>

#include "gtest/gtest.h"
#include "vortex/inverted/forward_index.h"

namespace vortex {
namespace {

TEST(ForwardIndexTest, BuildAndRead) {
    ForwardIndexBuilder builder(2);  // 2 indexed fields
    uint32_t fl1[] = {3, 5};
    uint32_t fl2[] = {4, 2};
    uint32_t fl3[] = {6, 8};
    builder.append(8, fl1);   // doc 0
    builder.append(6, fl2);   // doc 1
    builder.append(14, fl3);  // doc 2

    // Flush to memory buffer via temp file
    char tmpname[] = "/tmp/vortex_fwd_test_XXXXXX";
    int fd = mkstemp(tmpname);
    ASSERT_GT(fd, -1);
    unlink(tmpname);

    auto s = builder.flush(fd);
    ASSERT_TRUE(s.ok());

    // Read back
    off_t len = lseek(fd, 0, SEEK_END);
    std::vector<uint8_t> data(len);
    pread(fd, data.data(), len, 0);
    close(fd);

    auto fwd = ForwardIndex::from_memory(data.data(), data.size());
    ASSERT_TRUE(fwd.ok());

    EXPECT_EQ(fwd.value()->doc_count(), 3u);
    EXPECT_EQ(fwd.value()->field_count(), 2u);

    auto info = fwd.value()->get(0);
    EXPECT_EQ(info.doc_length, 8u);
    EXPECT_EQ(info.field_lengths[0], 3u);
    EXPECT_EQ(info.field_lengths[1], 5u);

    info = fwd.value()->get(1);
    EXPECT_EQ(info.doc_length, 6u);
    EXPECT_EQ(info.field_lengths[0], 4u);
    EXPECT_EQ(info.field_lengths[1], 2u);

    info = fwd.value()->get(2);
    EXPECT_EQ(info.doc_length, 14u);
    EXPECT_EQ(info.field_lengths[0], 6u);
    EXPECT_EQ(info.field_lengths[1], 8u);
}

TEST(ForwardIndexTest, EmptyIndex) {
    ForwardIndexBuilder builder(1);
    char tmpname[] = "/tmp/vortex_fwd_empty_XXXXXX";
    int fd = mkstemp(tmpname);
    ASSERT_GT(fd, -1);
    unlink(tmpname);

    auto s = builder.flush(fd);
    ASSERT_TRUE(s.ok());

    off_t len = lseek(fd, 0, SEEK_END);
    std::vector<uint8_t> data(len);
    pread(fd, data.data(), len, 0);
    close(fd);

    auto fwd = ForwardIndex::from_memory(data.data(), data.size());
    ASSERT_TRUE(fwd.ok());
    EXPECT_EQ(fwd.value()->doc_count(), 0u);
}

TEST(ForwardIndexTest, SingleField) {
    ForwardIndexBuilder builder(1);
    uint32_t fl[] = {5};
    builder.append(5, fl);
    builder.append(10, fl);

    char tmpname[] = "/tmp/vortex_fwd_single_XXXXXX";
    int fd = mkstemp(tmpname);
    ASSERT_GT(fd, -1);
    unlink(tmpname);

    builder.flush(fd);
    off_t len = lseek(fd, 0, SEEK_END);
    std::vector<uint8_t> data(len);
    pread(fd, data.data(), len, 0);
    close(fd);

    auto fwd = ForwardIndex::from_memory(data.data(), data.size());
    ASSERT_TRUE(fwd.ok());
    EXPECT_EQ(fwd.value()->doc_count(), 2u);

    auto info = fwd.value()->get(0);
    EXPECT_EQ(info.field_lengths[0], 5u);
}

TEST(ForwardIndexTest, ManyFields) {
    constexpr uint16_t kFields = 10;
    ForwardIndexBuilder builder(kFields);
    uint32_t fl[kFields];
    for (int j = 0; j < kFields; j++) fl[j] = j + 1;
    builder.append(55, fl);

    char tmpname[] = "/tmp/vortex_fwd_many_XXXXXX";
    int fd = mkstemp(tmpname);
    ASSERT_GT(fd, -1);
    unlink(tmpname);

    builder.flush(fd);
    off_t len = lseek(fd, 0, SEEK_END);
    std::vector<uint8_t> data(len);
    pread(fd, data.data(), len, 0);
    close(fd);

    auto fwd = ForwardIndex::from_memory(data.data(), data.size());
    ASSERT_TRUE(fwd.ok());
    EXPECT_EQ(fwd.value()->field_count(), kFields);

    auto info = fwd.value()->get(0);
    EXPECT_EQ(info.doc_length, 55u);
    for (uint16_t j = 0; j < kFields; j++) {
        EXPECT_EQ(info.field_lengths[j], j + 1u);
    }
}

}  // namespace
}  // namespace vortex