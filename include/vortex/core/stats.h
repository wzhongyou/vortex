#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace vortex {

class AtomicCounter {
public:
    void inc() { val_.fetch_add(1, std::memory_order_relaxed); }
    void add(uint64_t n) { val_.fetch_add(n, std::memory_order_relaxed); }
    uint64_t get() const { return val_.load(std::memory_order_relaxed); }

private:
    std::atomic<uint64_t> val_{0};
};

class Histogram {
public:
    explicit Histogram(std::vector<uint64_t> bucket_upper_bounds);

    void record(uint64_t value_us);

    uint64_t count() const;
    uint64_t min() const;
    uint64_t max() const;
    double avg() const;
    double percentile(double p) const;

private:
    std::vector<uint64_t> upper_bounds_;
    std::vector<std::atomic<uint64_t>> buckets_;
    AtomicCounter total_count_;
    AtomicCounter total_sum_;
    std::atomic<uint64_t> min_{UINT64_MAX};
    std::atomic<uint64_t> max_{0};
};

struct IndexStats {
    AtomicCounter total_docs;
    AtomicCounter total_terms;
    AtomicCounter segment_count;
    AtomicCounter memory_bytes;
    AtomicCounter disk_bytes;
    AtomicCounter wal_bytes;
    AtomicCounter wal_syncs;
    AtomicCounter docs_added;
    AtomicCounter docs_removed;
    AtomicCounter flushes;
    Histogram flush_latency_ms;
    Histogram doc_add_latency_us;

    IndexStats();
};

struct QueryStats {
    AtomicCounter queries;
    Histogram query_latency_ms;
    Histogram segments_queried;
    Histogram docs_scored;
    Histogram posting_bytes_read;

    QueryStats();
};

}  // namespace vortex
