#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "vortex/core/arena.h"
#include "vortex/inverted/tokenizer.h"
#include "vortex/inverted/filter.h"

namespace vortex {

class Analyzer {
public:
    Analyzer(std::unique_ptr<Tokenizer> tokenizer,
             std::vector<std::unique_ptr<TokenFilter>> filters);

    struct TermWithFreq {
        std::string_view term;  // points into arena
        uint32_t tf;
    };

    // Tokenize text, apply filters, aggregate term frequencies.
    // Output terms and their in-document frequencies.
    // All term string data is allocated on the given arena.
    void analyze(std::string_view text,
                 std::vector<TermWithFreq>& output,
                 Arena& arena);

private:
    std::unique_ptr<Tokenizer> tokenizer_;
    std::vector<std::unique_ptr<TokenFilter>> filters_;
};

}  // namespace vortex
