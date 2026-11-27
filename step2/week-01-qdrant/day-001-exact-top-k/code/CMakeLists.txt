cmake_minimum_required(VERSION 3.20)
project(step2_exact_top_k LANGUAGES CXX)

add_executable(exact_top_k main.cpp top_k.cpp)
target_compile_features(exact_top_k PRIVATE cxx_std_20)
set_target_properties(exact_top_k PROPERTIES CXX_EXTENSIONS OFF)

enable_testing()
add_test(NAME candidate_recall COMMAND exact_top_k)
add_test(NAME exact_top_k_algorithms COMMAND exact_top_k --compare)
add_test(NAME filter_order COMMAND exact_top_k --check-filter-order)
add_test(NAME top_k_edge_cases COMMAND exact_top_k --check-edge-cases)
