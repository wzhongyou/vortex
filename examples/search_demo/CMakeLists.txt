find_package(Threads REQUIRED)

add_executable(vortex_search_demo main.cpp)
target_link_libraries(vortex_search_demo PRIVATE vortex Threads::Threads)
target_include_directories(vortex_search_demo PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)
if(NOT MSVC)
    target_compile_options(vortex_search_demo PRIVATE -Wall -Wextra -Wpedantic)
endif()
