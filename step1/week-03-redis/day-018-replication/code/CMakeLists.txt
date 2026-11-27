cmake_minimum_required(VERSION 3.20)

project(archdesign_day018 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# 复制机制拆成“协议 / 数据集 / 积压缓冲 / 主库 / 从库”五个模块。
add_library(day18_replication STATIC
    src/resp.cpp
    src/kv.cpp
    src/backlog.cpp
    src/master.cpp
    src/replica.cpp
)
target_include_directories(day18_replication PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(replication_demo
    main.cpp
    demo.cpp
    bench.cpp
    tests/test_support.cpp
    tests/test_registry.cpp
    tests/tests_full_sync.cpp
    tests/tests_partial.cpp
    tests/tests_wait_failover.cpp
)
target_link_libraries(replication_demo PRIVATE day18_replication)
target_include_directories(replication_demo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if (MSVC)
    target_compile_options(day18_replication PRIVATE /W4 /permissive-)
    target_compile_options(replication_demo PRIVATE /W4 /permissive-)
else()
    target_compile_options(day18_replication PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(replication_demo PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()

set(DAY18_TEST_CASES
    full_sync
    incremental
    full_sync_tail
    full_sync_ttl
    partial_resync
    backlog_overflow
    psync_boundary
    wrong_replid
    wait_acks
    wait_timeout
    failover_replid2
    master_driven_expire
    backlog_wrap
    command_stream
)

foreach(case IN LISTS DAY18_TEST_CASES)
    add_test(NAME replication_${case} COMMAND replication_demo --case ${case})
endforeach()
