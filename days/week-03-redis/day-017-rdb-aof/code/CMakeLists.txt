cmake_minimum_required(VERSION 3.20)

project(archdesign_day017 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 有编解码和基准，默认 Release。
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# 持久化的每个概念一个模块：bytes / page_store / rdb / mem_fs / resp / aof / engine。
add_library(day17_persistence STATIC
    src/bytes.cpp
    src/page_store.cpp
    src/rdb.cpp
    src/mem_fs.cpp
    src/resp.cpp
    src/aof.cpp
    src/engine.cpp
)
target_include_directories(day17_persistence PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(persistence_demo
    main.cpp
    demo.cpp
    bench.cpp
    tests/test_support.cpp
    tests/test_registry.cpp
    tests/tests_cow_rdb.cpp
    tests/tests_aof.cpp
    tests/tests_rewrite_model.cpp
)
target_link_libraries(persistence_demo PRIVATE day17_persistence)
target_include_directories(persistence_demo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if (MSVC)
    target_compile_options(day17_persistence PRIVATE /W4 /permissive-)
    target_compile_options(persistence_demo PRIVATE /W4 /permissive-)
else()
    target_compile_options(day17_persistence PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(persistence_demo PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()

# 每个用例单独注册：坏在哪条不变量上，ctest 直接报名字。
set(DAY17_TEST_CASES
    page_store_cow
    rdb_roundtrip
    rdb_ttl
    rdb_checksum
    rdb_bgsave
    aof_resp
    aof_always
    aof_everysec
    aof_no
    aof_truncated
    aof_corrupt
    aof_rewrite
    rewrite_crash
    rewrite_preamble
    recovery_precedence
    auto_rewrite
    cow_cost
    model
)

foreach(case IN LISTS DAY17_TEST_CASES)
    add_test(NAME persistence_${case} COMMAND persistence_demo --case ${case})
endforeach()
