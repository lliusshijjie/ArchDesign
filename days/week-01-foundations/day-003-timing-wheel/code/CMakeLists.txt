cmake_minimum_required(VERSION 3.20)

project(archdesign_day003 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_executable(timing_wheel_demo main.cpp)
target_link_libraries(timing_wheel_demo PRIVATE Threads::Threads)

if (MSVC)
    target_compile_options(timing_wheel_demo PRIVATE /W4 /permissive-)
else()
    target_compile_options(timing_wheel_demo PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME timing_wheel_demo COMMAND timing_wheel_demo)
