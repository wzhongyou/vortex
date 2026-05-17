#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "vortex/core/document.h"
#include "vortex/core/result.h"
#include "vortex/core/status.h"

namespace vortex {

class Schema;

class WAL {
public:
    explicit WAL(const std::string& path);

    Status append_add(uint32_t internal_id, std::string_view external_id,
                       const Document& doc);
    Status append_remove(std::string_view external_id);
    Status sync();
    Status truncate();

    struct RecoveryState {
        uint32_t next_internal_id;
        std::vector<std::pair<uint32_t, std::pair<std::string, Document>>> active_docs;
        std::vector<std::string> removed_ids;
    };
    Result<RecoveryState> recover(const Schema& schema);

    uint64_t bytes_written() const { return bytes_written_; }

private:
    int fd_;
    std::vector<uint8_t> write_buf_;
    uint64_t bytes_written_ = 0;
};

}  // namespace vortex
