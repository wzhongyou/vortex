#pragma once

#include <string>
#include <vector>

#include "vortex/core/types.h"

namespace vortex {

enum class QueryType : uint8_t {
    TERM,
    AND,
    OR,
    NOT,
};

struct Query {
    QueryType type;
    std::string term;
    std::vector<Query> sub_queries;

    static Query Term(std::string t) {
        return {QueryType::TERM, std::move(t), {}};
    }
    static Query And(std::vector<Query> subs) {
        return {QueryType::AND, {}, std::move(subs)};
    }
    static Query Or(std::vector<Query> subs) {
        return {QueryType::OR, {}, std::move(subs)};
    }
    static Query Not(Query q) {
        std::vector<Query> subs;
        subs.push_back(std::move(q));
        return {QueryType::NOT, {}, std::move(subs)};
    }
    static Query Not(Query positive, Query negative) {
        std::vector<Query> subs;
        subs.push_back(std::move(positive));
        subs.push_back(std::move(negative));
        return {QueryType::NOT, {}, std::move(subs)};
    }
};

}  // namespace vortex
