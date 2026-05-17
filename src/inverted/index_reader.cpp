#include "vortex/inverted/index_reader.h"

#include <algorithm>
#include <queue>
#include "vortex/inverted/forward_index.h"
#include "vortex/inverted/delete_bitmap.h"
#include "vortex/inverted/term_dict.h"

#include "vortex/inverted/posting_list.h"
#include "vortex/inverted/scorer.h"
#include "vortex/inverted/segment.h"
#include "vortex/inverted/segment_list.h"

namespace vortex {

IndexReader::IndexReader(SegmentsSnapshot* snapshot,
                         SegmentList* segment_list)
    : snapshot_(snapshot)
    , segment_list_(segment_list) {
    BM25Params params;
    scorer_ = std::make_unique<BM25FScorer>(
        params, snapshot->total_docs, snapshot->avgdl);
}

IndexReader::~IndexReader() {
    if (snapshot_) {
        segment_list_->release_snapshot(snapshot_);
    }
}

// Heap entry for topk results
struct ScoredEntry {
    float score;
    uint32_t doc_id;
    uint64_t segment_id;

    bool operator<(const ScoredEntry& other) const {
        return score > other.score;  // min-heap
    }
};

// Execute a single TERM query against one segment
static void search_term_in_segment(const Segment& seg,
                                    const std::string& term,
                                    const BM25FScorer& scorer,
                                    std::vector<ScoredEntry>& out) {
    const TermInfo* info = seg.lookup_term(term);
    if (!info) return;

    // Build PostingListReader over just this term's posting data
    const uint8_t* term_data = seg.posting_data() + info->posting_offset;
    PostingListReader plr(term_data, info->posting_len);

    plr.for_each([&](uint32_t doc_id, uint32_t tf) {
        // Check if deleted
        if (seg.deletes() && seg.deletes()->is_deleted(doc_id)) return;

        // Get forward info for this doc
        auto doc_info = seg.forward_index()
                            ? seg.forward_index()->get(doc_id)
                            : ForwardIndex::DocInfo{0, nullptr};

        // Score
        double s = scorer.score(tf, info->doc_freq, doc_info.doc_length);
        out.push_back({static_cast<float>(s), doc_id, seg.id()});
    });
}

Result<SearchResult> IndexReader::search(const Query& query, size_t topk) {
    SearchResult result;
    stats_.queries.inc();

    if (!snapshot_ || snapshot_->segments.empty()) {
        result.total_hits = 0;
        result.elapsed_us = 0;
        return Result<SearchResult>::Ok(std::move(result));
    }

    // Collect all scored docs from all segments
    std::vector<ScoredEntry> all_results;

    if (query.type == QueryType::TERM) {
        for (auto& seg : snapshot_->segments) {
            search_term_in_segment(*seg, query.term, *scorer_, all_results);
        }
    } else if (query.type == QueryType::AND) {
        // AND: intersect posting lists across terms within each segment.
        // V1: simplified — execute each sub-query independently and intersect.
        // Full intersection with advance_to will be in V2.
        (void)query; // not yet implemented for complex queries
    } else if (query.type == QueryType::OR) {
        // OR: union posting lists.
        (void)query; // not yet implemented for complex queries
    }

    // Sort by score descending, take topk
    std::sort(all_results.begin(), all_results.end(),
              [](const ScoredEntry& a, const ScoredEntry& b) {
                  return a.score > b.score;
              });

    if (all_results.size() > topk) {
        all_results.resize(topk);
    }

    result.total_hits = all_results.size();
    for (auto& entry : all_results) {
        ScoredDoc doc;
        doc.score = entry.score;
        doc.segment_id = entry.segment_id;
        doc.internal_doc_id = entry.doc_id;
        doc.external_id = std::to_string(entry.doc_id);  // placeholder
        result.docs.push_back(std::move(doc));
    }

    result.elapsed_us = 0;
    return Result<SearchResult>::Ok(std::move(result));
}

}  // namespace vortex
