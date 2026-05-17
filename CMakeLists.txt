cmake_minimum_required(VERSION 3.16)
project(vortex VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(VORTEX_BUILD_TESTS     "Build unit tests"      OFF)
option(VORTEX_BUILD_BENCHMARKS "Build benchmarks"      OFF)
option(VORTEX_USE_AVX2        "Enable AVX2 code paths"  OFF)
option(VORTEX_USE_ICU         "Enable ICU integration"  OFF)
option(VORTEX_ASAN            "Enable AddressSanitizer" OFF)
option(VORTEX_UBSAN           "Enable UndefinedBehaviorSanitizer" OFF)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set(VORTEX_USE_AVX2 ON)
endif()

file(GLOB_RECURSE VORTEX_SOURCES
    src/core/*.cpp
    src/inverted/*.cpp
)

add_library(vortex STATIC ${VORTEX_SOURCES})
target_include_directories(vortex PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

if(MSVC)
    target_compile_options(vortex PRIVATE /W4 /WX)
else()
    target_compile_options(vortex PRIVATE
        -Wall -Wextra -Wpedantic -Werror
        -Wno-unused-parameter
    )
endif()

if(VORTEX_ASAN)
    target_compile_options(vortex PRIVATE -fsanitize=address)
    target_link_options(vortex PRIVATE -fsanitize=address)
endif()

if(VORTEX_UBSAN)
    target_compile_options(vortex PRIVATE -fsanitize=undefined)
    target_link_options(vortex PRIVATE -fsanitize=undefined)
endif()

if(VORTEX_USE_AVX2)
    target_compile_definitions(vortex PRIVATE VORTEX_HAS_AVX2)
    set_source_files_properties(
        src/inverted/posting_codec_avx2.cpp
        PROPERTIES COMPILE_FLAGS "-mavx2"
    )
endif()

# Phase 0: use std containers with absl-type alias.
# Full absl::flat_hash_map integration when simd build issue resolved.
add_library(vortex_containers INTERFACE)
target_include_directories(vortex_containers INTERFACE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
)
target_link_libraries(vortex PUBLIC vortex_containers)

include(FetchContent)

FetchContent_Declare(
    roaring
    GIT_REPOSITORY https://github.com/RoaringBitmap/CRoaring.git
    GIT_TAG        v4.0.0
)
set(ROARING_BUILD_STATIC ON)
set(ROARING_BUILD_TESTS OFF)
set(ROARING_BUILD_TOOLS OFF)
FetchContent_MakeAvailable(roaring)
target_link_libraries(vortex PUBLIC roaring)

if(VORTEX_BUILD_TESTS)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
    )
    FetchContent_MakeAvailable(googletest)
    enable_testing()
    add_subdirectory(tests)
endif()

if(VORTEX_BUILD_BENCHMARKS)
    FetchContent_Declare(
        googlebenchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG        v1.9.0
    )
    FetchContent_MakeAvailable(googlebenchmark)
    add_subdirectory(benchmarks)
endif()
