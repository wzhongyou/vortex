#include "vortex/inverted/external_id_map.h"

#include <unistd.h>
#include <algorithm>
#include <cstring>

#include "vortex/core/arena.h"

namespace vortex {

const ExternalIdMap::Location* ExternalIdMap::find(
    std::string_view external_id) const {
    auto it = map_.find(std::string(external_id));
    return it != map_.end() ? &it->second : nullptr;
}

void ExternalIdMap::insert(std::string_view external_id,
                            uint64_t seg_id, uint32_t doc_id) {
    map_[std::string(external_id)] = {seg_id, doc_id};
}

void ExternalIdMap::remove(std::string_view external_id) {
    map_.erase(std::string(external_id));
}

Status ExternalIdMap::flush(int fd, Arena& scratch) {
    // Collect entries sorted by internal_doc_id
    std::vector<std::pair<uint32_t, std::string>> entries;
    for (auto& [ext_id, loc] : map_) {
        entries.push_back({loc.internal_doc_id, ext_id});
    }
    std::sort(entries.begin(), entries.end());

    // Binary format: [doc_count:u32][for each: id_len:u16, id:char[]]
    uint32_t count = static_cast<uint32_t>(entries.size());
    write(fd, &count, 4);

    std::vector<std::string> reverse(count);
    for (size_t i = 0; i < entries.size(); i++) {
        uint16_t len = static_cast<uint16_t>(entries[i].second.size());
        write(fd, &len, 2);
        write(fd, entries[i].second.data(), len);
        reverse[entries[i].first] = entries[i].second;
    }

    return Status::OK();
}

Result<std::unique_ptr<ExternalIdMap>> ExternalIdMap::from_file(
    const std::string& path) {
    // Not implemented in V1 header-only mode.
    // Will be implemented when we add file I/O in Segment.
    (void)path;
    return Result<std::unique_ptr<ExternalIdMap>>::Err(
        Status::Internal("not implemented"));
}

std::string ExternalIdMap::resolve(uint32_t doc_id) const {
    if (doc_id < reverse_.size()) return reverse_[doc_id];
    return "<unknown>";
}

void ExternalIdMap::set_reverse_map(std::vector<std::string> doc_to_external_id) {
    reverse_ = std::move(doc_to_external_id);
}

}  // namespace vortex
