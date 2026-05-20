#pragma once

#include <cstddef>
#include <cstdint>

namespace vortex::codec {

struct BlockHeader {
    uint8_t num_docs;
    uint8_t doc_bits;
    uint8_t freq_bits;
    uint8_t flags;
};

static constexpr uint8_t kBlockSize = 128;

size_t encode_block(const uint32_t* doc_ids, const uint32_t* freqs,
                    uint8_t num_docs, uint8_t* output);

size_t decode_block(const uint8_t* input, uint32_t* doc_ids_out,
                    uint32_t* freqs_out, uint8_t& num_docs_out);

#ifdef VORTEX_HAS_AVX2
size_t decode_block_avx2(const uint8_t* input, uint32_t* doc_ids_out,
                          uint32_t* freqs_out, uint8_t& num_docs_out);
#endif

using DecodeFunc = size_t (*)(const uint8_t*, uint32_t*, uint32_t*, uint8_t&);
extern DecodeFunc g_decode_block;

void codec_init();

}  // namespace vortex::codec
