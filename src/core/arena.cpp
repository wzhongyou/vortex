#include "vortex/core/arena.h"

#include <thread>

namespace vortex {

Arena::Arena(size_t chunk_size)
    : chunk_size_(chunk_size) {
    head_ = new_chunk(0);
    current_ = head_;
}

Arena::~Arena() {
    clear();
}

Arena::Arena(Arena&& other) noexcept
    : head_(other.head_)
    , current_(other.current_)
    , chunk_size_(other.chunk_size_)
    , allocated_(other.allocated_)
    , capacity_(other.capacity_) {
    other.head_ = nullptr;
    other.current_ = nullptr;
    other.allocated_ = 0;
    other.capacity_ = 0;
}

Arena& Arena::operator=(Arena&& other) noexcept {
    if (this != &other) {
        clear();
        head_ = other.head_;
        current_ = other.current_;
        chunk_size_ = other.chunk_size_;
        allocated_ = other.allocated_;
        capacity_ = other.capacity_;
        other.head_ = nullptr;
        other.current_ = nullptr;
        other.allocated_ = 0;
        other.capacity_ = 0;
    }
    return *this;
}

Arena::Chunk* Arena::new_chunk(size_t min_size) {
    size_t alloc_size = min_size > chunk_size_ ? min_size : chunk_size_;
    auto* chunk = new Chunk();
    chunk->data = std::make_unique<char[]>(alloc_size);
    chunk->size = alloc_size;
    chunk->used = 0;
    chunk->next = nullptr;
    capacity_ += alloc_size;
    return chunk;
}

void* Arena::allocate(size_t size, size_t alignment) {
    if (size == 0) return nullptr;

    uintptr_t raw = reinterpret_cast<uintptr_t>(current_->data.get() + current_->used);
    uintptr_t aligned = (raw + alignment - 1) & ~(alignment - 1);
    size_t padding = aligned - raw;

    size_t needed = padding + size;
    if (current_->used + needed > current_->size) {
        Chunk* chunk = new_chunk(needed);
        chunk->next = current_->next;
        current_->next = chunk;
        current_ = chunk;
        aligned = reinterpret_cast<uintptr_t>(current_->data.get());
    }

    current_->used = reinterpret_cast<char*>(aligned) - current_->data.get() + size;
    allocated_ += size;
    return reinterpret_cast<void*>(aligned);
}

void Arena::reset() {
    Chunk* c = head_;
    while (c) {
        c->used = 0;
        c = c->next;
    }
    current_ = head_;
    allocated_ = 0;
}

void Arena::clear() {
    Chunk* c = head_;
    while (c) {
        Chunk* next = c->next;
        delete c;
        c = next;
    }
    head_ = nullptr;
    current_ = nullptr;
    allocated_ = 0;
    capacity_ = 0;
}

static thread_local Arena* tls_arena = nullptr;

ScopedThreadArena::ScopedThreadArena(Arena& arena)
    : prev_(tls_arena) {
    tls_arena = &arena;
}

ScopedThreadArena::~ScopedThreadArena() {
    tls_arena = prev_;
}

Arena* ScopedThreadArena::current() {
    return tls_arena;
}

}  // namespace vortex
