#include <cstdint>
#include <cstring>
#include <vector>

#include <benchmark/benchmark.h>

#include "vortex/inverted/posting_codec.h"

using namespace vortex;

// Generate a realistic block of 128 doc IDs and freqs
static void generate_block(uint32_t* doc_ids, uint32_t* freqs) {
    doc_ids[0] = 1;
    freqs[0] = 1;
    for (int i = 1; i < 128; i++) {
        doc_ids[i] = doc_ids[i - 1] + (i % 10) + 1;
        freqs[i] = (i % 5) + 1;
    }
}

static void BM_EncodeBlock(benchmark::State& state) {
    uint32_t doc_ids[128], freqs[128];
    uint8_t output[4096];
    generate_block(doc_ids, freqs);

    for (auto _ : state) {
        benchmark::DoNotOptimize(
            codec::encode_block(doc_ids, freqs, 128, output));
    }
}
BENCHMARK(BM_EncodeBlock);

static void BM_DecodeBlock(benchmark::State& state) {
    uint32_t doc_ids[128], freqs[128];
    uint8_t input[4096];
    generate_block(doc_ids, freqs);
    size_t encoded = codec::encode_block(doc_ids, freqs, 128, input);

    uint32_t out_docs[128], out_freqs[128];
    uint8_t num_docs;

    for (auto _ : state) {
        benchmark::DoNotOptimize(
            codec::decode_block(input, out_docs, out_freqs, num_docs));
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_DecodeBlock);

static void BM_DecodeViaDispatch(benchmark::State& state) {
    codec::codec_init();

    uint32_t doc_ids[128], freqs[128];
    uint8_t input[4096];
    generate_block(doc_ids, freqs);
    size_t encoded = codec::encode_block(doc_ids, freqs, 128, input);

    uint32_t out_docs[128], out_freqs[128];
    uint8_t num_docs;

    for (auto _ : state) {
        benchmark::DoNotOptimize(
            codec::g_decode_block(input, out_docs, out_freqs, num_docs));
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_DecodeViaDispatch);

static void BM_EncodeDecodeRoundtrip(benchmark::State& state) {
    uint32_t doc_ids[128], freqs[128];
    uint8_t buffer[4096];
    uint32_t out_docs[128], out_freqs[128];
    uint8_t num_docs;
    generate_block(doc_ids, freqs);

    for (auto _ : state) {
        size_t n = codec::encode_block(doc_ids, freqs, 128, buffer);
        size_t m = codec::decode_block(buffer, out_docs, out_freqs, num_docs);
        benchmark::DoNotOptimize(n + m);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_EncodeDecodeRoundtrip);

BENCHMARK_MAIN();