cmake_minimum_required(VERSION 3.20)

project(archdesign_day009 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
add_executable(mini_wal_demo main.cpp)
target_link_libraries(mini_wal_demo PRIVATE Threads::Threads)

if (MSVC)
    target_compile_options(mini_wal_demo PRIVATE /W4 /permissive-)
else()
    target_compile_options(mini_wal_demo PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
foreach(scenario IN ITEMS batching short-tail corrupt-record sync-failure)
    add_test(NAME wal_${scenario} COMMAND mini_wal_demo ${scenario})
    set_tests_properties(wal_${scenario} PROPERTIES TIMEOUT 20)
endforeach()
