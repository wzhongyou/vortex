cmake_minimum_required(VERSION 3.16)
project(vortex VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Options ────────────────────────────────────────────
option(VORTEX_BUILD_TESTS     "Build unit tests"      OFF)
option(VORTEX_BUILD_BENCHMARKS "Build benchmarks"      OFF)
option(VORTEX_USE_FLATBUFFERS "Enable FlatBuffers"     ON)

# ── Core library (header-only for now) ────────────────
add_library(vortex INTERFACE)
target_include_directories(vortex INTERFACE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# ── Dependencies ──────────────────────────────────────
include(FetchContent)

if(VORTEX_USE_FLATBUFFERS)
    FetchContent_Declare(
        flatbuffers
        GIT_REPOSITORY https://github.com/google/flatbuffers.git
        GIT_TAG        v24.3.25
    )
    FetchContent_MakeAvailable(flatbuffers)
    target_link_libraries(vortex INTERFACE flatbuffers)
    target_compile_definitions(vortex INTERFACE VORTEX_HAS_FLATBUFFERS)
endif()

# ── Tests ─────────────────────────────────────────────
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

# ── Benchmarks ────────────────────────────────────────
if(VORTEX_BUILD_BENCHMARKS)
    FetchContent_Declare(
        googlebenchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG        v1.9.0
    )
    FetchContent_MakeAvailable(googlebenchmark)
    add_subdirectory(benchmarks)
endif()
