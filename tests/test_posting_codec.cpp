#include <cstring>
#include <vector>

#include "gtest/gtest.h"
#include "vortex/inverted/posting_codec.h"

namespace vortex {
namespace {

// Helper: build a test block with known doc IDs and freqs
void build_test_block(uint32_t* doc_ids, uint32_t* freqs, uint8_t num_docs) {
    // Monotonic doc IDs, semi-random freqs
    doc_ids[0] = 7;
    freqs[0] = 3;
    for (uint8_t i = 1; i < num_docs; i++) {
        doc_ids[i] = doc_ids[i - 1] + (i * 11) % 127 + 1;
        freqs[i] = (i * 7 + 3) % 15 + 1;
    }
}

TEST(PostingCodecTest, EncodeDecodeRoundtripFull) {
    uint32_t doc_ids[128];
    uint32_t freqs[128];
    build_test_block(doc_ids, freqs, 128);

    uint8_t output[2048];
    size_t encoded = codec::encode_block(doc_ids, freqs, 128, output);
    EXPECT_GT(encoded, 4u);
    EXPECT_LT(encoded, sizeof(output));

    uint32_t decoded_docs[128];
    uint32_t decoded_freqs[128];
    uint8_t num_decoded = 0;
    // Use the scalar path explicitly
    size_t consumed = codec::decode_block(output, decoded_docs, decoded_freqs, num_decoded);
    EXPECT_EQ(consumed, encoded);
    EXPECT_EQ(num_decoded, 128);

    for (int i = 0; i < 128; i++) {
        EXPECT_EQ(decoded_docs[i], doc_ids[i]) << "doc mismatch at " << i;
        EXPECT_EQ(decoded_freqs[i], freqs[i]) << "freq mismatch at " << i;
    }
}

TEST(PostingCodecTest, DecodeViaFunctionPointer) {
    codec::codec_init();  // ensure dispatch is set

    uint32_t doc_ids[128];
    uint32_t freqs[128];
    build_test_block(doc_ids, freqs, 128);

    uint8_t output[2048];
    size_t encoded = codec::encode_block(doc_ids, freqs, 128, output);

    uint32_t decoded_docs[128];
    uint32_t decoded_freqs[128];
    uint8_t num_decoded = 0;
    size_t consumed = codec::g_decode_block(output, decoded_docs, decoded_freqs, num_decoded);
    EXPECT_EQ(consumed, encoded);
    EXPECT_EQ(num_decoded, 128);
    for (int i = 0; i < 128; i++) {
        EXPECT_EQ(decoded_docs[i], doc_ids[i]);
        EXPECT_EQ(decoded_freqs[i], freqs[i]);
    }
}

TEST(PostingCodecTest, EncodeDecodePartialBlock) {
    uint32_t doc_ids[64];
    uint32_t freqs[64];
    doc_ids[0] = 1;
    freqs[0] = 1;
    for (int i = 1; i < 64; i++) {
        doc_ids[i] = doc_ids[i - 1] + 2;
        freqs[i] = 1;
    }

    uint8_t output[1024];
    size_t encoded = codec::encode_block(doc_ids, freqs, 64, output);

    uint32_t decoded_docs[128];
    uint32_t decoded_freqs[128];
    uint8_t num_decoded = 0;
    codec::decode_block(output, decoded_docs, decoded_freqs, num_decoded);
    EXPECT_EQ(num_decoded, 64);
    for (int i = 0; i < 64; i++) {
        EXPECT_EQ(decoded_docs[i], doc_ids[i]);
        EXPECT_EQ(decoded_freqs[i], freqs[i]);
    }
}

TEST(PostingCodecTest, EmptyBlock) {
    uint8_t output[16];
    size_t encoded = codec::encode_block(nullptr, nullptr, 0, output);
    EXPECT_EQ(encoded, 4u);

    uint32_t decoded_docs[1];
    uint32_t decoded_freqs[1];
    uint8_t num_decoded = 0;
    size_t consumed = codec::decode_block(output, decoded_docs, decoded_freqs, num_decoded);
    EXPECT_EQ(consumed, 4u);
    EXPECT_EQ(num_decoded, 0);
}

TEST(PostingCodecTest, SingleDocBlock) {
    uint32_t doc_ids[1] = {42};
    uint32_t freqs[1] = {5};
    uint8_t output[64];

    size_t encoded = codec::encode_block(doc_ids, freqs, 1, output);
    uint32_t decoded_docs[1];
    uint32_t decoded_freqs[1];
    uint8_t num_decoded = 0;
    codec::decode_block(output, decoded_docs, decoded_freqs, num_decoded);
    EXPECT_EQ(num_decoded, 1);
    EXPECT_EQ(decoded_docs[0], 42u);
    EXPECT_EQ(decoded_freqs[0], 5u);
}

TEST(PostingCodecTest, LargeGapsBlock) {
    uint32_t doc_ids[128];
    uint32_t freqs[128];
    doc_ids[0] = 1000000;
    freqs[0] = 1;
    for (int i = 1; i < 128; i++) {
        doc_ids[i] = doc_ids[i - 1] + 50000 + i;
        freqs[i] = (i % 10) + 1;
    }

    uint8_t output[4096];
    size_t encoded = codec::encode_block(doc_ids, freqs, 128, output);

    uint32_t decoded_docs[128];
    uint32_t decoded_freqs[128];
    uint8_t num_decoded = 0;
    codec::decode_block(output, decoded_docs, decoded_freqs, num_decoded);
    EXPECT_EQ(num_decoded, 128);
    for (int i = 0; i < 128; i++) {
        EXPECT_EQ(decoded_docs[i], doc_ids[i]);
        EXPECT_EQ(decoded_freqs[i], freqs[i]);
    }
}

}  // namespace
}  // namespace vortex