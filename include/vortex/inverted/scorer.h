#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace vortex {

struct BM25Params {
    double k1 = 1.2;
    double b = 0.75;
};

// Abstract base class for scoring.
class Scorer {
public:
    virtual ~Scorer() = default;

    // Score a single term in a single field.
    virtual double score(uint32_t tf, uint32_t doc_freq, uint32_t field_length) const = 0;

    // Combine term scores for a document.
    virtual double combine(const std::vector<double>& term_scores) const = 0;

    virtual double idf(uint32_t doc_freq) const = 0;
};

class BM25FScorer final : public Scorer {
public:
    BM25FScorer(const BM25Params& params, uint64_t total_docs, double avgdl);

    double score(uint32_t tf, uint32_t doc_freq, uint32_t field_length) const override;

    double combine(const std::vector<double>& term_scores) const override;

    double idf(uint32_t doc_freq) const override;

private:
    BM25Params params_;
    uint64_t total_docs_;
    double avgdl_;
};

}  // namespace vortex
