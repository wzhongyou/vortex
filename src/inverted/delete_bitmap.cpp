#include "vortex/inverted/delete_bitmap.h"

#include <cstring>

namespace vortex {

bool DeleteBitmap::is_deleted(uint32_t doc_id) const {
    if (doc_id >= bitmap_.size()) return false;
    return bitmap_[doc_id];
}

void DeleteBitmap::mark_deleted(uint32_t doc_id) {
    if (doc_id >= bitmap_.size()) {
        bitmap_.resize(doc_id + 1, false);
    }
    if (!bitmap_[doc_id]) {
        bitmap_[doc_id] = true;
        deleted_++;
    }
}

void DeleteBitmap::mark_deleted_bulk(const std::vector<uint32_t>& doc_ids) {
    for (auto id : doc_ids) mark_deleted(id);
}

std::vector<uint8_t> DeleteBitmap::serialize() const {
    // Simple format: [total:u32][deleted:u32][bitmap_bytes: packed bits]
    size_t byte_count = (total_ + 7) / 8;
    std::vector<uint8_t> out(8 + byte_count);
    std::memcpy(out.data(), &total_, 4);
    std::memcpy(out.data() + 4, &deleted_, 4);

    for (size_t i = 0; i < bitmap_.size() && i / 8 < byte_count; i++) {
        if (bitmap_[i]) {
            out[8 + i / 8] |= (1U << (i % 8));
        }
    }
    return out;
}

DeleteBitmap DeleteBitmap::deserialize(const uint8_t* data, size_t len) {
    DeleteBitmap db;
    if (len < 8) return db;

    std::memcpy(&db.total_, data, 4);
    std::memcpy(&db.deleted_, data + 4, 4);

    size_t byte_count = (db.total_ + 7) / 8;
    db.bitmap_.resize(db.total_, false);
    for (size_t i = 0; i < db.total_ && i / 8 < byte_count; i++) {
        if (data[8 + i / 8] & (1U << (i % 8))) {
            db.bitmap_[i] = true;
        }
    }
    return db;
}

}  // namespace vortex
