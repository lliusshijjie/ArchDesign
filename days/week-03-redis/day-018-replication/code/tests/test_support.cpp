#include "test_support.hpp"

#include <stdexcept>

namespace archdesign::test {

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void require_same_kv(const Kv& left, const Kv& right, i64 now, const std::string& context) {
    check(left.size(now) == right.size(now),
          context + ": live size " + std::to_string(left.size(now)) + " vs " +
              std::to_string(right.size(now)));
    for (const auto& [key, value] : left.entries()) {
        const auto other = right.raw(key);
        check(other.has_value(), context + ": missing " + key);
        check(other->value == value.value, context + ": value mismatch for " + key);
        check(other->expire_at_ms == value.expire_at_ms, context + ": TTL mismatch for " + key);
    }
}

}  // namespace archdesign::test
