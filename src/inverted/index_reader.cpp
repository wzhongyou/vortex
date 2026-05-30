#include "vortex/inverted/index_reader.h"

#include <algorithm>
#include <optional>
#include <queue>
#include <unordered_map>
#include "vortex/core/document.h"
#include "vortex/core/schema.h"
#include "vortex/inverted/forward_index.h"
#include "vortex/inverted/delete_bitmap.h"
#include "vortex/inverted/term_dict.h"

#include "vortex/inverted/posting_list.h"
#include "vortex/inverted/scorer.h"
#include "vortex/inverted/segment.h"
#include "vortex/inverted/segment_list.h"

namespace vortex {

IndexReader::IndexReader(SegmentsSnapshot* snapshot,
                         SegmentList* segment_list,
                         const Schema* schema)
    : snapshot_(snapshot)
    , segment_list_(segment_list)
    , schema_(schema) {
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

// ── Forward declarations ──

static void search_query_in_segment(const Segment& seg,
                                     const Query& query,
                                     const Scorer& scorer,
                                     std::vector<ScoredEntry>& out);

// ── TERM ──

static void search_term_in_segment(const Segment& seg,
                                    const std::string& term,
                                    const Scorer& scorer,
                                    std::vector<ScoredEntry>& out) {
    const TermInfo* info = seg.lookup_term(term);
    if (!info) return;

    const uint8_t* term_data = seg.posting_data() + info->posting_offset;
    PostingListReader plr(term_data, info->posting_len);

    plr.for_each([&](uint32_t doc_id, uint32_t tf) {
        if (seg.deletes() && seg.deletes()->is_deleted(doc_id)) return;

        auto doc_info = seg.forward_index()
                            ? seg.forward_index()->get(doc_id)
                            : ForwardIndex::DocInfo{0, nullptr};

        double s = scorer.score(tf, info->doc_freq, doc_info.doc_length);
        out.push_back({static_cast<float>(s), doc_id, seg.id()});
    });
}

// ── AND ──

static void search_and_in_segment(const Segment& seg,
                                   const std::vector<Query>& sub_queries,
                                   const Scorer& scorer,
                                   std::vector<ScoredEntry>& out) {
    if (sub_queries.empty()) return;

    std::vector<std::vector<ScoredEntry>> sub_results(sub_queries.size());
    for (size_t i = 0; i < sub_queries.size(); i++) {
        search_query_in_segment(seg, sub_queries[i], scorer, sub_results[i]);
        std::sort(sub_results[i].begin(), sub_results[i].end(),
                  [](const ScoredEntry& a, const ScoredEntry& b) {
                      return a.doc_id < b.doc_id;
                  });
    }

    // Drive intersection from the smallest result set
    size_t driver_idx = 0;
    for (size_t i = 1; i < sub_results.size(); i++) {
        if (sub_results[i].size() < sub_results[driver_idx].size()) {
            driver_idx = i;
        }
    }

    for (auto& entry : sub_results[driver_idx]) {
        double combined = entry.score;
        bool all_match = true;

        for (size_t i = 0; i < sub_results.size(); i++) {
            if (i == driver_idx) continue;
            auto& set = sub_results[i];
            auto it = std::lower_bound(set.begin(), set.end(), entry.doc_id,
                [](const ScoredEntry& e, uint32_t doc_id) { return e.doc_id < doc_id; });
            if (it == set.end() || it->doc_id != entry.doc_id) {
                all_match = false;
                break;
            }
            combined += it->score;
        }

        if (all_match) {
            out.push_back({static_cast<float>(combined), entry.doc_id, seg.id()});
        }
    }
}

// ── OR ──

static void search_or_in_segment(const Segment& seg,
                                  const std::vector<Query>& sub_queries,
                                  const Scorer& scorer,
                                  std::vector<ScoredEntry>& out) {
    if (sub_queries.empty()) return;

    std::vector<std::vector<ScoredEntry>> sub_results(sub_queries.size());
    for (size_t i = 0; i < sub_queries.size(); i++) {
        search_query_in_segment(seg, sub_queries[i], scorer, sub_results[i]);
    }

    if (sub_results.size() == 1) {
        out = std::move(sub_results[0]);
        return;
    }

    // Union: map doc_id to max score across all sub-results
    std::unordered_map<uint32_t, float> doc_scores;
    for (auto& set : sub_results) {
        for (auto& entry : set) {
            auto it = doc_scores.find(entry.doc_id);
            if (it == doc_scores.end() || entry.score > it->second) {
                doc_scores[entry.doc_id] = entry.score;
            }
        }
    }

    out.reserve(doc_scores.size());
    for (auto& pair : doc_scores) {
        out.push_back({pair.second, pair.first, seg.id()});
    }
}

// ── NOT ──

static void search_not_in_segment(const Segment& seg,
                                   const std::vector<Query>& sub_queries,
                                   const Scorer& scorer,
                                   std::vector<ScoredEntry>& out) {
    if (sub_queries.empty()) return;

    // First sub-query is the positive set
    search_query_in_segment(seg, sub_queries[0], scorer, out);

    // Second sub-query (if any) is the negative (exclusion) set
    if (sub_queries.size() >= 2) {
        std::vector<ScoredEntry> exclude;
        search_query_in_segment(seg, sub_queries[1], scorer, exclude);

        if (!exclude.empty()) {
            std::sort(exclude.begin(), exclude.end(),
                      [](const ScoredEntry& a, const ScoredEntry& b) {
                          return a.doc_id < b.doc_id;
                      });

            std::vector<ScoredEntry> filtered;
            filtered.reserve(out.size());
            for (auto& entry : out) {
                auto it = std::lower_bound(exclude.begin(), exclude.end(), entry.doc_id,
                    [](const ScoredEntry& e, uint32_t doc_id) { return e.doc_id < doc_id; });
                if (it == exclude.end() || it->doc_id != entry.doc_id) {
                    filtered.push_back(entry);
                }
            }
            out = std::move(filtered);
        }
    } else {
        // Unary NOT: no positive base to match against
        out.clear();
    }
}

// ── Query dispatch ──

static void search_query_in_segment(const Segment& seg,
                                     const Query& query,
                                     const Scorer& scorer,
                                     std::vector<ScoredEntry>& out) {
    switch (query.type) {
        case QueryType::TERM:
            search_term_in_segment(seg, query.term, scorer, out);
            break;
        case QueryType::AND:
            search_and_in_segment(seg, query.sub_queries, scorer, out);
            break;
        case QueryType::OR:
            search_or_in_segment(seg, query.sub_queries, scorer, out);
            break;
        case QueryType::NOT:
            search_not_in_segment(seg, query.sub_queries, scorer, out);
            break;
    }
}

// ── Main search ──

Result<SearchResult> IndexReader::search(const Query& query, size_t topk) {
    SearchResult result;
    stats_.queries.inc();

    if (!snapshot_ || snapshot_->segments.empty()) {
        result.total_hits = 0;
        result.elapsed_us = 0;
        return Result<SearchResult>::Ok(std::move(result));
    }

    // Execute query across all segments
    std::vector<ScoredEntry> all_results;
    for (auto& seg : snapshot_->segments) {
        search_query_in_segment(*seg, query, *scorer_, all_results);
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
        doc.external_id = std::to_string(entry.doc_id);
        for (auto& seg : snapshot_->segments) {
            if (seg->id() == entry.segment_id) {
                doc.external_id = seg->resolve_external_id(entry.doc_id);
                break;
            }
        }
        result.docs.push_back(std::move(doc));
    }

    result.elapsed_us = 0;
    return Result<SearchResult>::Ok(std::move(result));
}

Result<std::optional<Document>> IndexReader::get_document(
    std::string_view external_id) {
    if (!snapshot_ || !schema_) {
        return Result<std::optional<Document>>::Ok(std::nullopt);
    }

    // Find which segment holds this external_id
    for (auto& seg : snapshot_->segments) {
        uint32_t doc_id = seg->find_doc_id(external_id);
        if (doc_id == kInvalidDocId) continue;

        // Read stored field values from the segment
        std::vector<std::string> values;
        seg->get_stored_values(doc_id, values, schema_->stored_field_count());

        // Reconstruct Document using schema stored field names
        Document doc;
        uint16_t si = 0;
        for (auto& fs : schema_->fields) {
            if (fs.stored) {
                doc.fields.push_back({
                    fs.name,
                    si < values.size() ? values[si] : std::string()
                });
                si++;
            }
        }
        return Result<std::optional<Document>>::Ok(std::move(doc));
    }

    return Result<std::optional<Document>>::Ok(std::nullopt);
}

}  // namespace vortex