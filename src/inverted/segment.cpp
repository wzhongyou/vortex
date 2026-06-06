#include "vortex/inverted/segment.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <sstream>
#include <string>

#include "vortex/core/arena.h"
#include "vortex/inverted/delete_bitmap.h"
#include "vortex/inverted/forward_index.h"
#include "vortex/inverted/posting_list.h"
#include "vortex/inverted/term_dict.h"

namespace vortex {

static const std::string empty_string;

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

Status Segment::load_idm(const uint8_t* data, size_t len) {
    owned_idm_data_.assign(data, data + len);

    // Build reverse mapping external_id → doc_id
    ext_id_to_doc_.clear();
    if (len < 4) return Status::OK();
    const uint8_t* p = owned_idm_data_.data();
    uint32_t count;
    std::memcpy(&count, p, 4); p += 4;
    for (uint32_t i = 0; i < count; i++) {
        if (p + 2 > owned_idm_data_.data() + len) break;
        uint16_t slen;
        std::memcpy(&slen, p, 2); p += 2;
        if (p + slen > owned_idm_data_.data() + len) break;
        ext_id_to_doc_[std::string(reinterpret_cast<const char*>(p), slen)] = i;
        p += slen;
    }
    return Status::OK();
}

std::string Segment::resolve_external_id(uint32_t doc_id) const {
    if (owned_idm_data_.empty()) return std::to_string(doc_id);
    const uint8_t* p = owned_idm_data_.data();
    size_t len = owned_idm_data_.size();
    uint32_t count;
    if (len < 4) return std::to_string(doc_id);
    std::memcpy(&count, p, 4);
    if (doc_id >= count) return std::to_string(doc_id);
    p += 4;
    for (uint32_t i = 0; i < doc_id; i++) {
        if (p + 2 > owned_idm_data_.data() + len) return std::to_string(doc_id);
        uint16_t slen;
        std::memcpy(&slen, p, 2);
        p += 2 + slen;
    }
    if (p + 2 > owned_idm_data_.data() + len) return std::to_string(doc_id);
    uint16_t slen;
    std::memcpy(&slen, p, 2);
    if (p + 2 + slen > owned_idm_data_.data() + len) return std::to_string(doc_id);
    return std::string(reinterpret_cast<const char*>(p + 2), slen);
}

const TermInfo* Segment::lookup_term(std::string_view term) const {
    if (!term_dict_) return nullptr;
    return term_dict_->lookup(term);
}

Status Segment::load_store(const uint8_t* data, size_t len) {
    // .store format: [doc_count:u32][stored_field_count:u16]
    //                then per-doc: [value_len:u32][value:char[value_len]] × stored_field_count
    // Data is kept as a blob; get_stored_values() parses it on the fly.
    if (len < 6) return Status::OK();  // empty store
    owned_store_data_.assign(data, data + len);
    return Status::OK();
}

uint32_t Segment::find_doc_id(std::string_view external_id) const {
    auto it = ext_id_to_doc_.find(std::string(external_id));
    return it != ext_id_to_doc_.end() ? it->second : kInvalidDocId;
}

void Segment::get_stored_values(uint32_t doc_id,
                                 std::vector<std::string>& out_values,
                                 uint16_t stored_field_count) const {
    out_values.clear();
    if (owned_store_data_.size() < 6) return;

    const uint8_t* p = owned_store_data_.data();
    uint32_t doc_count;
    std::memcpy(&doc_count, p, 4); p += 4;
    uint16_t sf_count;
    std::memcpy(&sf_count, p, 2); p += 2;
    if (sf_count == 0) return;
    if (doc_id >= doc_count) return;

    // Skip to the right doc
    for (uint32_t i = 0; i < doc_id; i++) {
        for (uint16_t f = 0; f < sf_count; f++) {
            uint32_t vlen;
            std::memcpy(&vlen, p, 4); p += 4;
            p += vlen;
        }
    }

    out_values.reserve(stored_field_count);
    uint16_t n = sf_count < stored_field_count ? sf_count : stored_field_count;
    for (uint16_t f = 0; f < n; f++) {
        uint32_t vlen;
        std::memcpy(&vlen, p, 4); p += 4;
        out_values.emplace_back(reinterpret_cast<const char*>(p), vlen);
        p += vlen;
    }
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

void MemorySegment::add_external_id(std::string_view external_id) {
    external_ids_.emplace_back(external_id);
}

void MemorySegment::add_stored_values(std::vector<std::string> values) {
    stored_values_.push_back(std::move(values));
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
        (void)write(fd, fst_data.data(), fst_data.size());
        close(fd);
    }

    // .doc
    {
        int fd = open((seg_prefix + ".doc").c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fd < 0) return Result<std::shared_ptr<const Segment>>::Err(
            Status::IOError("cannot create .doc"));
        (void)write(fd, posting_data.data(), posting_data.size());
        close(fd);
    }

    // .fwd
    {
        int fd = open((seg_prefix + ".fwd").c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fd < 0) return Result<std::shared_ptr<const Segment>>::Err(
            Status::IOError("cannot create .fwd"));

        uint32_t dc = doc_count_;
        uint16_t fc = indexed_field_count_;
        (void)write(fd, &dc, 4);
        (void)write(fd, &fc, 2);

        for (uint32_t i = 0; i < doc_count_; i++) {
            uint32_t dl = doc_lengths_[i];
            (void)write(fd, &dl, 4);
            for (uint16_t f = 0; f < fc; f++) {
                uint32_t fl = f < field_lengths_[i].size() ? field_lengths_[i][f] : 0;
                (void)write(fd, &fl, 4);
            }
        }
        close(fd);
    }

    // .idm
    {
        int fd = open((seg_prefix + ".idm").c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fd < 0) return Result<std::shared_ptr<const Segment>>::Err(
            Status::IOError("cannot create .idm"));

        uint32_t count = static_cast<uint32_t>(external_ids_.size());
        (void)write(fd, &count, 4);
        for (auto& ext_id : external_ids_) {
            uint16_t slen = static_cast<uint16_t>(ext_id.size());
            (void)write(fd, &slen, 2);
            (void)write(fd, ext_id.data(), slen);
        }
        close(fd);
    }

    // .store
    {
        int fd = open((seg_prefix + ".store").c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fd < 0) return Result<std::shared_ptr<const Segment>>::Err(
            Status::IOError("cannot create .store"));

        uint16_t sf_count = stored_values_.empty() ? 0
            : static_cast<uint16_t>(stored_values_[0].size());
        uint32_t dc = doc_count_;
        (void)write(fd, &dc, 4);
        (void)write(fd, &sf_count, 2);
        for (uint32_t i = 0; i < doc_count_; i++) {
            for (uint16_t f = 0; f < sf_count; f++) {
                const std::string& v = (i < stored_values_.size() && f < stored_values_[i].size())
                    ? stored_values_[i][f] : empty_string;
                uint32_t vlen = static_cast<uint32_t>(v.size());
                (void)write(fd, &vlen, 4);
                if (vlen > 0) (void)write(fd, v.data(), vlen);
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
        (void)write(fd, meta.data(), meta.size());
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
    std::vector<uint8_t> idm_buf;
    std::vector<uint8_t> store_buf;

    bool have_fst = load_file(seg_prefix + ".fst", fst_buf);
    bool have_doc = load_file(seg_prefix + ".doc", doc_buf);
    bool have_fwd = load_file(seg_prefix + ".fwd", fwd_buf);
    bool have_idm = load_file(seg_prefix + ".idm", idm_buf);
    bool have_store = load_file(seg_prefix + ".store", store_buf);

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
    if (have_idm) {
        segment->load_idm(idm_buf.data(), idm_buf.size());
    }
    if (have_store) {
        segment->owned_store_data_ = std::move(store_buf);
        segment->load_store(segment->owned_store_data_.data(),
                            segment->owned_store_data_.size());
    }

    // Init empty delete bitmap
    segment->load_deletes(nullptr, 0);

    return Result<std::shared_ptr<const Segment>>::Ok(std::move(segment));
}

}  // namespace vortex
