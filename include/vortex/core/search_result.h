#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vortex {

struct ScoredDoc {
    float score;
    std::string external_id;
    uint64_t segment_id;
    uint32_t internal_doc_id;
};

struct SearchResult {
    std::vector<ScoredDoc> docs;
    uint64_t total_hits = 0;
    uint64_t elapsed_us = 0;
};

}  // namespace vortex
