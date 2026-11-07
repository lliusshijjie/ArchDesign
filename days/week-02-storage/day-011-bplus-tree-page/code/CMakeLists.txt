cmake_minimum_required(VERSION 3.20)

project(archdesign_day011 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(slotted_page_demo main.cpp)

if (MSVC)
    target_compile_options(slotted_page_demo PRIVATE /W4 /permissive-)
else()
    target_compile_options(slotted_page_demo PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME slotted_page_demo COMMAND slotted_page_demo)
