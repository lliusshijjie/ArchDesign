// tests_evict.cpp
//
// 淘汰策略：noeviction、allkeys-lru、volatile-*、allkeys-lfu、volatile-ttl，
// 以及淘汰池的排序/丢弃规则。

#include <optional>
#include <string>

#include "test_support.hpp"

namespace archdesign::test {

void test_evict_noeviction() {
    i64 now = kEpoch;
    Config cfg;
    cfg.policy = Policy::NoEviction;
    cfg.maxmemory = 8192;
    Store s(cfg, Clock::manual(&now));

    check(s.set("keep", "v"), "the first SET fits");
    bool failed = false;
    for (int i = 0; i < 1000 && !failed; ++i) {
        const u64 before = s.memory_used();
        if (!s.set(key_of(i, "big"), value_of(300))) {
            failed = true;
            check(s.memory_used() == before, "a rejected SET must not change memory");
        }
    }
    check(failed, "writes must eventually be rejected");
    check(s.stats().oom_errors == 1, "one OOM error");
    check(s.stats().evicted_keys == 0, "noeviction must not evict anything");
    check(s.get("keep") == std::optional<std::string>("v"), "existing data stays");
}

void test_evict_allkeys_lru() {
    i64 now = kEpoch;
    Config cfg;
    cfg.policy = Policy::AllKeysLru;
    cfg.maxmemory_samples = 50;
    Store s(cfg, Clock::manual(&now));

    for (int i = 0; i < 50; ++i) {
        check(s.set(key_of(i), value_of(32)), "SET for LRU test");
    }
    // 每访问一个 key 就推进 1 秒：k0 最冷，k49 最热。
    for (int i = 0; i < 50; ++i) {
        now += 1000;
        check(s.get(key_of(i)).has_value(), "touch key");
    }

    const u64 used = s.memory_used();
    s.set_maxmemory(used - 1);
    const auto result = s.perform_evictions(1);
    check(result.ok, "one eviction must be enough to get under the limit");
    check(result.evicted == 1, "exactly one key leaves");
    check(!s.raw_contains("k0"), "the coldest key must be evicted");
    check(s.raw_contains("k49"), "the hottest key must survive");
}

void test_evict_volatile_only() {
    i64 now = kEpoch;
    Config cfg;
    cfg.policy = Policy::VolatileLru;
    cfg.maxmemory_samples = 40;
    cfg.maxmemory = 1'000'000'000;  // 只为让 perform_evictions 不早退
    Store s(cfg, Clock::manual(&now));

    for (int i = 0; i < 20; ++i) {
        check(s.set(key_of(i, "v"), value_of(32)), "SET volatile key");
        check(s.expire_in(key_of(i, "v"), 100'000) == 1, "EXPIRE volatile key");
    }
    for (int i = 0; i < 20; ++i) {
        check(s.set(key_of(i, "p"), value_of(32)), "SET persistent key");
    }

    const auto first = s.perform_evictions(s.memory_used() / 2);
    check(first.evicted >= 10, "about half the keys must be evicted");
    check(s.stats().evicted_keys == first.evicted, "eviction stats");
    for (int i = 0; i < 20; ++i) {
        check(s.raw_contains(key_of(i, "p")), "persistent keys must never be evicted");
    }
    check(s.expires_size() == 20 - first.evicted, "only volatile keys were removed");

    const auto second = s.perform_evictions(s.memory_used());
    check(!second.ok, "once the volatile pool is empty the eviction fails");
    check(second.no_candidates, "the failure reason is 'no candidate'");
    check(s.size() == 20, "the 20 persistent keys are still there");
}

void test_evict_lfu() {
    i64 now = kEpoch;
    Config cfg;
    cfg.policy = Policy::AllKeysLfu;
    cfg.maxmemory_samples = 40;
    cfg.maxmemory = 1'000'000'000;
    cfg.seed = 3;
    Store s(cfg, Clock::manual(&now));

    for (int i = 0; i < 40; ++i) {
        check(s.set(key_of(i), value_of(32)), "SET for LFU test");
    }
    for (int i = 0; i < 200; ++i) {
        now += 10;
        check(s.get("k0").has_value(), "hot key access");
    }
    const auto hot = s.object_freq("k0");
    check(hot.has_value() && *hot > 5, "the hot key's counter must have grown");

    now += 3 * 60 * 1000;  // 3 分钟：counter 按 lfu-decay-time=1 衰减
    const auto hot_decayed = s.object_freq("k0");
    const auto cold = s.object_freq("k1");
    check(cold.has_value() && *cold == 2, "an untouched counter decays to 2 after 3 minutes");
    check(hot_decayed.has_value() && *hot_decayed > *cold, "the hot key keeps a higher counter");

    const auto result = s.perform_evictions(1);
    check(result.ok && result.evicted == 1, "one key is evicted");
    check(s.raw_contains("k0"), "the frequently used key must survive");
    check(s.size() == 39, "exactly one key is gone");
}

void test_evict_ttl() {
    i64 now = kEpoch;
    Config cfg;
    cfg.policy = Policy::VolatileTtl;
    cfg.maxmemory_samples = 20;
    cfg.maxmemory = 1'000'000'000;
    Store s(cfg, Clock::manual(&now));

    for (int i = 0; i < 20; ++i) {
        check(s.set(key_of(i), value_of(32)), "SET for TTL policy");
        check(s.expire_in(key_of(i), 10'000 * (i + 1)) == 1, "EXPIRE for TTL policy");
    }
    now += 1000;  // 让所有 key 都还活着

    const auto result = s.perform_evictions(1);
    check(result.ok && result.evicted == 1, "one key is evicted");
    check(!s.raw_contains("k0"), "the key with the shortest TTL must go first");
    check(s.raw_contains("k19"), "the key with the longest TTL must stay");
}

void test_pool_order() {
    EvictionPool pool;
    for (u64 i = 0; i < 25; ++i) {
        pool.insert("k" + std::to_string(i), i);
    }
    check(pool.count() == EvictionPool::kCapacity, "the pool is capped at 16 entries");
    for (int i = 24; i >= 9; --i) {
        const auto item = pool.pop_best();
        check(item.has_value(), "the pool is not empty yet");
        check(item->key == "k" + std::to_string(i), "pop_best must return the largest idle");
        check(item->idle == static_cast<u64>(i), "idle scores stay sorted");
    }
    check(!pool.pop_best().has_value(), "the pool is drained");

    // 池满时，比池里最小的还小的候选直接丢弃。
    EvictionPool full;
    for (u64 i = 100; i < 116; ++i) full.insert("x" + std::to_string(i), i);
    full.insert("too-small", 3);
    check(full.count() == 16, "still 16 entries");
    check(full.rejected() == 1, "the too-small candidate was rejected");
    check(full.pop_best()->idle == 115, "the best candidate is still the top");
}

}  // namespace archdesign::test
