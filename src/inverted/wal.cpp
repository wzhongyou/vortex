#include "vortex/inverted/wal.h"

#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <vector>

#include "vortex/core/schema.h"

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

    if (fd_ < 0) {
        return Result<RecoveryState>::Ok(std::move(state));
    }

    // Seek to beginning for reading
    off_t file_size = lseek(fd_, 0, SEEK_END);
    if (file_size <= 0) {
        return Result<RecoveryState>::Ok(std::move(state));
    }
    lseek(fd_, 0, SEEK_SET);

    // Read entire file into buffer
    std::vector<uint8_t> buf(static_cast<size_t>(file_size));
    ssize_t n = read(fd_, buf.data(), buf.size());
    if (n <= 0) {
        return Result<RecoveryState>::Ok(std::move(state));
    }
    buf.resize(static_cast<size_t>(n));

    // Parse records
    size_t off = 0;
    uint32_t max_id = 0;
    while (off < buf.size()) {
        if (off >= buf.size()) break;
        uint8_t op = buf[off++];

        if (op == 0x01) {  // ADD
            if (off + 4 > buf.size()) break;
            uint32_t internal_id;
            std::memcpy(&internal_id, &buf[off], 4);
            off += 4;
            if (internal_id > max_id) max_id = internal_id;

            if (off + 2 > buf.size()) break;
            uint16_t id_len;
            std::memcpy(&id_len, &buf[off], 2);
            off += 2;
            if (off + id_len > buf.size()) break;
            std::string external_id(reinterpret_cast<const char*>(&buf[off]), id_len);
            off += id_len;

            if (off + 2 > buf.size()) break;
            uint16_t fc;
            std::memcpy(&fc, &buf[off], 2);
            off += 2;

            Document doc;
            for (uint16_t i = 0; i < fc; i++) {
                if (off + 2 > buf.size()) break;
                uint16_t nl;
                std::memcpy(&nl, &buf[off], 2);
                off += 2;
                if (off + nl > buf.size()) break;
                std::string name(reinterpret_cast<const char*>(&buf[off]), nl);
                off += nl;

                if (off + 4 > buf.size()) break;
                uint32_t vl;
                std::memcpy(&vl, &buf[off], 4);
                off += 4;
                if (off + vl > buf.size()) break;
                std::string value(reinterpret_cast<const char*>(&buf[off]), vl);
                off += vl;

                doc.fields.push_back({std::move(name), std::move(value)});
            }

            state.active_docs.push_back({internal_id, {std::move(external_id), std::move(doc)}});
        } else if (op == 0x02) {  // REMOVE
            if (off + 2 > buf.size()) break;
            uint16_t id_len;
            std::memcpy(&id_len, &buf[off], 2);
            off += 2;
            if (off + id_len > buf.size()) break;
            std::string external_id(reinterpret_cast<const char*>(&buf[off]), id_len);
            off += id_len;
            state.removed_ids.push_back(std::move(external_id));
        } else {
            // Unknown opcode or corrupted data — stop recovery
            break;
        }
    }

    state.next_internal_id = max_id;
    return Result<RecoveryState>::Ok(std::move(state));
}

}  // namespace vortex