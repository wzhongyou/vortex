#include "vortex/inverted/term_dict.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <vector>

namespace vortex {

namespace {

constexpr uint8_t F_FINAL = 0x01;

[[maybe_unused]] static void write_vbyte(std::vector<uint8_t>& buf, uint64_t v) {
    while (v >= 0x80) {
        buf.push_back(static_cast<uint8_t>(v | 0x80));
        v >>= 7;
    }
    buf.push_back(static_cast<uint8_t>(v));
}

uint64_t read_vbyte(const uint8_t*& p) {
    uint64_t v = 0;
    int s = 0;
    while (*p & 0x80) {
        v |= static_cast<uint64_t>(*p & 0x7F) << s;
        s += 7;
        p++;
    }
    v |= static_cast<uint64_t>(*p) << s;
    p++;
    return v;
}

void read_term_info(const uint8_t* p, TermInfo& info) {
    std::memcpy(&info.doc_freq, p, 4);
    std::memcpy(&info.posting_offset, p + 4, 8);
    std::memcpy(&info.posting_len, p + 12, 4);
}

struct Arc {
    uint8_t label;
    uint32_t target;
};

struct Node {
    std::vector<Arc> arcs;
    bool final = false;
    TermInfo term_info;
    uint32_t offset = 0;
};

}  // namespace

struct TermDictBuilder::Impl {
    std::vector<Node> nodes;
    std::string last_term;  // for ordering validation
};

TermDictBuilder::TermDictBuilder()
    : impl_(std::make_unique<Impl>()) {
    impl_->nodes.emplace_back();  // root
}

TermDictBuilder::~TermDictBuilder() = default;

Status TermDictBuilder::insert(std::string_view term, const TermInfo& info) {
    if (!impl_->last_term.empty() && term <= impl_->last_term) {
        return Status::InvalidArgument(
            "terms must be in strict lexicographic order: '" +
            std::string(term) + "' after '" + impl_->last_term + "'");
    }

    uint32_t node_idx = 0;  // start at root
    for (size_t i = 0; i < term.size(); i++) {
        uint8_t c = static_cast<uint8_t>(term[i]);
        Node& node = impl_->nodes[node_idx];

        bool found = false;
        for (size_t _ai = 0; _ai < node.arcs.size(); _ai++) { auto& arc = node.arcs[_ai]; (void)arc;
            if (arc.label == c) {
                node_idx = arc.target;
                found = true;
                break;
            }
        }

        if (!found) {
            // Note: emplace_back may reallocate nodes(), invalidating all references.
            // Access via index, not reference, after mutation.
            uint32_t new_idx = static_cast<uint32_t>(impl_->nodes.size());
            impl_->nodes.emplace_back();
            impl_->nodes[node_idx].arcs.push_back({c, new_idx});
            // Keep sorted
            auto& a = impl_->nodes[node_idx].arcs;
            for (size_t j = a.size() - 1; j > 0 && a[j].label < a[j-1].label; j--) {
                std::swap(a[j], a[j-1]);
            }
            node_idx = new_idx;
        }
    }

    Node& final_node = impl_->nodes[node_idx];
    final_node.final = true;
    final_node.term_info = info;

    impl_->last_term = std::string(term);
    term_count_++;
    return Status::OK();
}

std::vector<uint8_t> TermDictBuilder::finish() {
    std::vector<uint8_t> fst_data;
    fst_data.resize(8);  // header placeholder

    if (impl_->nodes.empty()) return fst_data;

    // DFS to get serialization order
    std::vector<uint32_t> order;
    std::vector<bool> visited(impl_->nodes.size(), false);

    std::function<void(uint32_t)> dfs = [&](uint32_t idx) {
        if (visited[idx]) return;
        visited[idx] = true;
        order.push_back(idx);

        auto& node = impl_->nodes[idx];
        std::sort(node.arcs.begin(), node.arcs.end(),
                  [](const Arc& a, const Arc& b) { return a.label < b.label; });

        for (size_t _ai = 0; _ai < node.arcs.size(); _ai++) { auto& arc = node.arcs[_ai]; (void)arc;
            dfs(arc.target);
        }
    };
    dfs(0);

    // Compute vbyte size needed for a value
    auto vbyte_len = [](uint64_t v) -> int {
        int len = 1;
        while (v >= 0x80) { len++; v >>= 7; }
        return len;
    };

    // Pass 1: compute node offsets in REVERSE DFS order so that
    // children's offsets are known when computing parent vbyte sizes.
    fst_data.clear();
    fst_data.resize(8);
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        Node& node = impl_->nodes[*it];
        node.offset = static_cast<uint32_t>(fst_data.size());

        size_t node_size = 2;
        for (auto& arc : node.arcs) {
            node_size += 1;
            node_size += vbyte_len(impl_->nodes[arc.target].offset);
        }
        if (node.final) node_size += 16;
        fst_data.resize(fst_data.size() + node_size);
    }

    // Pass 2: overwrite node data at the pre-computed offsets.
    // Offsets from Pass 1 are accurate, so we write in-place.
    for (uint32_t idx : order) {
        Node& node = impl_->nodes[idx];

        uint8_t fl = node.final ? F_FINAL : 0;
        fst_data[node.offset] = fl;
        fst_data[node.offset + 1] = static_cast<uint8_t>(node.arcs.size());

        size_t wp = node.offset + 2;
        for (auto& arc : node.arcs) {
            fst_data[wp++] = arc.label;
            uint32_t child_off = impl_->nodes[arc.target].offset;
            uint64_t v = child_off;
            while (v >= 0x80) {
                fst_data[wp++] = static_cast<uint8_t>(v | 0x80);
                v >>= 7;
            }
            fst_data[wp++] = static_cast<uint8_t>(v);
        }

        if (node.final) {
            std::memcpy(fst_data.data() + wp, &node.term_info.doc_freq, 4);
            std::memcpy(fst_data.data() + wp + 4, &node.term_info.posting_offset, 8);
            std::memcpy(fst_data.data() + wp + 12, &node.term_info.posting_len, 4);
        }
    }

    // Write header
    uint32_t tc = static_cast<uint32_t>(term_count_);
    std::memcpy(fst_data.data(), &tc, 4);
    std::memcpy(fst_data.data() + 4, &impl_->nodes[0].offset, 4);

    return fst_data;
}

// ── TermDict (Query Side) ──

Result<std::unique_ptr<TermDict>> TermDict::from_memory(
    const uint8_t* data, size_t len) {
    if (len < 8) {
        return Result<std::unique_ptr<TermDict>>::Err(
            Status::CorruptIndex("FST data too short"));
    }

    auto dict = std::unique_ptr<TermDict>(new TermDict());
    dict->data_ = data;
    dict->data_len_ = len;
    std::memcpy(&dict->term_count_, data, 4);
    std::memcpy(&dict->root_offset_, data + 4, 4);

    return Result<std::unique_ptr<TermDict>>::Ok(std::move(dict));
}

const TermInfo* TermDict::lookup(std::string_view term) const {
    if (!data_ || root_offset_ >= data_len_) return nullptr;

    uint32_t node_off = root_offset_;
    for (size_t i = 0; i < term.size(); i++) {
        uint8_t c = static_cast<uint8_t>(term[i]);

        const uint8_t* p = data_ + node_off;
        uint8_t flags = *p++; (void)flags;
        uint8_t num_arcs = *p++;

        bool found = false;
        uint32_t next_off = 0;

        for (uint8_t j = 0; j < num_arcs; j++) {
            uint8_t label = *p++;
            uint64_t target = read_vbyte(p);

            if (label == c) {
                next_off = static_cast<uint32_t>(target);
                found = true;
                break;
            }
            if (label > c) break;
        }

        if (!found) return nullptr;

        if (i == term.size() - 1) {
            if (next_off >= data_len_) return nullptr;
            const uint8_t* np = data_ + next_off;
            if (*np & F_FINAL) {
                np++;  // flags
                uint8_t n_arcs = *np++;
                for (uint8_t j = 0; j < n_arcs; j++) {
                    np++;  // label
                    read_vbyte(np);  // target
                }
                static TermInfo info;
                read_term_info(np, info);
                return &info;
            }
            return nullptr;
        }

        node_off = next_off;
    }

    return nullptr;
}

void TermDict::prefix_range(std::string_view prefix,
    std::function<bool(std::string_view, const TermInfo&)> fn) const {
    if (!data_ || root_offset_ >= data_len_) return;

    uint32_t node_off = root_offset_;
    const uint8_t* p;

    for (size_t i = 0; i < prefix.size(); i++) {
        uint8_t c = static_cast<uint8_t>(prefix[i]);
        p = data_ + node_off;
        uint8_t flags = *p++; (void)flags;
        uint8_t num_arcs = *p++;

        bool found = false;
        for (uint8_t j = 0; j < num_arcs; j++) {
            uint8_t label = *p++;
            uint64_t target = read_vbyte(p);
            if (label == c) {
                node_off = static_cast<uint32_t>(target);
                found = true;
                break;
            }
            if (label > c) break;
        }
        if (!found) return;
    }

    std::vector<std::pair<uint32_t, std::string>> stack;
    stack.push_back({node_off, std::string(prefix)});

    while (!stack.empty()) {
        auto [node_idx, current_term] = std::move(stack.back());
        stack.pop_back();

        const uint8_t* np = data_ + node_idx;
        uint8_t flags = *np++;
        uint8_t num_arcs = *np++;

        // Read arcs
        struct ArcPair { uint8_t label; uint32_t target; };
        std::vector<ArcPair> arcs;
        for (uint8_t j = 0; j < num_arcs; j++) {
            uint8_t label = *np++;
            uint64_t target = read_vbyte(np);
            arcs.push_back({label, static_cast<uint32_t>(target)});
        }

        if (flags & F_FINAL) {
            TermInfo info;
            read_term_info(np, info);
            if (!fn(current_term, info)) return;
        }

        // Push children in reverse for lexicographic order
        for (auto it = arcs.rbegin(); it != arcs.rend(); ++it) {
            stack.push_back({it->target, current_term + static_cast<char>(it->label)});
        }
    }
}

uint64_t TermDict::read_vbyte(const uint8_t*& p) {
    return vortex::read_vbyte(p);
}

}  // namespace vortex
