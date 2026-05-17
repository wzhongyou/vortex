#pragma once

#include <cstdint>

namespace vortex {

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i32 = int32_t;
using i64 = int64_t;

using DocId    = u32;
using SegId    = u64;
using FieldIdx = u16;
using TermFreq = u32;

constexpr DocId    kInvalidDocId    = UINT32_MAX;
constexpr FieldIdx kInvalidFieldIdx = UINT16_MAX;

}  // namespace vortex
