#include "top_k.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

constexpr std::array points{
    Point{11, {0.0, 0.0}},
    Point{22, {1.0, 0.0}},
    Point{33, {0.0, 2.0}},
    Point{44, {3.0, 0.0}},
    Point{55, {4.0, 0.0}},
};
constexpr Coordinates origin{0.0, 0.0};

void print_result(std::string_view label, const SearchResult& result) {
    std::cout << label;
    for (const auto& hit : result.hits) {
        std::cout << ' ' << hit.id;
    }
    std::cout << " (distance evaluations: " << result.distance_evaluations
              << ", peak candidates: " << result.peak_candidates << ")\n";
}

int run_demo() {
    constexpr std::size_t k = 3;

    const auto reference = top_k(points, origin, k);
    const auto limited = top_k_heap(std::span{points}.subspan(1), origin, k);
    const auto recall = recall_at_k(reference.hits, limited.hits);

    print_result("exact top 3:", reference);
    print_result("limited top 3:", limited);
    std::cout << "Recall@3: " << std::fixed << std::setprecision(4) << recall.value() << '\n';
    if (recall != 2.0 / 3.0 || limited.distance_evaluations != 4) {
        std::cerr << "Expected two reference hits and four candidate evaluations.\n";
        return 1;
    }
    return 0;
}

int compare_algorithms() {
    constexpr int point_count = 100'000;
    constexpr std::size_t k = 10;
    std::vector<Point> dataset;
    dataset.reserve(point_count);
    for (int id = point_count; id > 0; --id) {
        dataset.push_back({id, {static_cast<double>(id), 0.0}});
    }

    const auto sorted = top_k(dataset, origin, k);
    const auto bounded = top_k_heap(dataset, origin, k);
    print_result("full sort:", sorted);
    print_result("bounded heap:", bounded);

    if (sorted.hits != bounded.hits || bounded.hits.size() != k || bounded.hits.front().id != 1 ||
        bounded.hits.back().id != static_cast<int>(k) || sorted.peak_candidates != dataset.size() ||
        bounded.peak_candidates != k || sorted.distance_evaluations != dataset.size() ||
        bounded.distance_evaluations != dataset.size()) {
        std::cerr << "Expected identical results and distance counts, with only k candidates in the heap.\n";
        return 1;
    }
    return 0;
}

int check_filter_order() {
    constexpr std::size_t k = 3;
    constexpr std::array eligible{points[1], points[4]};
    const auto reference = top_k_heap(eligible, origin, k);
    auto post_filtered = top_k_heap(points, origin, k);
    std::erase_if(post_filtered.hits, [&eligible](const Hit& hit) {
        return std::ranges::none_of(eligible, [&](const Point& point) { return point.id == hit.id; });
    });

    print_result("filter before top 3:", reference);
    print_result("filter after top 3:", post_filtered);
    const auto recall = recall_at_k(reference.hits, post_filtered.hits);
    std::cout << "Filtered Recall@3: " << std::fixed << std::setprecision(4) << recall.value() << '\n';
    if (reference.hits.size() != eligible.size() || post_filtered.hits.size() != 1 || recall != 0.5) {
        std::cerr << "Expected post-filtering to miss one of the two eligible points.\n";
        return 1;
    }
    return 0;
}

int check_edge_cases() {
    constexpr std::array tied{Point{20, {-1.0, 0.0}}, Point{10, {1.0, 0.0}}};
    constexpr Coordinates query{0.0, 0.0};

    const auto reference = top_k(tied, query, 10);
    const auto best = top_k_heap(tied, query, 1);
    if (reference.hits.size() != tied.size() || reference.hits.front().id != 10 || best.hits.size() != 1 ||
        best.hits.front() != reference.hits.front() || top_k_heap(tied, query, 10).hits != reference.hits) {
        std::cerr << "Expected both points, ordered by ID at equal distance.\n";
        return 1;
    }

    const auto empty = top_k(tied, query, 0);
    const auto empty_heap = top_k_heap(tied, query, 0);
    if (!empty.hits.empty() || !empty_heap.hits.empty() || empty.distance_evaluations != 0 ||
        empty_heap.distance_evaluations != 0 || recall_at_k({}, {}).has_value()) {
        std::cerr << "Expected an empty top-k and an undefined recall for an empty reference.\n";
        return 1;
    }

    const std::array repeated{best.hits.front(), best.hits.front()};
    if (recall_at_k(reference.hits, {}) != 0.0 || recall_at_k(reference.hits, repeated) != 0.5) {
        std::cerr << "Expected zero recall for no hits and no credit for duplicates.\n";
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 1) {
        return run_demo();
    }

    const std::string_view option{argv[1]};
    if (option == "--compare") {
        return compare_algorithms();
    }
    if (option == "--check-filter-order") {
        return check_filter_order();
    }
    if (option == "--check-edge-cases") {
        return check_edge_cases();
    }
    std::cerr << "Unknown option: " << option << '\n';
    return 1;
}
