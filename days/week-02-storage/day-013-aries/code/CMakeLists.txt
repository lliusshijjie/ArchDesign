cmake_minimum_required(VERSION 3.20)

project(archdesign_day013 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(aries_demo main.cpp)

if (MSVC)
    target_compile_options(aries_demo PRIVATE /W4 /permissive-)
else()
    target_compile_options(aries_demo PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME aries_demo COMMAND aries_demo)
