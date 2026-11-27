cmake_minimum_required(VERSION 3.20)

project(archdesign_day010 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(buffer_pool_demo main.cpp)

if (MSVC)
    target_compile_options(buffer_pool_demo PRIVATE /W4 /permissive-)
else()
    target_compile_options(buffer_pool_demo PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME buffer_pool_demo COMMAND buffer_pool_demo)
