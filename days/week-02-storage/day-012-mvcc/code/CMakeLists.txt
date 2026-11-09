cmake_minimum_required(VERSION 3.20)

project(archdesign_day012 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(mvcc_demo main.cpp)

if (MSVC)
    target_compile_options(mvcc_demo PRIVATE /W4 /permissive-)
else()
    target_compile_options(mvcc_demo PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME mvcc_demo COMMAND mvcc_demo)
