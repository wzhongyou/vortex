#include "vortex/core/schema.h"

namespace vortex {

Status Schema::add_field(FieldSchema field) {
    if (field.name.empty()) {
        return Status::InvalidArgument("field name must not be empty");
    }
    if (name_to_index_.count(field.name)) {
        return Status::InvalidArgument("duplicate field name: " + field.name);
    }

    FieldIdx idx = static_cast<FieldIdx>(fields.size());
    name_to_index_[field.name] = idx;

    if (field.stored) stored_count_++;
    if (field.indexed && field.type == FieldType::TEXT) indexed_count_++;

    fields.push_back(std::move(field));
    return Status::OK();
}

const FieldSchema* Schema::field(std::string_view name) const {
    auto it = name_to_index_.find(std::string(name));
    if (it == name_to_index_.end()) return nullptr;
    return &fields[it->second];
}

FieldIdx Schema::field_index(std::string_view name) const {
    auto it = name_to_index_.find(std::string(name));
    if (it == name_to_index_.end()) return kInvalidFieldIdx;
    return it->second;
}

}  // namespace vortex
