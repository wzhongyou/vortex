#include "vortex/inverted/posting_codec.h"

#include <algorithm>
#include <cstring>

namespace vortex::codec {

DecodeFunc g_decode_block = decode_block;

void codec_init() {
    // On x86-64 with AVX2, switch to AVX2 implementation.
    // On ARM64, scalar path is the default (NEON path is future work).
    g_decode_block = decode_block;
}

size_t encode_block(const uint32_t* doc_ids, const uint32_t* freqs,
                    uint8_t num_docs, uint8_t* output) {
    if (num_docs == 0) {
        BlockHeader hdr{0, 0, 0, 0};
        std::memcpy(output, &hdr, 4);
        return 4;
    }

    // Compute deltas
    uint32_t deltas[128];
    deltas[0] = doc_ids[0];
    uint32_t max_delta = deltas[0];
    uint32_t max_freq = freqs[0];
    for (uint8_t i = 1; i < num_docs; i++) {
        deltas[i] = doc_ids[i] - doc_ids[i - 1];
        max_delta = std::max(max_delta, deltas[i]);
        max_freq = std::max(max_freq, freqs[i]);
    }

    // Bit widths (ceil log2)
    uint8_t doc_bits = 0;
    while ((1ULL << doc_bits) <= max_delta) doc_bits++;

    uint8_t freq_bits = 0;
    while ((1ULL << freq_bits) <= max_freq) freq_bits++;

    // Header
    BlockHeader hdr{num_docs, doc_bits, freq_bits, 0};
    std::memcpy(output, &hdr, 4);
    size_t offset = 4;

    // Pack deltas bit-by-bit
    uint8_t bit_buf = 0;
    int bit_pos = 0;
    auto write_bits = [&](uint32_t value, uint8_t bits) {
        for (uint8_t b = 0; b < bits; b++) {
            if (value & (1U << b)) bit_buf |= (1U << bit_pos);
            bit_pos++;
            if (bit_pos == 8) {
                output[offset++] = bit_buf;
                bit_buf = 0;
                bit_pos = 0;
            }
        }
    };

    for (uint8_t i = 0; i < num_docs; i++) {
        write_bits(deltas[i], doc_bits);
    }
    for (uint8_t i = 0; i < num_docs; i++) {
        write_bits(freqs[i], freq_bits);
    }

    // Flush remaining bits
    if (bit_pos > 0) {
        output[offset++] = bit_buf;
    }

    return offset;
}

size_t decode_block(const uint8_t* input, uint32_t* doc_ids_out,
                    uint32_t* freqs_out, uint8_t& num_docs_out) {
    BlockHeader hdr;
    std::memcpy(&hdr, input, 4);
    num_docs_out = hdr.num_docs;

    if (hdr.num_docs == 0) return 4;

    size_t offset = 4;
    int bit_pos = 0;
    uint8_t byte = input[offset];

    auto read_bits = [&](uint8_t bits) -> uint32_t {
        uint32_t val = 0;
        for (uint8_t b = 0; b < bits; b++) {
            if (bit_pos == 8) {
                bit_pos = 0;
                byte = input[++offset];
            }
            if (byte & (1U << bit_pos)) val |= (1U << b);
            bit_pos++;
        }
        return val;
    };

    // Decode deltas
    
    for (uint8_t i = 0; i < hdr.num_docs; i++) {
        uint32_t delta = read_bits(hdr.doc_bits);
        if (i == 0) {
            doc_ids_out[i] = delta;
        } else {
            doc_ids_out[i] = doc_ids_out[i - 1] + delta;
        }
    }

    // Decode freqs
    for (uint8_t i = 0; i < hdr.num_docs; i++) {
        freqs_out[i] = read_bits(hdr.freq_bits);
    }

    // Advance past any remaining partial byte
    if (bit_pos > 0) offset++;

    return offset;
}

}  // namespace vortex::codec
