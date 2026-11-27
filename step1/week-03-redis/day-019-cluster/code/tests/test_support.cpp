#include "test_support.hpp"

#include <stdexcept>

namespace archdesign::test {

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

Cluster three_masters() {
    Cluster cluster;
    cluster.add_master("M1");
    cluster.add_master("M2");
    cluster.add_master("M3");
    cluster.assign_range(0, 5460, "M1");
    cluster.assign_range(5461, 10922, "M2");
    cluster.assign_range(10923, 16383, "M3");
    return cluster;
}

std::string key_owned_by(const Cluster& cluster, const std::string& node,
                         const std::string& prefix) {
    for (int i = 0; i < 200000; ++i) {
        std::string key = prefix + std::to_string(i);
        if (cluster.slots().owner(key_hash_slot(key)) == node) return key;
    }
    throw std::runtime_error("no key found for node " + node);
}

std::string key_in_range(Slot begin, Slot end, const std::string& prefix) {
    for (int i = 0; i < 200000; ++i) {
        std::string key = prefix + std::to_string(i);
        const Slot slot = key_hash_slot(key);
        if (slot >= begin && slot <= end) return key;
    }
    throw std::runtime_error("no key found in range");
}

}  // namespace archdesign::test
