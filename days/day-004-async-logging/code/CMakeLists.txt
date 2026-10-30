cmake_minimum_required(VERSION 3.20)

project(archdesign_day004 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_executable(async_logger_demo main.cpp)
target_link_libraries(async_logger_demo PRIVATE Threads::Threads)

if (MSVC)
    target_compile_options(async_logger_demo PRIVATE /W4 /permissive-)
else()
    target_compile_options(async_logger_demo PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME async_logger_demo COMMAND async_logger_demo)
