option(VORTEX_BUILD_EXAMPLES "Build example programs" OFF)

if(VORTEX_BUILD_EXAMPLES)
    add_executable(vortex_example basic_usage.cpp)
    target_link_libraries(vortex_example PRIVATE vortex)
    target_include_directories(vortex_example PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${ROARING_INCLUDE_DIR}
    )
    add_subdirectory(search_demo)
endif()