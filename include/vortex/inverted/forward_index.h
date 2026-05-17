#pragma once

#include <cstdint>
#include <memory>

#include "vortex/core/result.h"
#include "vortex/core/status.h"
#include "vortex/core/types.h"

namespace vortex {

class ForwardIndexBuilder {
public:
    explicit ForwardIndexBuilder(uint16_t field_count);

    void append(uint32_t doc_length, const uint32_t* field_lengths);
    Status flush(int fd);

    size_t doc_count() const { return doc_count_; }

private:
    uint16_t field_count_;
    std::vector<uint32_t> buffer_;  // interleaved: [doc_len, fl0, fl1, ...]
    size_t doc_count_ = 0;
};

class ForwardIndex {
public:
    static Result<std::unique_ptr<ForwardIndex>> from_memory(
        const uint8_t* data, size_t len);

    struct DocInfo {
        uint32_t doc_length;
        const uint32_t* field_lengths;  // length = field_count
    };

    DocInfo get(uint32_t doc_id) const;

    uint16_t field_count() const { return field_count_; }
    uint32_t doc_count() const { return doc_count_; }

private:
    ForwardIndex() = default;
    const uint8_t* data_ = nullptr;
    uint32_t doc_count_ = 0;
    uint16_t field_count_ = 0;
    uint16_t entry_words_ = 0;  // words per entry (1 + field_count)
};

}  // namespace vortex
