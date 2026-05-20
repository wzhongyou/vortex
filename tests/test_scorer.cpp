#include "gtest/gtest.h"
#include "vortex/inverted/scorer.h"

namespace vortex {
namespace {

TEST(BM25FScorerTest, BasicScoring) {
    BM25Params params{1.2, 0.75};
    BM25FScorer scorer(params, 100, 50.0);  // 100 docs total, avgdl=50

    // Common term: doc_freq=10, tf=2, field_length=40
    double s = scorer.score(2, 10, 40);
    EXPECT_GT(s, 0.0);

    // Rare term should score higher
    double rare = scorer.score(1, 1, 40);
    EXPECT_GT(rare, s);
}

TEST(BM25FScorerTest, IDF) {
    BM25Params params{1.2, 0.75};
    BM25FScorer scorer(params, 100, 50.0);

    // IDF formula: ln((N - n + 0.5) / (n + 0.5) + 1)
    double idf_rare = scorer.idf(1);    // very rare
    double idf_common = scorer.idf(50);  // half of docs
    double idf_all = scorer.idf(100);    // all docs
    EXPECT_GT(idf_rare, idf_common);
    EXPECT_GT(idf_common, idf_all);
    EXPECT_GT(idf_all, 0.0);  // +1 ensures always positive
}

TEST(BM25FScorerTest, TermFrequencySaturation) {
    BM25Params params{1.2, 0.75};
    BM25FScorer scorer(params, 100, 50.0);

    // tf=1 vs tf=100: BM25 has diminishing returns on tf
    double s1 = scorer.score(1, 10, 50);
    double s100 = scorer.score(100, 10, 50);
    // s100 should be higher but not 100x higher
    EXPECT_GT(s100, s1);
    EXPECT_LT(s100, s1 * 20);  // saturation check
}

TEST(BM25FScorerTest, FieldLengthNormalization) {
    BM25Params params{1.2, 0.75};
    BM25FScorer scorer(params, 100, 50.0);

    // Same tf in a shorter field → higher score
    double short_field = scorer.score(2, 10, 10);  // field_length=10
    double long_field = scorer.score(2, 10, 100);   // field_length=100
    EXPECT_GT(short_field, long_field);
}

TEST(BM25FScorerTest, CombineScores) {
    BM25Params params{1.2, 0.75};
    BM25FScorer scorer(params, 100, 50.0);

    // Combine term scores
    double combined = scorer.combine({1.5, 2.0, 0.5});
    EXPECT_EQ(combined, 4.0);  // simple sum in V1
}

TEST(BM25FScorerTest, NegativeIDFNotPossible) {
    BM25Params params{1.2, 0.75};
    BM25FScorer scorer(params, 100, 50.0);

    // Even a term in all docs should have positive IDF due to +1 smoothing
    double idf = scorer.idf(100);
    EXPECT_GT(idf, 0.0);
}

TEST(BM25FScorerTest, ZeroDocFreqEdgeCase) {
    BM25Params params{1.2, 0.75};
    BM25FScorer scorer(params, 100, 50.0);

    // Zero doc_freq should give high but finite IDF
    double idf = scorer.idf(0);
    EXPECT_GT(idf, 0.0);
    EXPECT_LT(idf, 100.0);  // reasonable bound
}

TEST(BM25FScorerTest, DifferentK1Values) {
    BM25Params low_k1{0.5, 0.75};
    BM25Params high_k1{2.0, 0.75};
    BM25FScorer scorer_low(low_k1, 100, 50.0);
    BM25FScorer scorer_high(high_k1, 100, 50.0);

    // Higher k1 means less tf saturation → higher score for high tf
    double s_low = scorer_low.score(10, 10, 50);
    double s_high = scorer_high.score(10, 10, 50);
    EXPECT_GT(s_high, s_low);
}

}  // namespace
}  // namespace vortex