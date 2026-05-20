#if defined(__x86_64__) || defined(_M_X64)

#include "vortex/inverted/posting_codec.h"

#include <cstring>

#include <immintrin.h>

namespace vortex::codec {

namespace {

// Decode 8 uint32_t values from bit-plane representation using AVX2.
// byte_idx: which group of 8 elements (0..15)
// planes: doc_bits bytes, each byte has bit p for 8 consecutive elements
//        (bit 0 of byte = element byte_idx*8 + 0, etc.)
// out: output array for 8 decoded deltas
inline void decode_group_avx2(const uint8_t* planes, uint8_t bits,
                               uint32_t* out, const __m256i& lane_shifts) {
    __m256i result = _mm256_setzero_si256();

    for (uint8_t p = 0; p < bits; p++) {
        // Broadcast byte at plane p to all 8 lanes
        __m256i byte_val = _mm256_set1_epi32(static_cast<int>(planes[p]));

        // (byte_val >> lane) & 1  — extract bit j for lane j
        __m256i shifted = _mm256_srlv_epi32(byte_val, lane_shifts);
        __m256i bit = _mm256_and_si256(shifted, _mm256_set1_epi32(1));

        // Shift to position p and accumulate
        __m256i bit_at_p = _mm256_slli_epi32(bit, p);
        result = _mm256_or_si256(result, bit_at_p);
    }

    _mm256_storeu_si256(reinterpret_cast<__m256i*>(out), result);
}

// Prefix-sum: convert deltas to absolute doc IDs
inline void prefix_sum_avx2(uint32_t* deltas, uint8_t num) {
    for (uint8_t i = 1; i < num; i++) {
        deltas[i] += deltas[i - 1];
    }
}

} // anonymous namespace

size_t decode_block_avx2(const uint8_t* input, uint32_t* doc_ids_out,
                          uint32_t* freqs_out, uint8_t& num_docs_out) {
    BlockHeader hdr;
    std::memcpy(&hdr, input, 4);
    num_docs_out = hdr.num_docs;

    if (hdr.num_docs == 0) return 4;

    const uint8_t doc_bits = hdr.doc_bits;
    const uint8_t freq_bits = hdr.freq_bits;

    // Bit-plane data starts after 4-byte header
    const uint8_t* planes = input + 4;
    constexpr uint8_t kGroupSize = 8;
    constexpr int kGroupsPerBlock = kBlockSize / kGroupSize; // 16

    // Setup: per-lane shift amounts for bit extraction
    const __m256i lane_shifts = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);

    // Decode doc ID deltas: 16 groups of 8 elements
    for (int g = 0; g < kGroupsPerBlock; g++) {
        const uint8_t* plane_data = planes + g;  // byte g of each plane
        decode_group_avx2(plane_data, doc_bits, doc_ids_out + g * kGroupSize, lane_shifts);
    }

    // Convert deltas to absolute doc IDs
    prefix_sum_avx2(doc_ids_out, hdr.num_docs);

    // Decode freqs (same bit-plane layout)
    const uint8_t* freq_plane_start = planes + doc_bits * kGroupSize;
    for (int g = 0; g < kGroupsPerBlock; g++) {
        const uint8_t* plane_data = freq_plane_start + g;
        decode_group_avx2(plane_data, freq_bits, freqs_out + g * kGroupSize, lane_shifts);
    }

    // Total bytes consumed: header + doc_planes + freq_planes
    size_t total_bytes = 4 + doc_bits * kGroupSize + freq_bits * kGroupSize;
    return total_bytes;
}

}  // namespace vortex::codec

#endif  // __x86_64__ || _M_X64