#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "vortex/core/document.h"
#include "vortex/core/result.h"
#include "vortex/core/schema.h"
#include "vortex/core/status.h"
#include "vortex/core/stats.h"

namespace vortex {

class Arena;
class IndexReader;
struct MemorySegment;
class ExternalIdMap;
class WAL;
struct SegmentList;
class Analyzer;

struct IndexWriterOptions {
    std::string index_dir;
    Schema schema;
    uint32_t ram_buffer_mb = 64;
    std::string external_id_field = "doc_id";
};

class IndexWriter {
public:
    static Result<std::unique_ptr<IndexWriter>> open(IndexWriterOptions opts);
    ~IndexWriter();

    Status add_document(const Document& doc);
    Status remove_document(std::string_view external_id);
    Status flush();

    Result<std::shared_ptr<IndexReader>> get_reader();

    const IndexStats& stats() const { return stats_; }

private:
    IndexWriter(IndexWriterOptions opts);

    IndexWriterOptions opts_;
    std::mutex write_mutex_;
    std::unique_ptr<Arena> active_arena_;
    std::unique_ptr<Analyzer> analyzer_;
    std::unique_ptr<MemorySegment> active_segment_;
    std::unique_ptr<ExternalIdMap> id_map_;
    std::unique_ptr<WAL> wal_;
    std::unique_ptr<SegmentList> segment_list_;
    uint32_t next_internal_id_ = 0;
    uint64_t next_segment_id_ = 0;
    IndexStats stats_;
};

}  // namespace vortex
