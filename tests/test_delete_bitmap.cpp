#include "gtest/gtest.h"
#include "vortex/inverted/delete_bitmap.h"

namespace vortex {
namespace {

TEST(DeleteBitmapTest, InitiallyEmpty) {
    DeleteBitmap bm;
    bm.set_total(100);
    EXPECT_EQ(bm.deleted_count(), 0u);
    EXPECT_FALSE(bm.is_deleted(0));
    EXPECT_FALSE(bm.is_deleted(50));
    EXPECT_FALSE(bm.is_deleted(99));
}

TEST(DeleteBitmapTest, MarkAndCheck) {
    DeleteBitmap bm;
    bm.set_total(100);
    bm.mark_deleted(10);
    bm.mark_deleted(20);
    bm.mark_deleted(30);

    EXPECT_TRUE(bm.is_deleted(10));
    EXPECT_TRUE(bm.is_deleted(20));
    EXPECT_TRUE(bm.is_deleted(30));
    EXPECT_FALSE(bm.is_deleted(0));
    EXPECT_FALSE(bm.is_deleted(15));
    EXPECT_EQ(bm.deleted_count(), 3u);
}

TEST(DeleteBitmapTest, MarkDeletedBulk) {
    DeleteBitmap bm;
    bm.set_total(50);
    bm.mark_deleted_bulk({3, 7, 15, 22, 37});

    EXPECT_EQ(bm.deleted_count(), 5u);
    EXPECT_TRUE(bm.is_deleted(3));
    EXPECT_TRUE(bm.is_deleted(7));
    EXPECT_TRUE(bm.is_deleted(37));
    EXPECT_FALSE(bm.is_deleted(8));
}

TEST(DeleteBitmapTest, DoubleDeleteIdempotent) {
    DeleteBitmap bm;
    bm.set_total(10);
    bm.mark_deleted(5);
    bm.mark_deleted(5);  // again
    EXPECT_TRUE(bm.is_deleted(5));
    EXPECT_EQ(bm.deleted_count(), 1u);
}

TEST(DeleteBitmapTest, SerializeRoundtrip) {
    DeleteBitmap bm;
    bm.set_total(200);
    bm.mark_deleted_bulk({0, 1, 50, 100, 199});

    auto data = bm.serialize();
    EXPECT_FALSE(data.empty());

    auto restored = DeleteBitmap::deserialize(data.data(), data.size());
    EXPECT_EQ(restored.deleted_count(), 5u);
    EXPECT_EQ(restored.total(), 200u);
    EXPECT_TRUE(restored.is_deleted(0));
    EXPECT_TRUE(restored.is_deleted(1));
    EXPECT_TRUE(restored.is_deleted(100));
    EXPECT_FALSE(restored.is_deleted(2));
    EXPECT_FALSE(restored.is_deleted(101));
}

TEST(DeleteBitmapTest, SerializeEmpty) {
    DeleteBitmap bm;
    bm.set_total(0);

    auto data = bm.serialize();
    auto restored = DeleteBitmap::deserialize(data.data(), data.size());
    EXPECT_EQ(restored.deleted_count(), 0u);
    EXPECT_EQ(restored.total(), 0u);
    EXPECT_FALSE(restored.is_deleted(0));
}

}  // namespace
}  // namespace vortex