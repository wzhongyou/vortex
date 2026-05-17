#include "vortex/inverted/tokenizer.h"

#include <algorithm>
#include <vector>

namespace vortex {

// ── Unicode helpers ──

namespace {

bool is_ascii_letter(uint8_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_ascii_digit(uint8_t c) {
    return c >= '0' && c <= '9';
}

bool is_ascii_letter_or_digit(uint8_t c) {
    return is_ascii_letter(c) || is_ascii_digit(c);
}

// Full UTF-8 codepoint decode. Returns bytes consumed, 0 on error.
size_t decode_utf8_full(const char* s, const char* end, char32_t& cp) {
    if (s >= end) return 0;
    auto u = static_cast<uint8_t>(*s);
    if ((u & 0x80) == 0) {
        cp = u;
        return 1;
    }
    size_t avail = static_cast<size_t>(end - s);
    if ((u & 0xE0) == 0xC0 && avail >= 2 && (static_cast<uint8_t>(s[1]) & 0xC0) == 0x80) {
        cp = ((u & 0x1F) << 6) | (static_cast<uint8_t>(s[1]) & 0x3F);
        return 2;
    }
    if ((u & 0xF0) == 0xE0 && avail >= 3 &&
        (static_cast<uint8_t>(s[1]) & 0xC0) == 0x80 &&
        (static_cast<uint8_t>(s[2]) & 0xC0) == 0x80) {
        cp = ((u & 0x0F) << 12) | ((static_cast<uint8_t>(s[1]) & 0x3F) << 6) |
             (static_cast<uint8_t>(s[2]) & 0x3F);
        return 3;
    }
    if ((u & 0xF8) == 0xF0 && avail >= 4 &&
        (static_cast<uint8_t>(s[1]) & 0xC0) == 0x80 &&
        (static_cast<uint8_t>(s[2]) & 0xC0) == 0x80 &&
        (static_cast<uint8_t>(s[3]) & 0xC0) == 0x80) {
        cp = ((u & 0x07) << 18) | ((static_cast<uint8_t>(s[1]) & 0x3F) << 12) |
             ((static_cast<uint8_t>(s[2]) & 0x3F) << 6) |
             (static_cast<uint8_t>(s[3]) & 0x3F);
        return 4;
    }
    return 0;
}

// CJK Unicode blocks
bool is_cjk_codepoint(char32_t cp) {
    return (cp >= 0x4E00 && cp <= 0x9FFF) ||   // CJK Unified Ideographs
           (cp >= 0x3400 && cp <= 0x4DBF) ||   // CJK Unified Ideographs Extension A
           (cp >= 0x20000 && cp <= 0x2A6DF) || // Extension B
           (cp >= 0xF900 && cp <= 0xFAFF) ||   // CJK Compatibility Ideographs
           (cp >= 0x2F800 && cp <= 0x2FA1F) || // Compatibility Supplement
           (cp >= 0x3000 && cp <= 0x303F) ||   // CJK Symbols and Punctuation
           (cp >= 0xFF00 && cp <= 0xFFEF) ||   // Halfwidth and Fullwidth Forms
           (cp >= 0x3040 && cp <= 0x309F) ||   // Hiragana
           (cp >= 0x30A0 && cp <= 0x30FF) ||   // Katakana
           (cp >= 0xAC00 && cp <= 0xD7AF);     // Hangul Syllables
}

[[maybe_unused]] static bool is_unicode_letter(char32_t cp) {
    return is_ascii_letter(static_cast<uint8_t>(cp)) || is_cjk_codepoint(cp);
}

[[maybe_unused]] static bool is_unicode_letter_or_digit(char32_t cp) {
    return is_ascii_letter_or_digit(static_cast<uint8_t>(cp)) ||
           is_cjk_codepoint(cp);
}

}  // namespace

size_t peek_cjk(const char* p, const char* end, bool& is_cjk) {
    char32_t cp;
    size_t len = decode_utf8_full(p, end, cp);
    if (len == 0) {
        is_cjk = false;
        return 0;
    }
    is_cjk = is_cjk_codepoint(cp);
    return len;
}

// ── StandardTokenizer ──

void StandardTokenizer::tokenize(std::string_view text, TokenConsumer& consumer) {
    const char* p = text.data();
    const char* end = p + text.size();
    uint16_t pos = 0;
    uint16_t offset = 0;

    while (p < end) {
        // Skip non-letter/non-digit characters
        while (p < end) {
            char32_t cp;
            size_t len = decode_utf8_full(p, end, cp);
            if (len > 0 && is_unicode_letter_or_digit(cp)) break;
            if (len == 0) len = 1;
            p += len;
            offset += static_cast<uint16_t>(len);
        }

        if (p >= end) break;

        const char* start = p;
        uint16_t start_offset = offset;

        // Consume letter/digit run
        while (p < end) {
            char32_t cp;
            size_t len = decode_utf8_full(p, end, cp);
            if (len == 0 || !is_unicode_letter_or_digit(cp)) break;
            p += len;
            offset += static_cast<uint16_t>(len);
        }

        Token tok;
        tok.text = std::string_view(start, static_cast<size_t>(p - start));
        tok.position = pos++;
        tok.start_offset = start_offset;
        tok.end_offset = offset;
        consumer.on_token(tok);
    }
}

// ── CJKBigramTokenizer ──

void CJKBigramTokenizer::tokenize(std::string_view text, TokenConsumer& consumer) {
    if (text.size() < 2) return;

    // Collect all CJK character positions
    struct CjkPos {
        const char* ptr;
        uint16_t offset;
        size_t byte_len;
    };
    std::vector<CjkPos> cjk_chars;

    const char* p = text.data();
    const char* end = p + text.size();
    uint16_t offset = 0;

    while (p < end) {
        bool is_cjk;
        size_t len = peek_cjk(p, end, is_cjk);
        if (len > 0 && is_cjk) {
            cjk_chars.push_back({p, offset, len});
        }
        if (len == 0) len = 1;
        p += len;
        offset += static_cast<uint16_t>(len);
    }

    // Generate bigrams: consecutive pairs
    for (size_t i = 0; i + 1 < cjk_chars.size(); ++i) {
        const CjkPos& a = cjk_chars[i];
        const CjkPos& b = cjk_chars[i + 1];

        Token tok;
        tok.text = std::string_view(a.ptr, static_cast<size_t>(
            (b.ptr + b.byte_len) - a.ptr));
        tok.position = static_cast<uint16_t>(i);
        tok.start_offset = a.offset;
        tok.end_offset = b.offset + static_cast<uint16_t>(b.byte_len);
        consumer.on_token(tok);
    }
}

// ── MixedTokenizer ──

MixedTokenizer::MixedTokenizer()
    : standard_(std::make_unique<StandardTokenizer>())
    , cjk_(std::make_unique<CJKBigramTokenizer>()) {}

void MixedTokenizer::tokenize(std::string_view text, TokenConsumer& consumer) {
    // Count CJK vs total characters
    const char* p = text.data();
    const char* end = p + text.size();
    size_t total_chars = 0;
    size_t cjk_count = 0;

    while (p < end) {
        bool is_cjk;
        size_t len = peek_cjk(p, end, is_cjk);
        if (len > 0) {
            total_chars++;
            if (is_cjk) cjk_count++;
        } else {
            len = 1;
            total_chars++;
        }
        p += len;
    }

    // If >50% CJK characters, use bigram; otherwise use standard
    if (total_chars > 0 && static_cast<double>(cjk_count) / total_chars > 0.5) {
        cjk_->tokenize(text, consumer);
    } else {
        standard_->tokenize(text, consumer);
    }
}

}  // namespace vortex
