#include "vortex/core/stats.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

namespace vortex {

Histogram::Histogram(std::vector<uint64_t> bucket_upper_bounds)
    : upper_bounds_(std::move(bucket_upper_bounds))
    , buckets_(upper_bounds_.size() + 1) {}

void Histogram::record(uint64_t value_us) {
    uint64_t cur_min = min_.load(std::memory_order_relaxed);
    while (value_us < cur_min &&
           !min_.compare_exchange_weak(cur_min, value_us, std::memory_order_relaxed)) {}
    uint64_t cur_max = max_.load(std::memory_order_relaxed);
    while (value_us > cur_max &&
           !max_.compare_exchange_weak(cur_max, value_us, std::memory_order_relaxed)) {}

    total_count_.inc();
    total_sum_.add(value_us);

    size_t bucket = 0;
    while (bucket < upper_bounds_.size() && value_us >= upper_bounds_[bucket]) {
        bucket++;
    }
    buckets_[bucket].fetch_add(1, std::memory_order_relaxed);
}

uint64_t Histogram::count() const {
    return total_count_.get();
}

uint64_t Histogram::min() const {
    uint64_t v = min_.load(std::memory_order_relaxed);
    return v == UINT64_MAX ? 0 : v;
}

uint64_t Histogram::max() const {
    return max_.load(std::memory_order_relaxed);
}

double Histogram::avg() const {
    uint64_t n = total_count_.get();
    if (n == 0) return 0.0;
    return static_cast<double>(total_sum_.get()) / n;
}

double Histogram::percentile(double p) const {
    uint64_t total = total_count_.get();
    if (total == 0) return 0.0;

    uint64_t target = static_cast<uint64_t>(std::ceil(p / 100.0 * total));
    uint64_t cumulative = 0;

    for (size_t i = 0; i < buckets_.size(); i++) {
        cumulative += buckets_[i].load(std::memory_order_relaxed);
        if (cumulative >= target) {
            if (i < upper_bounds_.size()) {
                return static_cast<double>(upper_bounds_[i]);
            }
            return static_cast<double>(upper_bounds_.back()) * 2.0;
        }
    }
    return static_cast<double>(upper_bounds_.back()) * 2.0;
}

IndexStats::IndexStats()
    : flush_latency_ms({10, 50, 100, 500, 1000, 5000})
    , doc_add_latency_us({10, 50, 100, 500, 1000, 5000}) {}

QueryStats::QueryStats()
    : query_latency_ms({1, 5, 10, 25, 50, 100, 250, 500})
    , segments_queried({1, 2, 5, 10, 25, 50})
    , docs_scored({10, 100, 1000, 10000, 100000})
    , posting_bytes_read({1024, 16384, 65536, 262144, 1048576}) {}

}  // namespace vortex
