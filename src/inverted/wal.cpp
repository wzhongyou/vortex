#include "vortex/inverted/wal.h"

#include <fcntl.h>
#include <unistd.h>
#include <cstring>

namespace vortex {

WAL::WAL(const std::string& path) {
    fd_ = open(path.c_str(), O_CREAT | O_RDWR | O_APPEND, 0644);
}

Status WAL::append_add(uint32_t internal_id, std::string_view external_id,
                        const Document& doc) {
    if (fd_ < 0) return Status::IOError("WAL not open");
    // V1: simple binary format
    uint8_t op = 0x01;  // ADD
    write(fd_, &op, 1);
    write(fd_, &internal_id, 4);
    uint16_t id_len = static_cast<uint16_t>(external_id.size());
    write(fd_, &id_len, 2);
    write(fd_, external_id.data(), id_len);
    // Simplified: write field count + fields
    uint16_t fc = static_cast<uint16_t>(doc.fields.size());
    write(fd_, &fc, 2);
    for (auto& f : doc.fields) {
        uint16_t nl = static_cast<uint16_t>(f.name.size());
        write(fd_, &nl, 2);
        write(fd_, f.name.data(), nl);
        uint32_t vl = static_cast<uint32_t>(f.value.size());
        write(fd_, &vl, 4);
        write(fd_, f.value.data(), vl);
    }
    bytes_written_ += 1 + 4 + 2 + id_len + 2;
    return Status::OK();
}

Status WAL::append_remove(std::string_view external_id) {
    if (fd_ < 0) return Status::IOError("WAL not open");
    uint8_t op = 0x02;
    write(fd_, &op, 1);
    uint16_t id_len = static_cast<uint16_t>(external_id.size());
    write(fd_, &id_len, 2);
    write(fd_, external_id.data(), id_len);
    bytes_written_ += 1 + 2 + id_len;
    return Status::OK();
}

Status WAL::sync() {
    if (fd_ >= 0) fsync(fd_);
    return Status::OK();
}

Status WAL::truncate() {
    if (fd_ >= 0) ftruncate(fd_, 0);
    bytes_written_ = 0;
    return Status::OK();
}

Result<WAL::RecoveryState> WAL::recover(const Schema& schema) {
    RecoveryState state;
    state.next_internal_id = 0;
    return Result<RecoveryState>::Ok(std::move(state));
}

}  // namespace vortex
