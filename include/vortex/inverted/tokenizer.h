#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "vortex/core/types.h"

namespace vortex {

struct Token {
    std::string_view text;  // points into source or arena buffer
    uint16_t position;
    uint16_t start_offset;
    uint16_t end_offset;
};

class TokenConsumer {
public:
    virtual void on_token(Token token) = 0;
    virtual ~TokenConsumer() = default;
};

class Tokenizer {
public:
    virtual void tokenize(std::string_view text, TokenConsumer& consumer) = 0;
    virtual ~Tokenizer() = default;
};

// Latin-script tokenizer: splits on Unicode letter/digit boundaries.
class StandardTokenizer : public Tokenizer {
public:
    void tokenize(std::string_view text, TokenConsumer& consumer) override;
};

// CJK bigram tokenizer: "我爱北京" → ["我爱","爱北","北京"]
class CJKBigramTokenizer : public Tokenizer {
public:
    void tokenize(std::string_view text, TokenConsumer& consumer) override;
};

// Auto-dispatch: detects CJK character ratio and delegates.
class MixedTokenizer : public Tokenizer {
public:
    MixedTokenizer();

    void tokenize(std::string_view text, TokenConsumer& consumer) override;

private:
    std::unique_ptr<StandardTokenizer> standard_;
    std::unique_ptr<CJKBigramTokenizer> cjk_;
};

// Check if a UTF-8 codepoint at `p` is a CJK character.
// Returns bytes consumed (0 if invalid, 1-4 for valid).
// Sets *is_cjk to true if the character is in a CJK range.
size_t peek_cjk(const char* p, const char* end, bool& is_cjk);

}  // namespace vortex
