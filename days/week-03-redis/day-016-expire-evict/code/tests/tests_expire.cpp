// tests_expire.cpp
//
// 主动过期：抽样清空、时间预算、比例反馈（过期比例低就收工）。

#include <string>

#include "test_support.hpp"

namespace archdesign::test {

void test_active_drain() {
    i64 now = kEpoch;
    Config cfg;
    cfg.hz = 10;
    Store s(cfg, Clock::manual(&now));

    check(s.set("keep", "forever"), "persistent key");
    for (int i = 0; i < 5000; ++i) {
        check(s.set(key_of(i), value_of(16)), "SET for active drain");
        check(s.expire_in(key_of(i), 1000) == 1, "EXPIRE for active drain");
    }
    now += 1500;
    check(s.expires_size() == 5000, "5000 keys carry a TTL");

    int cycles = 0;
    while (s.expires_size() > 0 && cycles < 500) {
        const auto result = s.active_expire_cycle();
        ++cycles;
        if (result.sampled == 0) break;
    }

    check(s.expires_size() == 0, "active expiration must drain the expires table");
    check(s.size() == 1, "only the persistent key survives");
    check(s.get("keep") == std::optional<std::string>("forever"), "persistent key intact");
    check(s.stats().active_expired == 5000, "all 5000 keys expired actively");
    check(s.stats().lazy_expired == 0, "no lookup was needed");
    check(cycles > 5 && cycles < 200, "draining 5000 keys takes a bounded number of cycles");
    check(s.memory_used() < 512, "memory returns to the one-key baseline");
}

void test_active_budget() {
    i64 now = kEpoch;
    Config cfg;
    Store s(cfg, Clock::manual(&now));

    for (int i = 0; i < 2000; ++i) {
        check(s.set(key_of(i), value_of(8)), "SET for budget test");
        check(s.expire_in(key_of(i), 1000) == 1, "EXPIRE for budget test");
    }
    now += 2000;

    const auto result = s.active_expire_cycle(0);
    check(result.time_limit_exit, "a zero budget must exit on the time limit");
    check(result.iterations == 1, "the budget is checked between iterations");
    check(result.sampled == 20, "one iteration samples 20 keys");
    check(result.expired == 20, "all sampled keys were expired");
    check(s.expires_size() == 1980, "only one iteration worth of keys was removed");
}

void test_active_ratio_gate() {
    i64 now = kEpoch;
    Config cfg;
    Store s(cfg, Clock::manual(&now));

    for (int i = 0; i < 10000; ++i) {
        check(s.set(key_of(i), value_of(8)), "SET alive key");
        check(s.expire_in(key_of(i), 1'000'000'000) == 1, "EXPIRE alive key");
    }
    for (int i = 0; i < 100; ++i) {
        check(s.set(key_of(i, "dead"), value_of(8)), "SET dead key");
        check(s.expire_in(key_of(i, "dead"), 1000) == 1, "EXPIRE dead key");
    }
    now += 2000;

    const auto result = s.active_expire_cycle(25000);
    check(result.iterations <= 3, "a ~1% stale ratio must stop after one or two iterations");
    check(result.sampled <= 60, "at most a few dozen samples");
    check(result.expired <= 5, "only a few stale keys are expected in 20-60 samples");
    check(s.size() >= 10050, "the vast majority of keys must survive");
}

}  // namespace archdesign::test
