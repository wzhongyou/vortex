#include "vortex/inverted/skip_list.h"

#include <cstring>
#include <limits>

namespace vortex {

void SkipList::build(const std::vector<uint64_t>& block_offsets,
                     const std::vector<uint32_t>& block_max_doc,
                     int skip_interval) {
    levels_.clear();
    if (block_offsets.empty()) return;

    size_t num_blocks = block_offsets.size();

    // Build levels bottom-up
    int step = skip_interval;
    while (step < static_cast<int>(num_blocks)) {
        std::vector<SkipEntry> level;
        for (size_t i = step; i <= num_blocks; i += step) {
            size_t group_start = i - step;
            size_t group_end = std::min(static_cast<size_t>(i), num_blocks) - 1;
            // last_doc_id is the max doc_id in this group
            uint32_t max_doc = block_max_doc[group_end];
            SkipEntry entry{max_doc, block_offsets[group_start]};
            level.push_back(entry);
        }
        levels_.push_back(std::move(level));
        step *= skip_interval;
    }
}

uint64_t SkipList::advance(uint32_t target, uint64_t current_offset) const {
    if (levels_.empty()) return 0;

    // Scan level 0 linearly to find the first entry with last_doc_id >= target.
    // The multi-level skip optimization is deferred — level 0 scan is correct
    // and sufficient for V1 block counts (typically < 100 blocks per segment).
    const auto& level0 = levels_[0];
    for (const auto& entry : level0) {
        if (entry.last_doc_id >= target) {
            return entry.block_offset;
        }
    }

    // Target beyond all blocks
    return UINT64_MAX;
}

std::vector<uint8_t> SkipList::serialize() const {
    // Format: num_levels (u32), then for each level: count (u32) + entries
    size_t total = 4;  // num_levels
    for (auto& level : levels_) {
        total += 4 + level.size() * (sizeof(uint32_t) + sizeof(uint64_t));
    }

    std::vector<uint8_t> buf(total);
    size_t off = 0;

    uint32_t nl = static_cast<uint32_t>(levels_.size());
    std::memcpy(buf.data() + off, &nl, 4); off += 4;

    for (auto& level : levels_) {
        uint32_t count = static_cast<uint32_t>(level.size());
        std::memcpy(buf.data() + off, &count, 4); off += 4;
        for (auto& e : level) {
            std::memcpy(buf.data() + off, &e.last_doc_id, 4); off += 4;
            std::memcpy(buf.data() + off, &e.block_offset, 8); off += 8;
        }
    }
    return buf;
}

SkipList SkipList::deserialize(const uint8_t* data, size_t len) {
    SkipList sl;
    if (len < 4) return sl;

    size_t off = 0;
    uint32_t nl;
    std::memcpy(&nl, data + off, 4); off += 4;

    for (uint32_t l = 0; l < nl; l++) {
        if (off + 4 > len) break;
        uint32_t count;
        std::memcpy(&count, data + off, 4); off += 4;

        std::vector<SkipEntry> level;
        for (uint32_t i = 0; i < count; i++) {
            if (off + 12 > len) break;
            SkipEntry e;
            std::memcpy(&e.last_doc_id, data + off, 4); off += 4;
            std::memcpy(&e.block_offset, data + off, 8); off += 8;
            level.push_back(e);
        }
        sl.levels_.push_back(std::move(level));
    }
    return sl;
}

}  // namespace vortex
