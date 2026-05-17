#pragma once

#include <memory>
#include <vector>

#include "vortex/core/query.h"
#include "vortex/core/result.h"
#include "vortex/core/search_result.h"
#include "vortex/core/stats.h"

namespace vortex {

struct SegmentsSnapshot;
class BM25FScorer;

class IndexReader {
public:
    IndexReader(SegmentsSnapshot* snapshot,
                struct SegmentList* segment_list);
    ~IndexReader();

    Result<SearchResult> search(const Query& query, size_t topk = 10);

    const QueryStats& stats() const { return stats_; }

private:
    SegmentsSnapshot* snapshot_;
    SegmentList* segment_list_;
    std::unique_ptr<BM25FScorer> scorer_;
    mutable QueryStats stats_;
};

}  // namespace vortex
