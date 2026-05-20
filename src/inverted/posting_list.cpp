#include "vortex/inverted/posting_list.h"

#include <unistd.h>

#include <cstring>

#include "vortex/core/arena.h"
#include "vortex/inverted/posting_codec.h"

namespace vortex {

void PostingListBuilder::append(uint32_t doc_id, uint32_t term_freq) {
    doc_ids_.push_back(doc_id);
    freqs_.push_back(term_freq);
    total_docs_++;
}

Result<std::pair<uint64_t, uint32_t>> PostingListBuilder::flush(
    int fd, Arena& scratch) {

    size_t num_docs = doc_ids_.size();
    if (num_docs == 0) {
        return Result<std::pair<uint64_t, uint32_t>>::Ok({0, 0});
    }

    // Get current file position
    off_t start_off = lseek(fd, 0, SEEK_CUR);
    if (start_off < 0) {
        return Result<std::pair<uint64_t, uint32_t>>::Err(
            Status::IOError("lseek failed"));
    }

    // Encode blocks
    std::vector<uint64_t> block_offsets;
    std::vector<uint32_t> block_max_doc;

    uint8_t encode_buf[4 + 128 * 8];  // max block size
    std::vector<uint8_t> all_data;

    size_t pos = 0;
    while (pos < num_docs) {
        uint8_t n = static_cast<uint8_t>(std::min(num_docs - pos, size_t(128)));
        size_t bytes = codec::encode_block(
            doc_ids_.data() + pos, freqs_.data() + pos, n, encode_buf);

        block_offsets.push_back(all_data.size());
        block_max_doc.push_back(doc_ids_[pos + n - 1]);
        all_data.insert(all_data.end(), encode_buf, encode_buf + bytes);
        pos += n;
    }

    // Sentinel: zero header marks end of blocks for reader
    all_data.insert(all_data.end(), 4, 0);

    // Build skip list
    SkipList sl;
    sl.build(block_offsets, block_max_doc, 4);

    // Serialize skip list and append after block data
    std::vector<uint8_t> skip_data = sl.serialize();
    all_data.insert(all_data.end(), skip_data.begin(), skip_data.end());

    // Write to file
    ssize_t written = write(fd, all_data.data(), all_data.size());
    if (written < 0 || static_cast<size_t>(written) != all_data.size()) {
        return Result<std::pair<uint64_t, uint32_t>>::Err(
            Status::IOError("write failed"));
    }

    uint64_t offset = static_cast<uint64_t>(start_off);
    uint32_t byte_count = static_cast<uint32_t>(all_data.size());

    return Result<std::pair<uint64_t, uint32_t>>::Ok({offset, byte_count});
}

PostingListReader::PostingListReader() = default;

PostingListReader::PostingListReader(const uint8_t* data, size_t len)
    : data_(data), data_len_(len) {
    if (len < 4) return;

    const uint8_t* p = data_;
    const uint8_t* end = data_ + len;
    while (p + 4 <= end) {
        codec::BlockHeader hdr;
        std::memcpy(&hdr, p, 4);
        if (hdr.num_docs == 0) break;

        size_t db = (static_cast<size_t>(hdr.num_docs) * hdr.doc_bits + 7) / 8;
        size_t fb = (static_cast<size_t>(hdr.num_docs) * hdr.freq_bits + 7) / 8;
        size_t block_bytes = 4 + db + fb;

        if (p + block_bytes > end) break;
        total_docs_ += hdr.num_docs;
        num_blocks_++;
        p += block_bytes;
    }

    if (p < end) {
        // Skip past the 4-byte zero sentinel
        if (p + 4 <= end) p += 4;
        size_t skip_len = end - p;
        skip_list_ = SkipList::deserialize(p, skip_len);
    }
}

void PostingListReader::load_block(uint32_t block_idx) const {
    if (block_idx >= num_blocks_) {
        block_count_ = 0;
        return;
    }

    const uint8_t* p = data_;
    for (uint32_t b = 0; b < block_idx; b++) {
        codec::BlockHeader hdr;
        std::memcpy(&hdr, p, 4);
        size_t db = (static_cast<size_t>(hdr.num_docs) * hdr.doc_bits + 7) / 8;
        size_t fb = (static_cast<size_t>(hdr.num_docs) * hdr.freq_bits + 7) / 8;
        p += 4 + db + fb;
    }

    codec::decode_block(p, block_docs_, block_freqs_, block_count_);
    block_pos_ = 0;
    current_block_ = block_idx;
}

bool PostingListReader::advance_to(uint32_t target, uint32_t& doc_id,
                                    uint32_t& tf) const {
    if (!skip_list_.empty() && current_block_ < num_blocks_) {
        uint64_t target_off = skip_list_.advance(target, 0);
        const uint8_t* p = data_;
        for (uint32_t b = 0; b < num_blocks_; b++) {
            if (static_cast<uint64_t>(p - data_) >= target_off) {
                current_block_ = b;
                block_count_ = 0;
                block_pos_ = 0;
                break;
            }
            codec::BlockHeader hdr;
            std::memcpy(&hdr, p, 4);
            size_t db = (static_cast<size_t>(hdr.num_docs) * hdr.doc_bits + 7) / 8;
            size_t fb = (static_cast<size_t>(hdr.num_docs) * hdr.freq_bits + 7) / 8;
            p += 4 + db + fb;
        }
    }

    while (current_block_ < num_blocks_) {
        if (block_count_ == 0) load_block(current_block_);

        while (block_pos_ < block_count_) {
            uint32_t d = block_docs_[block_pos_];
            if (d >= target) {
                doc_id = d;
                tf = block_freqs_[block_pos_];
                return true;
            }
            block_pos_++;
        }
        current_block_++;
        block_count_ = 0;
    }

    return false;
}

}  // namespace vortex
