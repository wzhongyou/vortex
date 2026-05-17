#pragma once

#include <cstdint>
#include <vector>

namespace vortex {

class DeleteBitmap {
public:
    DeleteBitmap() = default;

    bool is_deleted(uint32_t doc_id) const;
    void mark_deleted(uint32_t doc_id);
    void mark_deleted_bulk(const std::vector<uint32_t>& doc_ids);

    size_t deleted_count() const { return deleted_; }
    uint32_t total() const { return total_; }
    void set_total(uint32_t total) { total_ = total; }

    std::vector<uint8_t> serialize() const;
    static DeleteBitmap deserialize(const uint8_t* data, size_t len);

private:
    std::vector<bool> bitmap_;  // simple bitmap for V1 (replace with Roaring in V2)
    size_t deleted_ = 0;
    uint32_t total_ = 0;
};

}  // namespace vortex
