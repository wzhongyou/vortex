#pragma once

#include <cstdint>
#include <vector>

#include "vortex/core/status.h"
#include "vortex/core/types.h"
#include "vortex/inverted/skip_list.h"

namespace vortex {

class Arena;

class PostingListBuilder {
public:
    PostingListBuilder() = default;

    // Append a posting. Must be called in doc_id ascending order.
    void append(uint32_t doc_id, uint32_t term_freq);

    // Write all blocks + skip list to fd. Returns {offset, byte_count}.
    Result<std::pair<uint64_t, uint32_t>> flush(int fd, Arena& scratch);

    size_t doc_freq() const { return total_docs_; }

private:
    std::vector<uint32_t> doc_ids_;
    std::vector<uint32_t> freqs_;
    size_t total_docs_ = 0;
};

class PostingListReader {
public:
    PostingListReader();
    PostingListReader(const uint8_t* data, size_t len);

    // Iterate all postings. Callback: void(uint32_t doc_id, uint32_t tf).
    template <typename Fn>
    void for_each(Fn&& fn) const;

    // Advance to first doc_id >= target. Returns false if exhausted.
    bool advance_to(uint32_t target, uint32_t& doc_id, uint32_t& tf) const;

    size_t doc_freq() const { return total_docs_; }

private:
    const uint8_t* data_ = nullptr;
    [[maybe_unused]] size_t data_len_ = 0;
    size_t total_docs_ = 0;
    SkipList skip_list_;
    size_t num_blocks_ = 0;

    mutable uint32_t current_block_ = 0;
    mutable uint32_t block_docs_[128];
    mutable uint32_t block_freqs_[128];
    mutable uint8_t  block_count_ = 0;
    mutable uint8_t  block_pos_ = 0;

    void load_block(uint32_t block_idx) const;
};

template <typename Fn>
void PostingListReader::for_each(Fn&& fn) const {
    for (uint32_t b = 0; b < num_blocks_; b++) {
        load_block(b);
        for (uint8_t i = 0; i < block_count_; i++) {
            fn(block_docs_[i], block_freqs_[i]);
        }
    }
}

}  // namespace vortex
