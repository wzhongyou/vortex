#include "vortex/inverted/segment_list.h"

#include "vortex/inverted/segment.h"

namespace vortex {

SegmentList::SegmentList() {
    auto* snap = new SegmentsSnapshot();
    snap->epoch_id = next_epoch_++;
    current_.store(snap);
}

const SegmentsSnapshot* SegmentList::acquire_snapshot() {
    auto* snap = current_.load(std::memory_order_acquire);
    snap->active_readers.fetch_add(int32_t(1), std::memory_order_relaxed);
    return snap;
}

void SegmentList::release_snapshot(SegmentsSnapshot* snap) {
    snap->active_readers.fetch_add(int32_t(-1), std::memory_order_relaxed);
}

void SegmentList::publish_segment(std::shared_ptr<const Segment> seg) {
    auto* old = current_.load();
    auto* snap = new SegmentsSnapshot();
    snap->epoch_id = next_epoch_++;
    snap->segments = old->segments;
    snap->segments.push_back(std::move(seg));

    // Recompute statistics
    uint64_t total_docs = 0;
    uint64_t total_terms = 0;
    for (auto& s : snap->segments) {
        total_docs += s->doc_count();
        total_terms += static_cast<uint64_t>(s->avgdl() * s->doc_count());
    }
    snap->total_docs = total_docs;
    snap->avgdl = total_docs > 0 ? static_cast<double>(total_terms) / total_docs : 0.0;

    current_.store(snap, std::memory_order_release);

    // Retire old snapshot
    retired_.push_back(old);
}

void SegmentList::reclaim_retired_snapshots() {
    auto it = retired_.begin();
    while (it != retired_.end()) {
        if ((*it)->active_readers.load() == 0) {
            delete *it;
            it = retired_.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace vortex
