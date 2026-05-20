#include <vector>

#include <benchmark/benchmark.h>

#include "vortex/inverted/scorer.h"

using namespace vortex;

static void BM_BM25FScore(benchmark::State& state) {
    BM25Params params{1.2, 0.75};
    BM25FScorer scorer(params, 1000000, 500.0);

    for (auto _ : state) {
        double s = scorer.score(3, 5000, 400);
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_BM25FScore);

static void BM_BM25FIDF(benchmark::State& state) {
    BM25Params params{1.2, 0.75};
    BM25FScorer scorer(params, 1000000, 500.0);

    for (auto _ : state) {
        double idf = scorer.idf(5000);
        benchmark::DoNotOptimize(idf);
    }
}
BENCHMARK(BM_BM25FIDF);

static void BM_BM25FCombine(benchmark::State& state) {
    BM25Params params{1.2, 0.75};
    BM25FScorer scorer(params, 1000000, 500.0);

    std::vector<double> scores = {1.5, 2.3, 0.8, 1.1, 3.2};

    for (auto _ : state) {
        double c = scorer.combine(scores);
        benchmark::DoNotOptimize(c);
    }
}
BENCHMARK(BM_BM25FCombine);

BENCHMARK_MAIN();