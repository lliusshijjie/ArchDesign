#include "top_k.hpp"

#include <array>
#include <iomanip>
#include <iostream>
#include <string_view>

namespace {

void print_result(std::string_view label, const SearchResult& result) {
    std::cout << label;
    for (const auto& hit : result.hits) {
        std::cout << ' ' << hit.id;
    }
    std::cout << " (distance evaluations: " << result.distance_evaluations << ")\n";
}

int run_demo() {
    constexpr std::array points{
        Point{11, {0.0, 0.0}},
        Point{22, {1.0, 0.0}},
        Point{33, {0.0, 2.0}},
        Point{44, {3.0, 0.0}},
        Point{55, {4.0, 0.0}},
    };
    constexpr Coordinates query{0.0, 0.0};
    constexpr std::size_t k = 3;

    const auto reference = top_k(points, query, k);
    const auto limited = top_k(std::span{points}.subspan(1), query, k);
    const auto recall = recall_at_k(reference.hits, limited.hits);

    print_result("exact top 3:", reference);
    print_result("limited top 3:", limited);
    std::cout << "Recall@3: " << std::fixed << std::setprecision(4) << recall.value() << '\n';
    return 0;
}

int check_edge_cases() {
    constexpr std::array tied{Point{20, {-1.0, 0.0}}, Point{10, {1.0, 0.0}}};
    constexpr Coordinates query{0.0, 0.0};

    const auto reference = top_k(tied, query, 10);
    if (reference.hits.size() != tied.size() || reference.hits.front().id != 10) {
        std::cerr << "Expected both points, ordered by ID at equal distance.\n";
        return 1;
    }

    const auto empty = top_k(tied, query, 0);
    if (!empty.hits.empty() || empty.distance_evaluations != 0 || recall_at_k({}, {}).has_value()) {
        std::cerr << "Expected an empty top-k and an undefined recall for an empty reference.\n";
        return 1;
    }

    if (recall_at_k(reference.hits, {}) != 0.0) {
        std::cerr << "Expected zero recall when all reference points are missed.\n";
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view{argv[1]} == "--check-edge-cases") {
        return check_edge_cases();
    }
    return run_demo();
}
