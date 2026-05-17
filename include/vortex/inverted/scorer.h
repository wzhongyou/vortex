#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace vortex {

struct BM25Params {
    double k1 = 1.2;
    double b = 0.75;
};

class BM25FScorer {
public:
    BM25FScorer(const BM25Params& params, uint64_t total_docs, double avgdl);

    // Score a single term in a single field.
    double score(uint32_t tf, uint32_t doc_freq, uint32_t field_length) const;

    // Combine term scores for a document.
    double combine(const std::vector<double>& term_scores) const;

    double idf(uint32_t doc_freq) const;

private:
    BM25Params params_;
    uint64_t total_docs_;
    double avgdl_;
};

}  // namespace vortex
