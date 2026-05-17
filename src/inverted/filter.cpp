#include "vortex/inverted/filter.h"

#include "vortex/inverted/unicode.h"

namespace vortex {

void LowercaseFilter::process(Token token, TokenConsumer& downstream) {
    // For tokens pointing into the source text (which is normalized before
    // tokenization), the text is already lowercase. This filter is a no-op
    // when downstream normalization is used, but provided for standalone use.
    downstream.on_token(token);
}

StopwordFilter::StopwordFilter(
    std::shared_ptr<std::unordered_set<std::string>> words)
    : stopwords_(std::move(words)) {}

void StopwordFilter::process(Token token, TokenConsumer& downstream) {
    if (stopwords_->count(std::string(token.text)) == 0) {
        downstream.on_token(token);
    }
}

void NFKCFilter::process(Token token, TokenConsumer& downstream) {
    // NFKC normalization is applied at the Analyzer level before tokenization.
    // This filter is a no-op in the default pipeline, provided for standalone use.
    downstream.on_token(token);
}

}  // namespace vortex
