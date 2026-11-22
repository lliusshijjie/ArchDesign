cmake_minimum_required(VERSION 3.20)

project(archdesign_day017 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 有编解码和基准，默认 Release。
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_executable(persistence_demo main.cpp)

if (MSVC)
    target_compile_options(persistence_demo PRIVATE /W4 /permissive-)
else()
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
