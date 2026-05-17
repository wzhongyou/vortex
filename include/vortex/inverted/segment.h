#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "vortex/core/result.h"
#include "vortex/core/types.h"
#include "vortex/inverted/posting_list.h"

namespace vortex {

class Arena;
class TermDict;
class ForwardIndex;
class DeleteBitmap;
struct TermInfo;

// Immutable on-disk segment.
struct Segment {
    Segment(uint64_t id, uint32_t doc_count, double avgdl);

    uint64_t id() const { return id_; }
    uint32_t doc_count() const { return doc_count_; }
    double avgdl() const { return avgdl_; }

    Status load_dict(const uint8_t* data, size_t len);
    Status load_postings(const uint8_t* data, size_t len);
    Status load_forward_index(const uint8_t* data, size_t len);
    Status load_deletes(const uint8_t* data, size_t len);

    TermDict* term_dict() const { return term_dict_.get(); }
    ForwardIndex* forward_index() const { return fwd_.get(); }
    DeleteBitmap* deletes() const { return deletes_.get(); }

    const TermInfo* lookup_term(std::string_view term) const;
    const uint8_t* posting_data() const;
    size_t posting_data_len() const;

private:
    uint64_t id_;
    uint32_t doc_count_;
    double avgdl_;

    std::unique_ptr<TermDict> term_dict_;
    const uint8_t* posting_data_ = nullptr;
    size_t posting_data_len_ = 0;
    std::unique_ptr<ForwardIndex> fwd_;
    std::unique_ptr<DeleteBitmap> deletes_;

    // Owned file data — keeps backing memory alive for the raw pointers above.
    std::vector<uint8_t> owned_fst_data_;
    std::vector<uint8_t> owned_posting_data_;
    std::vector<uint8_t> owned_fwd_data_;

    friend struct MemorySegment;  // flush() sets owned data
};

// Mutable in-memory segment being built.
struct MemorySegment {
    MemorySegment(uint64_t seg_id, uint16_t indexed_field_count);

    void add_term(uint32_t doc_id, std::string_view term, uint32_t tf);
    void add_doc_info(uint32_t doc_length,
                      const std::vector<uint32_t>& field_lengths);

    Result<std::shared_ptr<const Segment>> flush(
        const std::string& segment_dir, Arena& arena);

    uint64_t segment_id() const { return seg_id_; }
    uint32_t doc_count() const { return doc_count_; }
    size_t memory_used() const { return total_term_occurrences_; }

private:
    uint64_t seg_id_;
    uint16_t indexed_field_count_;
    uint32_t doc_count_ = 0;
    uint32_t total_terms_ = 0;
    size_t total_term_occurrences_ = 0;

    std::unordered_map<std::string, std::unique_ptr<PostingListBuilder>> term_builders_;
    std::vector<uint32_t> doc_lengths_;
    std::vector<std::vector<uint32_t>> field_lengths_;
};

}  // namespace vortex
