#include "test_support.hpp"

#include <stdexcept>

namespace archdesign::test {

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string key_of(int index, const char* prefix) {
    return std::string(prefix) + std::to_string(index);
}

std::string value_of(int index, std::size_t size) {
    std::string value = "v" + std::to_string(index) + ":";
    while (value.size() < size) value.push_back('x');
    return value;
}

EngineConfig base_config(bool aof_enabled, std::size_t pages) {
    EngineConfig config;
    config.aof_enabled = aof_enabled;
    config.page_count = pages;
    config.auto_rewrite_min_size = 4096;
    return config;
}

void seed(Engine& engine, int count, i64 now, i64 ttl_ms) {
    for (int i = 0; i < count; ++i) engine.set(key_of(i), value_of(i), now, ttl_ms);
}

std::optional<Entry> snapshot_key(const Snapshot& snapshot, std::string_view wanted) {
    std::optional<Entry> found;
    snapshot.for_each([&](const std::string& key, const Entry& entry) {
        if (key == wanted) found = entry;
    });
    return found;
}

void require_same_live(const PageStore& left, const PageStore& right, i64 now,
                       const std::string& context) {
    check(left.live_key_count(now) == right.live_key_count(now),
          context + ": live key count differs (" + std::to_string(left.live_key_count(now)) +
              " vs " + std::to_string(right.live_key_count(now)) + ")");
    for (const std::string& key : left.keys()) {
        const auto a = left.get_live(key, now);
        const auto b = right.get_live(key, now);
        check(a.has_value() == b.has_value(), context + ": presence differs for " + key);
        if (!a) continue;
        check(a->value == b->value, context + ": value differs for " + key);
        check(a->expire_at_ms == b->expire_at_ms, context + ": TTL differs for " + key);
    }
}

}  // namespace archdesign::test
