#include "vortex/inverted/external_id_map.h"

#include <unistd.h>
#include <fcntl.h>
#include <algorithm>
#include <cstring>
#include <vector>

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
    ssize_t written = write(fd, &count, 4);
    (void)written;

    std::vector<std::string> reverse(count);
    for (size_t i = 0; i < entries.size(); i++) {
        uint16_t len = static_cast<uint16_t>(entries[i].second.size());
        ssize_t w1 = write(fd, &len, 2);
        ssize_t w2 = write(fd, entries[i].second.data(), len);
        (void)w1; (void)w2;
        reverse[entries[i].first] = entries[i].second;
    }

    return Status::OK();
}

Result<std::unique_ptr<ExternalIdMap>> ExternalIdMap::from_file(
    const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return Result<std::unique_ptr<ExternalIdMap>>::Err(
            Status::IOError("cannot open " + path));
    }

    off_t file_size = lseek(fd, 0, SEEK_END);
    if (file_size <= 0) {
        close(fd);
        return Result<std::unique_ptr<ExternalIdMap>>::Ok(
            std::make_unique<ExternalIdMap>());
    }
    lseek(fd, 0, SEEK_SET);

    std::vector<uint8_t> buf(static_cast<size_t>(file_size));
    ssize_t n = read(fd, buf.data(), buf.size());
    close(fd);
    if (n <= 0) {
        return Result<std::unique_ptr<ExternalIdMap>>::Ok(
            std::make_unique<ExternalIdMap>());
    }

    auto map = std::make_unique<ExternalIdMap>();
    size_t off = 0;
    if (off + 4 > buf.size()) return Result<std::unique_ptr<ExternalIdMap>>::Ok(std::move(map));

    uint32_t count;
    std::memcpy(&count, &buf[off], 4);
    off += 4;

    for (uint32_t i = 0; i < count; i++) {
        if (off + 2 > buf.size()) break;
        uint16_t len;
        std::memcpy(&len, &buf[off], 2);
        off += 2;
        if (off + len > buf.size()) break;
        std::string ext_id(reinterpret_cast<const char*>(&buf[off]), len);
        off += len;
        map->map_[ext_id] = {0, i};  // segment_id=0, internal_doc_id=i
    }

    return Result<std::unique_ptr<ExternalIdMap>>::Ok(std::move(map));
}

std::string ExternalIdMap::resolve(uint32_t doc_id) const {
    if (doc_id < reverse_.size()) return reverse_[doc_id];
    return "<unknown>";
}

void ExternalIdMap::set_reverse_map(std::vector<std::string> doc_to_external_id) {
    reverse_ = std::move(doc_to_external_id);
}

}  // namespace vortex