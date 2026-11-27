cmake_minimum_required(VERSION 3.20)

project(archdesign_day014 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(mini_kv_demo main.cpp)

if (MSVC)
    target_compile_options(mini_kv_demo PRIVATE /W4 /permissive-)
else()
    target_compile_options(mini_kv_demo PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME mini_kv_demo COMMAND mini_kv_demo)
