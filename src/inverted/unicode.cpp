#include "vortex/inverted/unicode.h"

#include <cstdint>

namespace vortex {

void lowercase_ascii_inplace(std::string& s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + 32);
        }
    }
}

namespace {

// Fullwidth character ranges and their halfwidth offsets.
struct FullwidthRange {
    char32_t start;
    char32_t end;
    char32_t offset;  // subtract from fullwidth codepoint to get halfwidth
};

constexpr FullwidthRange kFullwidthRanges[] = {
    {0x3000, 0x3000, 0x3000 - 0x0020},  // ideographic space → space
    {0xFF01, 0xFF5E, 0xFF01 - 0x0021},  // fullwidth !-~ → halfwidth
};

// Decode one UTF-8 codepoint. Returns bytes consumed (1-4), 0 on error.
size_t decode_utf8(const char* s, char32_t& cp) {
    auto u = reinterpret_cast<const uint8_t*>(s);
    if ((u[0] & 0x80) == 0) {
        cp = u[0];
        return 1;
    }
    if ((u[0] & 0xE0) == 0xC0 && (u[1] & 0xC0) == 0x80) {
        cp = ((u[0] & 0x1F) << 6) | (u[1] & 0x3F);
        return 2;
    }
    if ((u[0] & 0xF0) == 0xE0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80) {
        cp = ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F);
        return 3;
    }
    if ((u[0] & 0xF8) == 0xF0 && (u[1] & 0xC0) == 0x80 &&
        (u[2] & 0xC0) == 0x80 && (u[3] & 0xC0) == 0x80) {
        cp = ((u[0] & 0x07) << 18) | ((u[1] & 0x3F) << 12) |
             ((u[2] & 0x3F) << 6) | (u[3] & 0x3F);
        return 4;
    }
    return 0;  // invalid
}

// Encode a codepoint to UTF-8. Returns bytes written.
size_t encode_utf8(char32_t cp, char* out) {
    if (cp <= 0x7F) {
        out[0] = static_cast<char>(cp);
        return 1;
    }
    if (cp <= 0x7FF) {
        out[0] = static_cast<char>(0xC0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp <= 0xFFFF) {
        out[0] = static_cast<char>(0xE0 | (cp >> 12));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = static_cast<char>(0xF0 | (cp >> 18));
    out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (cp & 0x3F));
    return 4;
}

// Map fullwidth to halfwidth. Returns true if mapping applied.
bool map_fullwidth(char32_t cp, char32_t& mapped) {
    // ASCII fullwidth letters A-Z, a-z → lowercase halfwidth
    if (cp >= 0xFF21 && cp <= 0xFF3A) {  // fullwidth A-Z
        mapped = 0x0061 + (cp - 0xFF21);  // → halfwidth a-z
        return true;
    }
    if (cp >= 0xFF41 && cp <= 0xFF5A) {  // fullwidth a-z
        mapped = 0x0061 + (cp - 0xFF41);  // → halfwidth a-z
        return true;
    }
    for (auto& r : kFullwidthRanges) {
        if (cp >= r.start && cp <= r.end) {
            mapped = cp - r.offset;
            return true;
        }
    }
    return false;
}

// ASCII lowercase for a single codepoint.
char32_t ascii_lower_cp(char32_t cp) {
    if (cp >= 'A' && cp <= 'Z') {
        return cp + 32;
    }
    return cp;
}

}  // namespace

std::string nfkc_normalize(std::string_view input) {
    std::string result;
    result.reserve(input.size());

    const char* p = input.data();
    const char* end = p + input.size();

    char buf[4];
    while (p < end) {
        char32_t cp;
        size_t len = decode_utf8(p, cp);
        if (len == 0) {
            result.push_back(*p);
            ++p;
            continue;
        }

        char32_t mapped;
        if (map_fullwidth(cp, mapped)) {
            mapped = ascii_lower_cp(mapped);
            size_t out_len = encode_utf8(mapped, buf);
            result.append(buf, out_len);
        } else {
            cp = ascii_lower_cp(cp);
            size_t out_len = encode_utf8(cp, buf);
            result.append(buf, out_len);
        }
        p += len;
    }
    return result;
}

}  // namespace vortex
