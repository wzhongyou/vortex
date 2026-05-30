#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include "vortex/core/types.h"

namespace vortex {

struct Segment;

struct SegmentsSnapshot {
    std::atomic<int32_t> active_readers{0};
    uint64_t epoch_id;
    std::vector<std::shared_ptr<const Segment>> segments;

    uint64_t total_docs = 0;
    double avgdl = 0.0;
    std::vector<double> field_avg_lengths;
};

struct SegmentList {
    SegmentList();

    const SegmentsSnapshot* acquire_snapshot();
    void release_snapshot(SegmentsSnapshot* snap);

    void publish_segment(std::shared_ptr<const Segment> seg);
    void remove_segments(const std::vector<uint64_t>& segment_ids);
    void reclaim_retired_snapshots();

private:
    std::atomic<SegmentsSnapshot*> current_{nullptr};
    std::vector<SegmentsSnapshot*> retired_;
    uint64_t next_epoch_{1};
};

}  // namespace vortex
