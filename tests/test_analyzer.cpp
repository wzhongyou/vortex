#include <algorithm>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "gtest/gtest.h"
#include "vortex/core/arena.h"
#include "vortex/inverted/analyzer.h"
#include "vortex/inverted/filter.h"
#include "vortex/inverted/tokenizer.h"

namespace vortex {
namespace {

// Test consumer that collects tokens in a vector
class CollectConsumer : public TokenConsumer {
public:
    void on_token(Token token) override {
        tokens.push_back(std::string(token.text));
        positions.push_back(token.position);
    }
    std::vector<std::string> tokens;
    std::vector<uint16_t> positions;
};

TEST(StandardTokenizerTest, BasicTokenization) {
    StandardTokenizer tokenizer;
    CollectConsumer consumer;
    tokenizer.tokenize("Hello World! This is a test.", consumer);
    ASSERT_EQ(consumer.tokens.size(), 6u);
    EXPECT_EQ(consumer.tokens[0], "Hello");
    EXPECT_EQ(consumer.tokens[1], "World");
    EXPECT_EQ(consumer.tokens[2], "This");
    EXPECT_EQ(consumer.tokens[3], "is");
    EXPECT_EQ(consumer.tokens[4], "a");
    EXPECT_EQ(consumer.tokens[5], "test");
}

TEST(StandardTokenizerTest, EmptyInput) {
    StandardTokenizer tokenizer;
    CollectConsumer consumer;
    tokenizer.tokenize("", consumer);
    EXPECT_EQ(consumer.tokens.size(), 0u);
}

TEST(StandardTokenizerTest, OnlyPunctuation) {
    StandardTokenizer tokenizer;
    CollectConsumer consumer;
    tokenizer.tokenize("!@#$%^&*()", consumer);
    EXPECT_EQ(consumer.tokens.size(), 0u);
}

TEST(StandardTokenizerTest, Numbers) {
    StandardTokenizer tokenizer;
    CollectConsumer consumer;
    tokenizer.tokenize("test123 456test", consumer);
    ASSERT_EQ(consumer.tokens.size(), 2u);
    EXPECT_EQ(consumer.tokens[0], "test123");
    EXPECT_EQ(consumer.tokens[1], "456test");
}

TEST(CJKBigramTokenizerTest, BasicBigram) {
    CJKBigramTokenizer tokenizer;
    CollectConsumer consumer;
    tokenizer.tokenize("我爱北京", consumer);
    ASSERT_EQ(consumer.tokens.size(), 3u);
    EXPECT_EQ(consumer.tokens[0], "我爱");
    EXPECT_EQ(consumer.tokens[1], "爱北");
    EXPECT_EQ(consumer.tokens[2], "北京");
}

TEST(CJKBigramTokenizerTest, MixedContent) {
    CJKBigramTokenizer tokenizer;
    CollectConsumer consumer;
    tokenizer.tokenize("Hello世界", consumer);
    // "Hello" and CJK bigrams of "世界"
    ASSERT_GE(consumer.tokens.size(), 1u);
}

TEST(MixedTokenizerTest, LatinText) {
    MixedTokenizer tokenizer;
    CollectConsumer consumer;
    tokenizer.tokenize("hello world test", consumer);
    ASSERT_EQ(consumer.tokens.size(), 3u);
    EXPECT_EQ(consumer.tokens[0], "hello");
    EXPECT_EQ(consumer.tokens[1], "world");
    EXPECT_EQ(consumer.tokens[2], "test");
}

TEST(LowercaseFilterTest, PassesThrough) {
    LowercaseFilter filter;
    CollectConsumer consumer;
    filter.process(Token{"Hello", 0, 0, 5}, consumer);
    ASSERT_EQ(consumer.tokens.size(), 1u);
    // LowercaseFilter is a no-op in V1 (normalization is at the Analyzer level)
    EXPECT_EQ(consumer.tokens[0], "Hello");
}

TEST(LowercaseFilterTest, AlreadyLowercase) {
    LowercaseFilter filter;
    CollectConsumer consumer;
    filter.process(Token{"hello", 0, 0, 5}, consumer);
    ASSERT_EQ(consumer.tokens.size(), 1u);
    EXPECT_EQ(consumer.tokens[0], "hello");
}

TEST(StopwordFilterTest, FilterStopwords) {
    auto stopwords = std::make_shared<std::unordered_set<std::string>>(
        std::initializer_list<std::string>{"the", "a", "an"});
    StopwordFilter filter(stopwords);

    CollectConsumer consumer;
    filter.process(Token{"the", 0, 0, 3}, consumer);
    filter.process(Token{"hello", 1, 4, 9}, consumer);
    filter.process(Token{"a", 2, 10, 11}, consumer);
    filter.process(Token{"world", 3, 12, 17}, consumer);

    ASSERT_EQ(consumer.tokens.size(), 2u);
    EXPECT_EQ(consumer.tokens[0], "hello");
    EXPECT_EQ(consumer.tokens[1], "world");
}

TEST(AnalyzerTest, FullPipeline) {
    auto tokenizer = std::make_unique<StandardTokenizer>();
    auto stopwords = std::make_shared<std::unordered_set<std::string>>(
        std::initializer_list<std::string>{"the", "a", "is"});
    std::vector<std::unique_ptr<TokenFilter>> filters;
    filters.push_back(std::make_unique<LowercaseFilter>());
    filters.push_back(std::make_unique<StopwordFilter>(stopwords));

    Analyzer analyzer(std::move(tokenizer), std::move(filters));
    Arena arena(1024);

    std::vector<Analyzer::TermWithFreq> result;
    analyzer.analyze("The Quick Brown Fox", result, arena);

    // "the" is stopword (nfkc_normalize lowercases input),
    // "quick", "brown", "fox" remain. Each appears once.
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) { return a.term < b.term; });
    ASSERT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0].term, "brown");
    EXPECT_EQ(result[0].tf, 1u);
    EXPECT_EQ(result[1].term, "fox");
    EXPECT_EQ(result[1].tf, 1u);
    EXPECT_EQ(result[2].term, "quick");
    EXPECT_EQ(result[2].tf, 1u);
}

TEST(AnalyzerTest, RepeatedTerms) {
    auto tokenizer = std::make_unique<StandardTokenizer>();
    std::vector<std::unique_ptr<TokenFilter>> filters;
    filters.push_back(std::make_unique<LowercaseFilter>());

    Analyzer analyzer(std::move(tokenizer), std::move(filters));
    Arena arena(1024);

    std::vector<Analyzer::TermWithFreq> result;
    analyzer.analyze("hello hello world", result, arena);

    ASSERT_EQ(result.size(), 2u);
    // Each term appears with its frequency
    for (auto& t : result) {
        if (t.term == "hello") {
            EXPECT_EQ(t.tf, 2u);
        } else if (t.term == "world") {
            EXPECT_EQ(t.tf, 1u);
        } else {
            FAIL() << "unexpected term: " << t.term;
        }
    }
}

TEST(CJKTest, PeekCJK) {
    const char* text = "a中国";  // 'a' followed by CJK chars
    const char* end = text + 7;
    bool is_cjk = false;
    // First char is ASCII
    size_t consumed = peek_cjk(text, end, is_cjk);
    EXPECT_EQ(consumed, 1u);
    EXPECT_FALSE(is_cjk);

    // Second char is CJK (中)
    consumed = peek_cjk(text + 1, end, is_cjk);
    EXPECT_GT(consumed, 1u);
    EXPECT_TRUE(is_cjk);
}

}  // namespace
}  // namespace vortex