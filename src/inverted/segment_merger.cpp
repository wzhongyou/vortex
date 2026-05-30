#include "vortex/inverted/segment_merger.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "vortex/core/arena.h"
#include "vortex/core/types.h"
#include "vortex/inverted/delete_bitmap.h"
#include "vortex/inverted/forward_index.h"
#include "vortex/inverted/posting_list.h"
#include "vortex/inverted/segment.h"
#include "vortex/inverted/term_dict.h"

namespace vortex {

Result<std::shared_ptr<const Segment>> SegmentMerger::merge(
    const std::string& index_dir,
    const std::vector<std::shared_ptr<const Segment>>& segments,
    uint64_t new_segment_id,
    uint16_t stored_field_count,
    Arena& arena) {

    if (segments.empty()) {
        return Result<std::shared_ptr<const Segment>>::Err(
            Status::InvalidArgument("no segments to merge"));
    }

    // ── Step 1: Calculate doc_id offsets per segment ──
    size_t num_segs = segments.size();
    std::vector<uint32_t> offsets(num_segs, 0);
    uint32_t merged_doc_count = 0;
    uint64_t merged_total_terms = 0;

    for (size_t i = 0; i < num_segs; i++) {
        offsets[i] = merged_doc_count;
        merged_doc_count += segments[i]->doc_count();
        merged_total_terms += static_cast<uint64_t>(
            segments[i]->avgdl() * segments[i]->doc_count());
    }

    double merged_avgdl = merged_doc_count > 0
        ? static_cast<double>(merged_total_terms) / merged_doc_count
        : 0.0;

    // ── Step 2: Collect all unique terms across all segments ──
    std::set<std::string> all_terms;
    for (auto& seg : segments) {
        if (seg->term_dict()) {
            seg->term_dict()->prefix_range("", [&](std::string_view term, const TermInfo&) {
                all_terms.insert(std::string(term));
                return true;
            });
        }
    }

    // ── Step 3: Build merged posting data ──
    TermDictBuilder dict_builder;
    std::vector<uint8_t> merged_posting_data;
    std::string tmp_path = index_dir + "/_tmp_merge_postings";

    for (auto& term : all_terms) {
        PostingListBuilder builder;

        for (size_t i = 0; i < num_segs; i++) {
            const TermInfo* info = segments[i]->lookup_term(term);
            if (!info) continue;

            const uint8_t* data = segments[i]->posting_data() + info->posting_offset;
            PostingListReader reader(data, info->posting_len);
            reader.for_each([&](uint32_t doc_id, uint32_t tf) {
                builder.append(offsets[i] + doc_id, tf);
            });
        }

        int fd = open(tmp_path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fd < 0) {
            return Result<std::shared_ptr<const Segment>>::Err(
                Status::IOError("cannot create temp posting file for merge"));
        }

        auto flush_result = builder.flush(fd, arena);
        if (!flush_result.ok()) {
            close(fd);
            return Result<std::shared_ptr<const Segment>>::Err(flush_result.status());
        }

        auto off_and_len = flush_result.move_value();
        auto byte_count = off_and_len.second;

        std::vector<uint8_t> buf(byte_count);
        lseek(fd, static_cast<off_t>(off_and_len.first), SEEK_SET);
        ssize_t n = read(fd, buf.data(), byte_count);
        close(fd);
        (void)n;

        uint64_t global_offset = merged_posting_data.size();
        merged_posting_data.insert(merged_posting_data.end(), buf.begin(), buf.end());

        TermInfo info;
        info.doc_freq = static_cast<uint32_t>(builder.doc_freq());
        info.posting_offset = global_offset;
        info.posting_len = byte_count;

        dict_builder.insert(term, info);
    }
    unlink(tmp_path.c_str());

    // ── Step 4: Write .fst and .doc ──
    std::string seg_prefix = index_dir + "/_" + std::to_string(new_segment_id);

    {
        std::vector<uint8_t> fst_data = dict_builder.finish();
        int fd = open((seg_prefix + ".fst").c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fd < 0) return Result<std::shared_ptr<const Segment>>::Err(
            Status::IOError("cannot create merged .fst"));
        ssize_t written = write(fd, fst_data.data(), fst_data.size());
        close(fd);
        if (written < 0) return Result<std::shared_ptr<const Segment>>::Err(
            Status::IOError("failed writing merged .fst"));
    }

    {
        int fd = open((seg_prefix + ".doc").c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fd < 0) return Result<std::shared_ptr<const Segment>>::Err(
            Status::IOError("cannot create merged .doc"));
        ssize_t written = write(fd, merged_posting_data.data(), merged_posting_data.size());
        close(fd);
        if (written < 0) return Result<std::shared_ptr<const Segment>>::Err(
            Status::IOError("failed writing merged .doc"));
    }

    // ── Step 5: Merge forward indexes ──
    {
        // Determine field_count from the first non-null forward_index
        uint16_t field_count = 0;
        for (auto& seg : segments) {
            if (seg->forward_index()) {
                field_count = seg->forward_index()->field_count();
                break;
            }
        }

        ForwardIndexBuilder fwd_builder(field_count);

        for (auto& seg : segments) {
            auto* fwd = seg->forward_index();
            if (!fwd) continue;
            for (uint32_t d = 0; d < seg->doc_count(); d++) {
                auto info = fwd->get(d);
                fwd_builder.append(info.doc_length, info.field_lengths);
            }
        }

        int fd = open((seg_prefix + ".fwd").c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fd < 0) return Result<std::shared_ptr<const Segment>>::Err(
            Status::IOError("cannot create merged .fwd"));
        Status s = fwd_builder.flush(fd);
        close(fd);
        if (!s.ok()) return Result<std::shared_ptr<const Segment>>::Err(s);
    }

    // ── Step 6: Merge external ID maps ──
    {
        int fd = open((seg_prefix + ".idm").c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fd < 0) return Result<std::shared_ptr<const Segment>>::Err(
            Status::IOError("cannot create merged .idm"));

        uint32_t count = merged_doc_count;
        write(fd, &count, 4);

        for (auto& seg : segments) {
            // Read the segment's idm raw data (skip header count)
            auto& idm = seg->owned_idm_data_;
            if (idm.size() < 4) continue;
            const uint8_t* p = idm.data() + 4;  // skip doc_count
            size_t remaining = idm.size() - 4;
            while (remaining >= 2) {
                uint16_t slen;
                std::memcpy(&slen, p, 2); p += 2; remaining -= 2;
                if (remaining < slen) break;
                write(fd, p - 2, static_cast<size_t>(slen) + 2);  // len + data
                p += slen; remaining -= slen;
            }
        }
        close(fd);
    }

    // ── Step 7: Merge stored values ──
    {
        int fd = open((seg_prefix + ".store").c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fd < 0) return Result<std::shared_ptr<const Segment>>::Err(
            Status::IOError("cannot create merged .store"));

        uint32_t dc = merged_doc_count;
        uint16_t sfc = stored_field_count;
        write(fd, &dc, 4);
        write(fd, &sfc, 2);

        if (stored_field_count > 0) {
            for (auto& seg : segments) {
                auto& store = seg->owned_store_data_;
                if (store.size() < 6) continue;
                // Skip header: [doc_count:4][sf_count:2]
                const uint8_t* p = store.data() + 6;
                size_t remaining = store.size() - 6;

                for (uint32_t d = 0; d < seg->doc_count() && remaining > 0; d++) {
                    for (uint16_t f = 0; f < stored_field_count && remaining >= 4; f++) {
                        uint32_t vlen;
                        std::memcpy(&vlen, p, 4); p += 4; remaining -= 4;
                        if (remaining < vlen) break;
                        write(fd, p - 4, static_cast<size_t>(vlen) + 4);
                        p += vlen; remaining -= vlen;
                    }
                }
            }
        }
        close(fd);
    }

    // ── Step 8: Write .meta ──
    {
        int fd = open((seg_prefix + ".meta").c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fd < 0) return Result<std::shared_ptr<const Segment>>::Err(
            Status::IOError("cannot create merged .meta"));

        std::ostringstream oss;
        oss << "{\"segment_id\":" << new_segment_id
            << ",\"doc_count\":" << merged_doc_count
            << ",\"total_terms\":" << merged_total_terms
            << ",\"avgdl\":" << merged_avgdl << "}";
        std::string meta = oss.str();
        write(fd, meta.data(), meta.size());
        close(fd);
    }

    // ── Step 9: Load merged segment ──
    auto segment = std::make_shared<Segment>(new_segment_id, merged_doc_count, merged_avgdl);

    auto load_file = [](const std::string& path, std::vector<uint8_t>& out) -> bool {
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) return false;
        struct stat st;
        fstat(fd, &st);
        out.resize(st.st_size);
        ssize_t n = read(fd, out.data(), st.st_size);
        close(fd);
        return n == static_cast<ssize_t>(st.st_size);
    };

    std::vector<uint8_t> fst_buf, doc_buf, fwd_buf, idm_buf, store_buf;

    if (load_file(seg_prefix + ".fst", fst_buf)) {
        segment->owned_fst_data_ = std::move(fst_buf);
        segment->load_dict(segment->owned_fst_data_.data(),
                           segment->owned_fst_data_.size());
    }
    if (load_file(seg_prefix + ".doc", doc_buf)) {
        segment->owned_posting_data_ = std::move(doc_buf);
        segment->load_postings(segment->owned_posting_data_.data(),
                               segment->owned_posting_data_.size());
    }
    if (load_file(seg_prefix + ".fwd", fwd_buf)) {
        segment->owned_fwd_data_ = std::move(fwd_buf);
        segment->load_forward_index(segment->owned_fwd_data_.data(),
                                    segment->owned_fwd_data_.size());
    }
    if (load_file(seg_prefix + ".idm", idm_buf)) {
        segment->load_idm(idm_buf.data(), idm_buf.size());
    }
    if (load_file(seg_prefix + ".store", store_buf)) {
        segment->owned_store_data_ = std::move(store_buf);
        segment->load_store(segment->owned_store_data_.data(),
                            segment->owned_store_data_.size());
    }

    // Init empty delete bitmap
    segment->load_deletes(nullptr, 0);

    return Result<std::shared_ptr<const Segment>>::Ok(std::move(segment));
}

}  // namespace vortex