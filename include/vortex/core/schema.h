#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "vortex/core/status.h"
#include "vortex/core/types.h"

namespace vortex {

enum class FieldType : uint8_t {
    TEXT,
    KEYWORD,
};

struct FieldSchema {
    std::string name;
    FieldType type;
    bool stored;
    bool indexed;
};

class Schema {
public:
    Status add_field(FieldSchema field);

    const FieldSchema* field(std::string_view name) const;
    FieldIdx field_index(std::string_view name) const;

    std::vector<FieldSchema> fields;

    uint16_t stored_field_count() const { return stored_count_; }
    uint16_t indexed_field_count() const { return indexed_count_; }

private:
    using FieldMap = std::unordered_map<std::string, FieldIdx>;

    FieldMap name_to_index_;
    uint16_t stored_count_ = 0;
    uint16_t indexed_count_ = 0;
};

}  // namespace vortex
