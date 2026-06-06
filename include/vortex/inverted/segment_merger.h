#pragma once

#include <memory>
#include <string>
#include <vector>

#include "vortex/core/result.h"

namespace vortex {

class Arena;
struct Segment;

class SegmentMerger {
public:
    // Merge multiple segments into one. Returns the new merged segment.
    // All segments must have been created from the same schema.
    // stored_field_count: from Schema::stored_field_count()
    static Result<std::shared_ptr<const Segment>> merge(
        const std::string& index_dir,
        const std::vector<std::shared_ptr<const Segment>>& segments,
        uint64_t new_segment_id,
        uint16_t stored_field_count,
        Arena& arena);
};

}  // namespace vortex