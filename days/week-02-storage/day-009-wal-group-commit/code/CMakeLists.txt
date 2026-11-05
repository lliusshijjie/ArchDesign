cmake_minimum_required(VERSION 3.20)

project(archdesign_day009 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(mini_wal_demo main.cpp)

if (MSVC)
    target_compile_options(mini_wal_demo PRIVATE /W4 /permissive-)
else()
    target_compile_options(mini_wal_demo PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
foreach(scenario IN ITEMS batching short-tail corrupt-record sync-failure)
    add_test(NAME wal_${scenario} COMMAND mini_wal_demo ${scenario})
endforeach()
