#include "vortex/inverted/segment.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <sstream>

#include "vortex/core/arena.h"
#include "vortex/inverted/delete_bitmap.h"
#include "vortex/inverted/forward_index.h"
#include "vortex/inverted/posting_list.h"
#include "vortex/inverted/term_dict.h"

namespace vortex {

Segment::Segment(uint64_t id, uint32_t doc_count, double avgdl)
    : id_(id), doc_count_(doc_count), avgdl_(avgdl) {}

const uint8_t* Segment::posting_data() const { return posting_data_; }
size_t Segment::posting_data_len() const { return posting_data_len_; }

Status Segment::load_dict(const uint8_t* data, size_t len) {
    auto r = TermDict::from_memory(data, len);
    if (!r.ok()) return r.status();
    term_dict_ = r.move_value();
    return Status::OK();
}

Status Segment::load_postings(const uint8_t* data, size_t len) {
    posting_data_ = data;
    posting_data_len_ = len;
    return Status::OK();
}

Status Segment::load_forward_index(const uint8_t* data, size_t len) {
    auto r = ForwardIndex::from_memory(data, len);
    if (!r.ok()) return r.status();
    fwd_ = r.move_value();
    return Status::OK();
}

Status Segment::load_deletes(const uint8_t* data, size_t len) {
    deletes_ = std::make_unique<DeleteBitmap>();
    *deletes_ = DeleteBitmap::deserialize(data, len);
    if (deletes_->total() == 0) deletes_->set_total(doc_count_);
    return Status::OK();
}

const TermInfo* Segment::lookup_term(std::string_view term) const {
    if (!term_dict_) return nullptr;
    return term_dict_->lookup(term);
}

// ── MemorySegment ──

MemorySegment::MemorySegment(uint64_t seg_id, uint16_t indexed_field_count)
    : seg_id_(seg_id)
    , indexed_field_count_(indexed_field_count) {}

void MemorySegment::add_term(uint32_t doc_id, std::string_view term, uint32_t tf) {
    auto it = term_builders_.find(std::string(term));
    if (it == term_builders_.end()) {
        auto builder = std::make_unique<PostingListBuilder>();
        builder->append(doc_id, tf);
        term_builders_[std::string(term)] = std::move(builder);
    } else {
        it->second->append(doc_id, tf);
    }
    total_term_occurrences_++;
}

void MemorySegment::add_doc_info(uint32_t doc_length,
                                  const std::vector<uint32_t>& field_lengths) {
    doc_lengths_.push_back(doc_length);
    field_lengths_.push_back(field_lengths);
    doc_count_++;
    total_terms_ += doc_length;
}

Result<std::shared_ptr<const Segment>> MemorySegment::flush(
    const std::string& segment_dir, Arena& arena) {

    if (doc_count_ == 0) {
        return Result<std::shared_ptr<const Segment>>::Err(
            Status::InvalidArgument("no documents to flush"));
    }

    double avgdl = total_terms_ > 0
        ? static_cast<double>(total_terms_) / doc_count_ : 0.0;

    // Collect and sort terms
    std::vector<std::string> sorted_terms;
    for (auto& pair : term_builders_) {
        sorted_terms.push_back(pair.first);
    }
    std::sort(sorted_terms.begin(), sorted_terms.end());

    // Build FST and posting data
    TermDictBuilder dict_builder;
    std::vector<uint8_t> posting_data;

    std::string tmp_path = segment_dir + "/_tmp_postings";

    for (auto& term_str : sorted_terms) {
        auto& builder = term_builders_[term_str];

        int fd = open(tmp_path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fd < 0) {
            return Result<std::shared_ptr<const Segment>>::Err(
                Status::IOError("cannot create temp posting file"));
        }

        auto flush_result = builder->flush(fd, arena);
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

        uint64_t global_offset = posting_data.size();
        posting_data.insert(posting_data.end(), buf.begin(), buf.end());

        TermInfo info;
        info.doc_freq = static_cast<uint32_t>(builder->doc_freq());
        info.posting_offset = global_offset;
        info.posting_len = byte_count;

        dict_builder.insert(term_str, info);
    }
    unlink(tmp_path.c_str());

    // Write files
    std::string seg_prefix = segment_dir + "/_" + std::to_string(seg_id_);

    // .fst
    {
        std::vector<uint8_t> fst_data = dict_builder.finish();
        int fd = open((seg_prefix + ".fst").c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fd < 0) return Result<std::shared_ptr<const Segment>>::Err(
            Status::IOError("cannot create .fst"));
        write(fd, fst_data.data(), fst_data.size());
        close(fd);
    }

    // .doc
    {
        int fd = open((seg_prefix + ".doc").c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fd < 0) return Result<std::shared_ptr<const Segment>>::Err(
            Status::IOError("cannot create .doc"));
        write(fd, posting_data.data(), posting_data.size());
        close(fd);
    }

    // .fwd
    {
        int fd = open((seg_prefix + ".fwd").c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fd < 0) return Result<std::shared_ptr<const Segment>>::Err(
            Status::IOError("cannot create .fwd"));

        uint32_t dc = doc_count_;
        uint16_t fc = indexed_field_count_;
        write(fd, &dc, 4);
        write(fd, &fc, 2);

        for (uint32_t i = 0; i < doc_count_; i++) {
            uint32_t dl = doc_lengths_[i];
            write(fd, &dl, 4);
            for (uint16_t f = 0; f < fc; f++) {
                uint32_t fl = f < field_lengths_[i].size() ? field_lengths_[i][f] : 0;
                write(fd, &fl, 4);
            }
        }
        close(fd);
    }

    // .meta
    {
        int fd = open((seg_prefix + ".meta").c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fd < 0) return Result<std::shared_ptr<const Segment>>::Err(
            Status::IOError("cannot create .meta"));

        std::ostringstream oss;
        oss << "{\"segment_id\":" << seg_id_
            << ",\"doc_count\":" << doc_count_
            << ",\"total_terms\":" << total_terms_
            << ",\"avgdl\":" << avgdl << "}";
        std::string meta = oss.str();
        write(fd, meta.data(), meta.size());
        close(fd);
    }

    // Load segment data into owned vectors so raw pointers remain valid.
    auto segment = std::make_shared<Segment>(seg_id_, doc_count_, avgdl);

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

    std::vector<uint8_t> fst_buf;
    std::vector<uint8_t> doc_buf;
    std::vector<uint8_t> fwd_buf;

    bool have_fst = load_file(seg_prefix + ".fst", fst_buf);
    bool have_doc = load_file(seg_prefix + ".doc", doc_buf);
    bool have_fwd = load_file(seg_prefix + ".fwd", fwd_buf);

    if (have_fst) {
        segment->owned_fst_data_ = std::move(fst_buf);
        segment->load_dict(segment->owned_fst_data_.data(),
                           segment->owned_fst_data_.size());
    }
    if (have_doc) {
        segment->owned_posting_data_ = std::move(doc_buf);
        segment->load_postings(segment->owned_posting_data_.data(),
                               segment->owned_posting_data_.size());
    }
    if (have_fwd) {
        segment->owned_fwd_data_ = std::move(fwd_buf);
        segment->load_forward_index(segment->owned_fwd_data_.data(),
                                    segment->owned_fwd_data_.size());
    }

    // Init empty delete bitmap
    segment->load_deletes(nullptr, 0);

    return Result<std::shared_ptr<const Segment>>::Ok(std::move(segment));
}

}  // namespace vortex
