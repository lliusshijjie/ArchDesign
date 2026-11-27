#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

using Coordinates = std::array<double, 2>;

struct Point {
    int id;
    Coordinates position;
};

struct Hit {
    int id;
    double squared_distance;

    bool operator==(const Hit&) const = default;
};

struct SearchResult {
    std::vector<Hit> hits;
    std::size_t distance_evaluations = 0;
    std::size_t peak_candidates = 0;
};

SearchResult top_k(std::span<const Point> points, Coordinates query, std::size_t k);

SearchResult top_k_heap(std::span<const Point> points, Coordinates query, std::size_t k);

std::optional<double> recall_at_k(std::span<const Hit> reference, std::span<const Hit> candidates);
