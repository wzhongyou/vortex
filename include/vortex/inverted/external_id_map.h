#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "vortex/core/result.h"
#include "vortex/core/status.h"
#include "vortex/core/types.h"

namespace vortex {

class Arena;

// Maps external_id → (segment_id, internal_doc_id).
class ExternalIdMap {
public:
    struct Location {
        uint64_t segment_id;
        uint32_t internal_doc_id;
    };

    ExternalIdMap() = default;

    const Location* find(std::string_view external_id) const;
    void insert(std::string_view external_id, uint64_t seg_id, uint32_t doc_id);
    void remove(std::string_view external_id);

    // Write .idm file (sorted by internal_doc_id).
    // Also builds the reverse mapping for search result resolution.
    Status flush(int fd, Arena& scratch);

    // Load from .idm file.
    static Result<std::unique_ptr<ExternalIdMap>> from_file(
        const std::string& path);

    size_t size() const { return map_.size(); }

    // Resolve internal doc_id → external_id for search results.
    std::string resolve(uint32_t doc_id) const;

    // Build reverse mapping (called after flushing segment).
    void set_reverse_map(std::vector<std::string> doc_to_external_id);

private:
    std::unordered_map<std::string, Location> map_;
    std::vector<std::string> reverse_;  // doc_id → external_id (for this segment)
};

}  // namespace vortex
