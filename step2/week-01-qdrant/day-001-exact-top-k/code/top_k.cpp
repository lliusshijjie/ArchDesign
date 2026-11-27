#include "top_k.hpp"

#include <algorithm>
#include <tuple>

SearchResult top_k(std::span<const Point> points, Coordinates query, std::size_t k) {
    SearchResult result;
    if (k == 0) {
        return result;
    }

    result.hits.reserve(points.size());
    for (const auto& point : points) {
        const auto dx = point.position[0] - query[0];
        const auto dy = point.position[1] - query[1];
        result.hits.push_back({point.id, dx * dx + dy * dy});
    }
    result.distance_evaluations = result.hits.size();

    std::ranges::sort(result.hits, {}, [](const Hit& hit) {
        return std::tie(hit.squared_distance, hit.id);
    });
    result.hits.resize(std::min(k, result.hits.size()));
    return result;
}

std::optional<double> recall_at_k(std::span<const Hit> reference, std::span<const Hit> candidates) {
    if (reference.empty()) {
        return std::nullopt;
    }

    const auto matches = std::ranges::count_if(reference, [&](const Hit& expected) {
        return std::ranges::any_of(candidates, [&](const Hit& actual) {
            return expected.id == actual.id;
        });
    });
    return static_cast<double>(matches) / static_cast<double>(reference.size());
}
