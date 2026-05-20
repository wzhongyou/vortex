include(GoogleTest)

# ── Integration test ──
add_executable(test_integration test_integration.cpp)
target_link_libraries(test_integration PRIVATE vortex gtest gtest_main)
gtest_discover_tests(test_integration)

# ── Unit tests ──
set(UNIT_TESTS
    test_arena
    test_posting_codec
    test_skip_list
    test_term_dict
    test_forward_index
    test_scorer
    test_analyzer
    test_delete_bitmap
    test_external_id_map
    test_wal
)

foreach(test_name ${UNIT_TESTS})
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE vortex gtest gtest_main)
    gtest_discover_tests(${test_name})
endforeach()