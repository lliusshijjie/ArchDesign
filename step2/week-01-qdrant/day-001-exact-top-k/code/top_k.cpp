#include "top_k.hpp"

#include <algorithm>
#include <tuple>

namespace {

Hit score(const Point& point, Coordinates query) {
    const auto dx = point.position[0] - query[0];
    const auto dy = point.position[1] - query[1];
    return {point.id, dx * dx + dy * dy};
}

bool is_better(const Hit& left, const Hit& right) {
    return std::tie(left.squared_distance, left.id) < std::tie(right.squared_distance, right.id);
}

} // namespace

SearchResult top_k(std::span<const Point> points, Coordinates query, std::size_t k) {
    SearchResult result;
    if (k == 0) {
        return result;
    }

    result.hits.reserve(points.size());
    for (const auto& point : points) {
        result.hits.push_back(score(point, query));
    }
    result.distance_evaluations = result.hits.size();
    result.peak_candidates = result.hits.size();

    std::ranges::sort(result.hits, is_better);
    result.hits.resize(std::min(k, result.hits.size()));
    return result;
}

SearchResult top_k_heap(std::span<const Point> points, Coordinates query, std::size_t k) {
    SearchResult result;
    if (k == 0) {
        return result;
    }

    auto& heap = result.hits;
    heap.reserve(std::min(k, points.size()));
    for (const auto& point : points) {
        const auto hit = score(point, query);
        if (heap.size() < k) {
            heap.push_back(hit);
            std::ranges::push_heap(heap, is_better);
        } else if (is_better(hit, heap.front())) {
            std::ranges::pop_heap(heap, is_better);
            heap.back() = hit;
            std::ranges::push_heap(heap, is_better);
        }
    }

    result.distance_evaluations = points.size();
    result.peak_candidates = heap.size();
    std::ranges::sort_heap(heap, is_better);
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
