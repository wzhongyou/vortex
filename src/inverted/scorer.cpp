#include "vortex/inverted/scorer.h"

#include <cmath>

namespace vortex {

BM25FScorer::BM25FScorer(const BM25Params& params, uint64_t total_docs, double avgdl)
    : params_(params), total_docs_(total_docs), avgdl_(avgdl) {}

double BM25FScorer::score(uint32_t tf, uint32_t doc_freq,
                           uint32_t field_length) const {
    double idf_val = idf(doc_freq);
    double tf_norm = (tf * (params_.k1 + 1.0)) /
                     (tf + params_.k1 * (1.0 - params_.b +
                      params_.b * field_length / avgdl_));
    return idf_val * tf_norm;
}

double BM25FScorer::combine(const std::vector<double>& term_scores) const {
    double total = 0.0;
    for (auto s : term_scores) total += s;
    return total;
}

double BM25FScorer::idf(uint32_t doc_freq) const {
    return std::log((static_cast<double>(total_docs_) - doc_freq + 0.5) /
                    (doc_freq + 0.5) + 1.0);
}

}  // namespace vortex
