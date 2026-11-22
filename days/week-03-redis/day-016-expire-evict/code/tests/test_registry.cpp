#include "test_support.hpp"

namespace archdesign::test {

const std::vector<TestCase>& test_cases() {
    static const std::vector<TestCase> cases = {
        {"semantics", test_semantics},
        {"lazy_get", test_lazy_get},
        {"lazy_write", test_lazy_write},
        {"active_drain", test_active_drain},
        {"active_budget", test_active_budget},
        {"active_ratio_gate", test_active_ratio_gate},
        {"evict_noeviction", test_evict_noeviction},
        {"evict_allkeys_lru", test_evict_allkeys_lru},
        {"evict_volatile_only", test_evict_volatile_only},
        {"evict_lfu", test_evict_lfu},
        {"evict_ttl", test_evict_ttl},
        {"replica_hidden", test_replica_hidden},
        {"pool_order", test_pool_order},
        {"model", test_model},
    };
    return cases;
}

}  // namespace archdesign::test
