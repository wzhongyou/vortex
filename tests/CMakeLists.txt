add_executable(test_integration test_integration.cpp)
target_link_libraries(test_integration PRIVATE vortex gtest gtest_main)

include(GoogleTest)
gtest_discover_tests(test_integration)
