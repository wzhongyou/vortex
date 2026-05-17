#include "vortex/inverted/index_writer.h"

#include <sys/stat.h>

#include "vortex/core/arena.h"
#include "vortex/inverted/analyzer.h"
#include "vortex/inverted/external_id_map.h"
#include "vortex/inverted/filter.h"
#include "vortex/inverted/index_reader.h"
#include "vortex/inverted/segment.h"
#include "vortex/inverted/segment_list.h"
#include "vortex/inverted/tokenizer.h"
#include "vortex/inverted/wal.h"

namespace vortex {

Result<std::unique_ptr<IndexWriter>> IndexWriter::open(IndexWriterOptions opts) {
    if (opts.index_dir.empty()) {
        return Result<std::unique_ptr<IndexWriter>>::Err(
            Status::InvalidArgument("index_dir must not be empty"));
    }
    if (opts.schema.stored_field_count() == 0 && opts.schema.indexed_field_count() == 0) {
        return Result<std::unique_ptr<IndexWriter>>::Err(
            Status::InvalidArgument("schema has no indexed or stored fields"));
    }

    // Create index directory
    mkdir(opts.index_dir.c_str(), 0755);

    auto writer = std::unique_ptr<IndexWriter>(new IndexWriter(std::move(opts)));
    return Result<std::unique_ptr<IndexWriter>>::Ok(std::move(writer));
}

IndexWriter::IndexWriter(IndexWriterOptions opts)
    : opts_(std::move(opts))
    , active_arena_(std::make_unique<Arena>())
    , id_map_(std::make_unique<ExternalIdMap>())
    , wal_(std::make_unique<WAL>(opts_.index_dir + "/wal.log"))
    , segment_list_(std::make_unique<SegmentList>()) {

    // Build analyzer
    auto tokenizer = std::make_unique<MixedTokenizer>();
    std::vector<std::unique_ptr<TokenFilter>> filters;
    filters.push_back(std::make_unique<LowercaseFilter>());
    analyzer_ = std::make_unique<Analyzer>(std::move(tokenizer), std::move(filters));

    // Create first memory segment
    active_segment_ = std::make_unique<MemorySegment>(
        next_segment_id_++, opts_.schema.indexed_field_count());
}

IndexWriter::~IndexWriter() {
    flush();
}

Status IndexWriter::add_document(const Document& doc) {
    std::lock_guard<std::mutex> lock(write_mutex_);

    // Find external ID
    std::string external_id;
    for (auto& f : doc.fields) {
        if (f.name == opts_.external_id_field) {
            external_id = f.value;
            break;
        }
    }
    if (external_id.empty()) {
        return Status::InvalidArgument("document missing external_id field: " +
                                        opts_.external_id_field);
    }

    // Check duplicate
    if (id_map_->find(external_id)) {
        return Status::InvalidArgument("duplicate external_id: " + external_id);
    }

    uint32_t internal_id = next_internal_id_++;

    // WAL
    Status wal_status = wal_->append_add(internal_id, external_id, doc);
    if (!wal_status.ok()) return wal_status;

    // External ID mapping
    id_map_->insert(external_id, active_segment_->segment_id(), internal_id);

    // Tokenize indexed TEXT fields, aggregating term frequencies per document.
    uint32_t total_doc_length = 0;
    std::vector<uint32_t> field_lengths(opts_.schema.indexed_field_count(), 0);
    std::unordered_map<std::string, uint32_t> doc_term_freq;

    for (auto& fv : doc.fields) {
        auto* fs = opts_.schema.field(fv.name);
        if (!fs) continue;

        if (fs->indexed && fs->type == FieldType::TEXT) {
            FieldIdx fidx = opts_.schema.field_index(fv.name);
            if (fidx == kInvalidFieldIdx) continue;

            // Map to indexed field position
            uint16_t indexed_pos = 0;
            for (uint16_t i = 0; i < fidx; i++) {
                if (opts_.schema.fields[i].indexed &&
                    opts_.schema.fields[i].type == FieldType::TEXT) {
                    indexed_pos++;
                }
            }

            ScopedThreadArena scoped_arena(*active_arena_);

            // Analyze text
            std::vector<Analyzer::TermWithFreq> terms;
            analyzer_->analyze(fv.value, terms, *active_arena_);

            for (auto& twf : terms) {
                doc_term_freq[std::string(twf.term)] += twf.tf;
                field_lengths[indexed_pos] += twf.tf;
                total_doc_length += twf.tf;
            }
        }
    }

    // Add aggregated terms to the memory segment
    for (auto& pair : doc_term_freq) {
        active_segment_->add_term(internal_id, pair.first, pair.second);
    }

    active_segment_->add_doc_info(total_doc_length, field_lengths);

    // Check flush threshold
    if (active_arena_->allocated() >= opts_.ram_buffer_mb * 1024 * 1024) {
        Status fs = flush();
        if (!fs.ok()) return fs;
    }

    stats_.docs_added.inc();
    return Status::OK();
}

Status IndexWriter::remove_document(std::string_view external_id) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    wal_->append_remove(external_id);
    const auto* loc = id_map_->find(external_id);
    if (loc) {
        // Mark as deleted in the appropriate segment's delete bitmap
        // (V1: not fully implemented — requires tracking which segment the doc is in)
        id_map_->remove(external_id);
    }
    stats_.docs_removed.inc();
    return Status::OK();
}

Status IndexWriter::flush() {
    std::lock_guard<std::mutex> lock(write_mutex_);

    if (active_segment_->doc_count() == 0) return Status::OK();

    // Flush memory segment to disk
    auto result = active_segment_->flush(opts_.index_dir, *active_arena_);
    if (!result.ok()) return result.status();

    auto segment = result.move_value();

    // Register in segment list
    segment_list_->publish_segment(segment);

    // Sync and truncate WAL
    wal_->sync();
    wal_->truncate();

    // Reset for next segment
    active_arena_->reset();
    active_segment_ = std::make_unique<MemorySegment>(
        next_segment_id_++, opts_.schema.indexed_field_count());
    next_internal_id_ = 0;

    stats_.flushes.inc();
    return Status::OK();
}

Result<std::shared_ptr<IndexReader>> IndexWriter::get_reader() {
    auto* snap = segment_list_->acquire_snapshot();
    auto reader = std::shared_ptr<IndexReader>(new IndexReader(const_cast<SegmentsSnapshot*>(snap), segment_list_.get()));
    return Result<std::shared_ptr<IndexReader>>::Ok(std::move(reader));
}

}  // namespace vortex
