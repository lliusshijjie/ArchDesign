cmake_minimum_required(VERSION 3.20)

project(archdesign_workshop_a LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(archdesign_storage STATIC disk.cpp engine.cpp)
target_include_directories(archdesign_storage PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if (MSVC)
    target_compile_options(archdesign_storage PRIVATE /W4 /permissive-)
else()
    target_compile_options(archdesign_storage PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(workshop_a_harness harness.cpp)
target_link_libraries(workshop_a_harness PRIVATE archdesign_storage)

if (MSVC)
    target_compile_options(workshop_a_harness PRIVATE /W4 /permissive-)
else()
    target_compile_options(workshop_a_harness PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME workshop_a_harness COMMAND workshop_a_harness)
