cmake_minimum_required(VERSION 3.20)

project(archdesign_day019 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# 槽/CRC、拓扑、路由、迁移、故障转移、客户端分别成模块。
add_library(day19_cluster STATIC
    src/crc16.cpp
    src/slot_map.cpp
    src/cluster_core.cpp
    src/cluster_routing.cpp
    src/cluster_failover.cpp
    src/client.cpp
)
target_include_directories(day19_cluster PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(cluster_demo
    main.cpp
    demo.cpp
    bench.cpp
    tests/test_support.cpp
    tests/test_registry.cpp
    tests/tests_slots.cpp
    tests/tests_redirect.cpp
    tests/tests_failover.cpp
)
target_link_libraries(cluster_demo PRIVATE day19_cluster)
target_include_directories(cluster_demo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if (MSVC)
    target_compile_options(day19_cluster PRIVATE /W4 /permissive-)
    target_compile_options(cluster_demo PRIVATE /W4 /permissive-)
else()
    target_compile_options(day19_cluster PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(cluster_demo PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()

set(DAY19_TEST_CASES
    slot_hash
    slot_map
    moved
    client_cache
    crossslot
    clusterdown
    ask_migration
    tryagain
    ask_target_requires_asking
    gossip_quorum
    failover
    failover_no_quorum
    epoch_vote_once
    replica_data
)

foreach(case IN LISTS DAY19_TEST_CASES)
    add_test(NAME cluster_${case} COMMAND cluster_demo --case ${case})
endforeach()
