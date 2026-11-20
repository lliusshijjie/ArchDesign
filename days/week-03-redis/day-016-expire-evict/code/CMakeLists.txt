cmake_minimum_required(VERSION 3.20)

project(archdesign_day016 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 有测量和基准，默认 Release。
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_executable(expire_evict_demo main.cpp)

if (MSVC)
    target_compile_options(expire_evict_demo PRIVATE /W4 /permissive-)
else()
    target_compile_options(expire_evict_demo PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()

# 每个用例单独注册：坏在哪条不变量上，ctest 直接报名字。
set(DAY16_TEST_CASES
    semantics
    lazy_get
    lazy_write
    active_drain
    active_budget
    active_ratio_gate
    evict_noeviction
    evict_allkeys_lru
    evict_volatile_only
    evict_lfu
    evict_ttl
    replica_hidden
    pool_order
    model
)

foreach(case IN LISTS DAY16_TEST_CASES)
    add_test(NAME expire_${case} COMMAND expire_evict_demo --case ${case})
endforeach()
