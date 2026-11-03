cmake_minimum_required(VERSION 3.20)
project(archdesign_day007 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Threads REQUIRED)

# 直接复用前六课的实现；本实验不复制或改写它们。
set(LESSON_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
add_library(executor_lab_core lab.cpp)
target_include_directories(executor_lab_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${LESSON_ROOT}/day-001-thread-pool/code"
    "${LESSON_ROOT}/day-002-redis-event-loop/code"
    "${LESSON_ROOT}/day-003-timing-wheel/code"
    "${LESSON_ROOT}/day-004-async-logging/code"
    "${LESSON_ROOT}/day-005-memory-allocator/code"
    "${LESSON_ROOT}/day-006-lock-free-ring/code")
target_link_libraries(executor_lab_core PUBLIC Threads::Threads)
add_executable(executor_lab main.cpp scenarios.cpp)
target_link_libraries(executor_lab PRIVATE executor_lab_core)

foreach(target executor_lab_core executor_lab)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endforeach()

enable_testing()
foreach(scenario normal ingress overload deadline slow-log failure-shutdown)
    add_test(NAME lab_${scenario} COMMAND executor_lab --scenario ${scenario})
    set_tests_properties(lab_${scenario} PROPERTIES TIMEOUT 15)
endforeach()
add_test(NAME lab_sample_input COMMAND executor_lab --input "${CMAKE_CURRENT_SOURCE_DIR}/../data/requests.txt")
set_tests_properties(lab_sample_input PROPERTIES TIMEOUT 15)
