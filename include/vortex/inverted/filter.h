#pragma once

#include <memory>
#include <string>
#include <unordered_set>

#include "vortex/inverted/tokenizer.h"

namespace vortex {

class TokenFilter {
public:
    virtual void process(Token token, TokenConsumer& downstream) = 0;
    virtual ~TokenFilter() = default;
};

// Convert token text to ASCII lowercase.
class LowercaseFilter : public TokenFilter {
public:
    void process(Token token, TokenConsumer& downstream) override;
};

// Remove tokens whose text is in the stopword set.
class StopwordFilter : public TokenFilter {
public:
    explicit StopwordFilter(std::shared_ptr<std::unordered_set<std::string>> words);

    void process(Token token, TokenConsumer& downstream) override;

private:
    std::shared_ptr<std::unordered_set<std::string>> stopwords_;
};

// Apply NFKC normalization to token text.
// Requires a stable buffer for the normalized text (typically arena-allocated).
class NFKCFilter : public TokenFilter {
public:
    void process(Token token, TokenConsumer& downstream) override;
};

}  // namespace vortex
