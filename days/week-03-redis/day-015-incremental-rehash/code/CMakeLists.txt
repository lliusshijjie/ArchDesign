cmake_minimum_required(VERSION 3.20)

project(archdesign_day015 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 默认 Release：这个例子里有测量停顿的演示，-O0 的数字没有参考价值。
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_executable(dict_demo main.cpp)

if (MSVC)
    target_compile_options(dict_demo PRIVATE /W4 /permissive-)
else()
    target_compile_options(dict_demo PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()

# 每个用例单独注册，这样 ctest 能直接指出坏在哪一条不变量上。
set(DICT_TEST_CASES
    basic
    growth
    no_stall
    lookup_both_tables
    insert_new_table
    erase_both_tables
    shrink
    time_budget
    scan_stable
    scan_during_rehash
    model
)

foreach(case IN LISTS DICT_TEST_CASES)
    add_test(NAME dict_${case} COMMAND dict_demo --case ${case})
endforeach()
