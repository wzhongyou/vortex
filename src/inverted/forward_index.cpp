#include "vortex/inverted/forward_index.h"

#include <unistd.h>
#include <cstring>

namespace vortex {

ForwardIndexBuilder::ForwardIndexBuilder(uint16_t field_count)
    : field_count_(field_count) {
    buffer_.reserve(10000 * (1 + field_count));
}

void ForwardIndexBuilder::append(uint32_t doc_length,
                                  const uint32_t* field_lengths) {
    buffer_.push_back(doc_length);
    for (uint16_t f = 0; f < field_count_; f++) {
        buffer_.push_back(field_lengths[f]);
    }
    doc_count_++;
}

Status ForwardIndexBuilder::flush(int fd) {
    // Header: doc_count (4B) + field_count (2B)
    uint32_t dc = doc_count_;
    write(fd, &dc, 4);
    write(fd, &field_count_, 2);

    size_t data_size = buffer_.size() * sizeof(uint32_t);
    ssize_t written = write(fd, buffer_.data(), data_size);
    if (written < 0 || static_cast<size_t>(written) != data_size) {
        return Status::IOError("forward index write failed");
    }
    return Status::OK();
}

Result<std::unique_ptr<ForwardIndex>> ForwardIndex::from_memory(
    const uint8_t* data, size_t len) {
    if (len < 4) {
        return Result<std::unique_ptr<ForwardIndex>>::Err(
            Status::CorruptIndex("forward index too short"));
    }
    // First 4 bytes: doc_count, next 2: field_count
    uint32_t dc;
    uint16_t fc;
    std::memcpy(&dc, data, 4);
    std::memcpy(&fc, data + 4, 2);
    uint16_t ew = 1 + fc;

    auto fi = std::unique_ptr<ForwardIndex>(new ForwardIndex());
    fi->data_ = data;
    fi->doc_count_ = dc;
    fi->field_count_ = fc;
    fi->entry_words_ = ew;
    return Result<std::unique_ptr<ForwardIndex>>::Ok(std::move(fi));
}

ForwardIndex::DocInfo ForwardIndex::get(uint32_t doc_id) const {
    DocInfo info{0, nullptr};
    if (doc_id >= doc_count_) return info;

    size_t off = 6 + static_cast<size_t>(doc_id) * entry_words_ * 4;
    const uint32_t* p = reinterpret_cast<const uint32_t*>(data_ + off);
    info.doc_length = p[0];
    info.field_lengths = p + 1;
    return info;
}

}  // namespace vortex
