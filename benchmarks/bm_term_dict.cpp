#include <cstdio>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "vortex/inverted/term_dict.h"

using namespace vortex;

// Build a large FST dictionary and benchmark lookups
class TermDictBench : public benchmark::Fixture {
public:
    void SetUp(const ::benchmark::State&) override {
        TermDictBuilder builder;
        for (int i = 0; i < kNumTerms; i++) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "term_%06d", i);
            builder.insert(buf, TermInfo{
                static_cast<uint32_t>(i % 500),
                static_cast<uint64_t>(i) * 100,
                64
            });
        }
        auto fst = builder.finish();
        auto result = TermDict::from_memory(fst.data(), fst.size());
        dict_ = result.move_value();
    }

    void TearDown(const ::benchmark::State&) override {
        dict_.reset();
    }

    static constexpr int kNumTerms = 50000;
    std::unique_ptr<TermDict> dict_;
};

BENCHMARK_DEFINE_F(TermDictBench, LookupHit)(benchmark::State& state) {
    int idx = 0;
    for (auto _ : state) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "term_%06d", idx % kNumTerms);
        auto* info = dict_->lookup(buf);
        benchmark::DoNotOptimize(info);
        idx++;
    }
}
BENCHMARK_REGISTER_F(TermDictBench, LookupHit)->Iterations(10000);

BENCHMARK_DEFINE_F(TermDictBench, LookupMiss)(benchmark::State& state) {
    for (auto _ : state) {
        auto* info = dict_->lookup("nonexistent_term_xyz");
        benchmark::DoNotOptimize(info);
    }
}
BENCHMARK_REGISTER_F(TermDictBench, LookupMiss)->Iterations(10000);

BENCHMARK_DEFINE_F(TermDictBench, PrefixRange)(benchmark::State& state) {
    for (auto _ : state) {
        int count = 0;
        dict_->prefix_range("term_01", [&](std::string_view, const TermInfo&) {
            count++;
            return count < 100;
        });
        benchmark::DoNotOptimize(count);
    }
}
BENCHMARK_REGISTER_F(TermDictBench, PrefixRange)->Iterations(1000);

BENCHMARK_MAIN();