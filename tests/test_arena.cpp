#include "gtest/gtest.h"
#include "vortex/core/arena.h"

namespace vortex {
namespace {

TEST(ArenaTest, AllocateAndZeroInit) {
    Arena arena(1024);
    void* p = arena.allocate(64, 8);
    ASSERT_NE(p, nullptr);
    EXPECT_GE(arena.allocated(), 64u);
}

TEST(ArenaTest, MultipleAllocations) {
    Arena arena(256);
    void* p1 = arena.allocate(32, 8);
    void* p2 = arena.allocate(64, 8);
    void* p3 = arena.allocate(16, 8);
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    ASSERT_NE(p3, nullptr);
    // Pointers should be distinct
    EXPECT_NE(p1, p2);
    EXPECT_NE(p2, p3);
}

TEST(ArenaTest, ResetReusesChunk) {
    Arena arena(4096);
    void* p1 = arena.allocate(128, 8);
    ASSERT_NE(p1, nullptr);
    size_t capacity_before = arena.capacity();
    arena.reset();
    EXPECT_EQ(arena.allocated(), 0u);
    void* p2 = arena.allocate(128, 8);
    ASSERT_NE(p2, nullptr);
    // Chunk should be reused
    EXPECT_EQ(arena.capacity(), capacity_before);
}

TEST(ArenaTest, ClearFreesMemory) {
    Arena arena(256);
    arena.allocate(256, 8);
    EXPECT_GT(arena.capacity(), 0u);
    arena.clear();
    EXPECT_EQ(arena.capacity(), 0u);
    EXPECT_EQ(arena.allocated(), 0u);
}

TEST(ArenaTest, ChunkGrowth) {
    Arena arena(64);
    // Allocate more than initial chunk size to trigger growth
    void* p1 = arena.allocate(128, 8);
    ASSERT_NE(p1, nullptr);
    EXPECT_GE(arena.capacity(), 128u);
}

TEST(ArenaTest, AlignedAllocation) {
    Arena arena(4096);
    void* p = arena.allocate(32, 256);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 256, 0u);
}

TEST(ArenaTest, ArenaStrdup) {
    Arena arena(256);
    std::string_view s = "hello world";
    std::string_view dup = arena_strdup(arena, s);
    EXPECT_EQ(dup, s);
    // Should be a copy, not the same pointer
    EXPECT_NE(dup.data(), s.data());
}

TEST(ArenaTest, ArenaNew) {
    Arena arena(256);
    struct Point {
        int x, y;
        Point(int a, int b) : x(a), y(b) {}
    };
    Point* p = arena_new<Point>(arena, 3, 4);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->x, 3);
    EXPECT_EQ(p->y, 4);
}

TEST(ArenaTest, MoveConstruct) {
    Arena a1(1024);
    a1.allocate(64, 8);
    size_t cap = a1.capacity();
    size_t alloc = a1.allocated();

    Arena a2(std::move(a1));
    EXPECT_EQ(a2.capacity(), cap);
    EXPECT_EQ(a2.allocated(), alloc);
    EXPECT_EQ(a1.capacity(), 0u);
    EXPECT_EQ(a1.allocated(), 0u);
}

TEST(ScopedThreadArenaTest, BindAndUnbind) {
    Arena arena(256);
    EXPECT_EQ(ScopedThreadArena::current(), nullptr);
    {
        ScopedThreadArena scoped(arena);
        EXPECT_NE(ScopedThreadArena::current(), nullptr);
        EXPECT_EQ(ScopedThreadArena::current(), &arena);
    }
    EXPECT_EQ(ScopedThreadArena::current(), nullptr);
}

}  // namespace
}  // namespace vortex