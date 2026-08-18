cmake_minimum_required(VERSION 3.16)
project(pgquarry CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra)

# Locate libpq. On most distros `pg_config` tells us where headers/libs live.
find_program(PG_CONFIG pg_config)
if(PG_CONFIG)
  execute_process(COMMAND ${PG_CONFIG} --includedir
                  OUTPUT_VARIABLE PG_INCLUDE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND ${PG_CONFIG} --libdir
                  OUTPUT_VARIABLE PG_LIB_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND ${PG_CONFIG} --sharedir
                  OUTPUT_VARIABLE PG_SHARE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
else()
  message(WARNING "pg_config not found; falling back to system default paths (libpq-dev)")
  set(PG_INCLUDE_DIR "/usr/include")
  set(PG_LIB_DIR "/usr/lib64")
  set(PG_SHARE_DIR "/usr/share/postgresql")
endif()

find_package(spdlog CONFIG REQUIRED)

# toml++ v3.4.0 — single-header, MIT. Vendored at src/third_party/toml.hpp,
# same convention walkrie uses for the same header.
add_library(tomlplusplus INTERFACE)
target_include_directories(tomlplusplus INTERFACE ${CMAKE_SOURCE_DIR}/src/third_party)

# llama.cpp — built as part of this project via add_subdirectory.
# EXCLUDE_FROM_ALL keeps llama.cpp's own example binaries from being built
# unless explicitly requested. Set LLAMA_DIR on the cmake command line if
# your checkout is somewhere other than third_party/llama.cpp:
#   cmake -DLLAMA_DIR=/path/to/llama.cpp ..
set(LLAMA_DIR "${CMAKE_SOURCE_DIR}/third_party/llama.cpp" CACHE PATH "Path to llama.cpp source checkout")
set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS    OFF CACHE BOOL "" FORCE)

add_subdirectory(${LLAMA_DIR} ${CMAKE_BINARY_DIR}/llama_build EXCLUDE_FROM_ALL)

link_directories(${PG_LIB_DIR})

add_executable(pgquarry_worker
    src/worker_main.cpp
    src/embedding_provider.cpp
    src/llama_provider.cpp
    src/generation_provider.cpp
    src/llama_generation_provider.cpp
    src/job_store.cpp
    src/job_dispatcher.cpp
    src/writeback_sql_builder.cpp
    src/watch_cache.cpp
)
target_include_directories(pgquarry_worker PRIVATE
    ${PG_INCLUDE_DIR}
    ${LLAMA_DIR}/include
    ${LLAMA_DIR}/ggml/include
)
target_link_libraries(pgquarry_worker PRIVATE
    pq
    spdlog::spdlog_header_only
    llama
    ggml
    tomlplusplus
)

# `CREATE EXTENSION pgquarry;` needs these in Postgres's own extension dir —
# separate from the worker binary, which the user places wherever they run it.
install(FILES pgquarry.control sql/pgquarry--1.0.sql
        DESTINATION ${PG_SHARE_DIR}/extension)

# doctest v2.4.11 — single-header, MIT. Vendored at src/third_party/doctest.h,
# same convention as toml++ above. Only covers pure logic (AppConfig::validate())
# so it doesn't link libpq/llama.cpp at all.
enable_testing()
add_executable(pgquarry_tests tests/config_test.cpp)
target_include_directories(pgquarry_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/third_party
)
target_compile_definitions(pgquarry_tests PRIVATE
    TESTS_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures"
)
add_test(NAME pgquarry_tests COMMAND pgquarry_tests)
