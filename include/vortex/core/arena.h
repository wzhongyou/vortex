#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>

#include "vortex/core/types.h"

namespace vortex {

class Arena {
public:
    static constexpr size_t kDefaultChunkSize = 256 * 1024;

    explicit Arena(size_t chunk_size = kDefaultChunkSize);
    ~Arena();

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&&) noexcept;
    Arena& operator=(Arena&&) noexcept;

    void* allocate(size_t size, size_t alignment = 8);
    void reset();
    void clear();

    size_t allocated() const { return allocated_; }
    size_t capacity() const { return capacity_; }

private:
    struct Chunk {
        std::unique_ptr<char[]> data;
        size_t size;
        size_t used;
        Chunk* next;
    };

    Chunk* new_chunk(size_t min_size);

    Chunk* head_ = nullptr;
    Chunk* current_ = nullptr;
    size_t chunk_size_;
    size_t allocated_ = 0;
    size_t capacity_ = 0;
};

class ScopedThreadArena {
public:
    explicit ScopedThreadArena(Arena& arena);
    ~ScopedThreadArena();

    ScopedThreadArena(const ScopedThreadArena&) = delete;
    ScopedThreadArena& operator=(const ScopedThreadArena&) = delete;

    static Arena* current();

private:
    Arena* prev_;
};

template <typename T, typename... Args>
T* arena_new(Arena& arena, Args&&... args) {
    void* mem = arena.allocate(sizeof(T), alignof(T));
    return new (mem) T(std::forward<Args>(args)...);
}

inline std::string_view arena_strdup(Arena& arena, const char* s, size_t len) {
    char* buf = static_cast<char*>(arena.allocate(len + 1, 1));
    std::memcpy(buf, s, len);
    buf[len] = '\0';
    return std::string_view(buf, len);
}

inline std::string_view arena_strdup(Arena& arena, std::string_view s) {
    return arena_strdup(arena, s.data(), s.size());
}

}  // namespace vortex
