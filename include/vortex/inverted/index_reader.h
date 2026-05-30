#pragma once

#include <optional>
#include <memory>
#include <vector>

#include "vortex/core/document.h"
#include "vortex/core/query.h"
#include "vortex/core/result.h"
#include "vortex/core/search_result.h"
#include "vortex/core/stats.h"

namespace vortex {

class Schema;
struct SegmentsSnapshot;
class Scorer;

class IndexReader {
public:
    IndexReader(SegmentsSnapshot* snapshot,
                struct SegmentList* segment_list,
                const Schema* schema);
    ~IndexReader();

    Result<SearchResult> search(const Query& query, size_t topk = 10);
    Result<std::optional<Document>> get_document(std::string_view external_id);

    const QueryStats& stats() const { return stats_; }

private:
    SegmentsSnapshot* snapshot_;
    SegmentList* segment_list_;
    const Schema* schema_;
    std::unique_ptr<Scorer> scorer_;
    mutable QueryStats stats_;
};

}  // namespace vortex