#pragma once

#include <string>
#include <vector>

namespace vortex {

struct FieldValue {
    std::string name;
    std::string value;
};

struct Document {
    std::vector<FieldValue> fields;
};

}  // namespace vortex
