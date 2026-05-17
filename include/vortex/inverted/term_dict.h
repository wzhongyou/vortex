#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "vortex/core/result.h"
#include "vortex/core/status.h"

namespace vortex {

struct TermInfo {
    uint32_t doc_freq;
    uint64_t posting_offset;
    uint32_t posting_len;
};

// Build an FST dictionary from sorted terms.
class TermDictBuilder {
public:
    TermDictBuilder();
    ~TermDictBuilder();

    // Insert a term with its posting info. MUST be called in lexicographic order.
    Status insert(std::string_view term, const TermInfo& info);

    // Finalize and return the serialized FST bytes.
    std::vector<uint8_t> finish();

    size_t term_count() const { return term_count_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    size_t term_count_ = 0;
};

// Query an FST dictionary (mmap'd, zero-copy).
class TermDict {
public:
    // Load from a memory buffer (typically mmap'd).
    static Result<std::unique_ptr<TermDict>> from_memory(
        const uint8_t* data, size_t len);

    // Exact lookup. Returns nullptr if not found.
    const TermInfo* lookup(std::string_view term) const;

    // Iterate all terms with the given prefix.
    // Callback: bool(std::string_view term, const TermInfo& info).
    // Return false from callback to stop early.
    void prefix_range(std::string_view prefix,
                      std::function<bool(std::string_view, const TermInfo&)> fn) const;

    size_t term_count() const { return term_count_; }
    size_t memory_usage() const { return data_len_; }

private:
    TermDict() = default;

    const uint8_t* data_ = nullptr;
    size_t data_len_ = 0;
    size_t term_count_ = 0;
    size_t root_offset_ = 0;

    // Internal: follow an arc for a given byte label.
    // Returns target node offset, or 0 if no arc.
    uint32_t follow_arc(uint32_t node_off, uint8_t label) const;

    // Read vbyte-encoded uint64 from data.
    static uint64_t read_vbyte(const uint8_t*& p);
};

}  // namespace vortex
