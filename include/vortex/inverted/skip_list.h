#pragma once

#include <cstdint>
#include <vector>

namespace vortex {

struct SkipEntry {
    uint32_t last_doc_id;    // max doc_id in the skipped block(s)
    uint64_t block_offset;   // file offset of the target block
};

class SkipList {
public:
    SkipList() = default;

    // Build multi-level skip list from block metadata.
    // skip_interval: blocks between skip entries at level 0 (default 4).
    void build(const std::vector<uint64_t>& block_offsets,
               const std::vector<uint32_t>& block_max_doc,
               int skip_interval = 4);

    // Find the block offset just before the first block with max_doc >= target.
    // Returns 0 if target is before all blocks (start from block 0).
    // Returns UINT64_MAX if target is beyond all blocks.
    uint64_t advance(uint32_t target, uint64_t current_offset) const;

    // Serialize to byte array for storage in .doc file tail.
    std::vector<uint8_t> serialize() const;

    // Deserialize from byte array.
    static SkipList deserialize(const uint8_t* data, size_t len);

    bool empty() const { return levels_.empty(); }
    size_t level_count() const { return levels_.size(); }

private:
    // levels_[0]: every skip_interval blocks
    // levels_[1]: every skip_interval^2 blocks, etc.
    std::vector<std::vector<SkipEntry>> levels_;
};

}  // namespace vortex
