#pragma once

#include <string>
#include <string_view>

namespace vortex {

// ASCII lowercase conversion (in-place).
void lowercase_ascii_inplace(std::string& s);

// Full-width to half-width conversion + ASCII lowercase.
// Covers: fullwidth ASCII (FF01-FF5E → 0021-007E), fullwidth space (3000 → 0020),
// and common fullwidth punctuation.
// Returns a new string.
std::string nfkc_normalize(std::string_view input);

}  // namespace vortex
