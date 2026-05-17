#include "vortex/inverted/analyzer.h"

#include <string>
#include <unordered_map>

#include "vortex/inverted/unicode.h"

namespace vortex {

Analyzer::Analyzer(std::unique_ptr<Tokenizer> tokenizer,
                   std::vector<std::unique_ptr<TokenFilter>> filters)
    : tokenizer_(std::move(tokenizer))
    , filters_(std::move(filters)) {}

void Analyzer::analyze(std::string_view text,
                        std::vector<TermWithFreq>& output,
                        Arena& arena) {
    output.clear();

    // Step 1: NFKC normalize
    std::string normalized = nfkc_normalize(text);
    std::string_view input = arena_strdup(arena, normalized);

    // Step 2: Tokenize into raw tokens
    struct TokenBuf {
        std::string_view text;
        uint16_t position;
    };
    std::vector<TokenBuf> raw_tokens;

    class CollectRaw : public TokenConsumer {
    public:
        std::vector<TokenBuf>* tokens;
        void on_token(Token tok) override {
            tokens->push_back({tok.text, tok.position});
        }
    } collector;
    collector.tokens = &raw_tokens;
    tokenizer_->tokenize(input, collector);

    // Step 3: Apply filter chain to each token.
    // A token survives if all filters pass it through.
    // We use a simple chain: consumer[i] → filter[i] → consumer[i+1] → ... → term_freq map.

    std::unordered_map<std::string, uint32_t> freq;

    for (auto& tb : raw_tokens) {
        // Apply filters
        bool keep = true;
        for (auto& f : filters_) {
            // Use a pass-through detector: if filter calls downstream.on_token(),
            // the token is kept; otherwise dropped.
            bool passed = false;
            class PassCheck : public TokenConsumer {
            public:
                bool* flag;
                void on_token(Token) override { *flag = true; }
            } check;
            check.flag = &passed;

            Token tok;
            tok.text = tb.text;
            tok.position = tb.position;
            tok.start_offset = 0;
            tok.end_offset = 0;

            f->process(tok, check);
            if (!passed) {
                keep = false;
                break;
            }
        }
        if (!keep) continue;

        // Aggregate frequency. Store term on arena for stable string_view.
        auto it = freq.find(std::string(tb.text));
        if (it != freq.end()) {
            it->second++;
        } else {
            std::string_view arena_term = arena_strdup(arena, tb.text);
            freq[std::string(arena_term)] = 1;
        }
    }

    // Step 4: Build output — copy terms to arena for stable string_views
    for (auto& pair : freq) {
        std::string_view arena_term = arena_strdup(arena, pair.first);
        output.push_back({arena_term, pair.second});
    }
}

}  // namespace vortex
