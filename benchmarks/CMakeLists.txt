# Benchmarks for inverted index components.

# ── Posting codec benchmark ──
add_executable(bm_posting_codec bm_posting_codec.cpp)
target_link_libraries(bm_posting_codec PRIVATE vortex benchmark::benchmark)

# ── Term dictionary benchmark ──
add_executable(bm_term_dict bm_term_dict.cpp)
target_link_libraries(bm_term_dict PRIVATE vortex benchmark::benchmark)

# ── Scorer benchmark ──
add_executable(bm_scorer bm_scorer.cpp)
target_link_libraries(bm_scorer PRIVATE vortex benchmark::benchmark)

# ── End-to-end search benchmark ──
add_executable(bm_search bm_search.cpp)
target_link_libraries(bm_search PRIVATE vortex benchmark::benchmark)