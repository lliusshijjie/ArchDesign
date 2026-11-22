// tests_replica_model.cpp
//
// 从库只隐藏不删除；以及 4000 步随机操作 + 参考模型的一致性检查。

#include <algorithm>
#include <map>
#include <optional>
#include <random>
#include <string>

#include "test_support.hpp"

namespace archdesign::test {

void test_replica_hidden() {
    i64 now = kEpoch;
    Config cfg;
    cfg.replica = true;
    Store s(cfg, Clock::manual(&now));

    check(s.set("k", "v"), "SET on replica");
    check(s.expire_in("k", 1000) == 1, "EXPIRE on replica");
    now += 2000;

    check(!s.get("k").has_value(), "an expired key is invisible on a replica");
    check(s.raw_contains("k"), "but the replica keeps the key in memory");
    check(s.raw_has_expire("k"), "and keeps the TTL metadata");
    check(s.memory_used() > 0, "memory is not freed yet");
    check(s.pttl("k") == -2, "PTTL hides it too");
    check(s.stats().replica_hidden >= 2, "each hidden access is counted");

    // 主库发来的 DEL 到达后才真正释放。
    check(s.del("k") == 1, "the master's DEL removes the key");
    check(s.memory_used() == 0, "memory is reclaimed by the DEL");
    check(s.size() == 0, "keyspace is empty again");
}

void test_model() {
    i64 now = kEpoch;
    Config cfg;
    cfg.seed = 7;
    Store s(cfg, Clock::manual(&now));

    struct ModelValue {
        std::string value;
        std::optional<i64> expire_at;
    };
    std::map<std::string, ModelValue> model;
    std::mt19937 rng(1234);

    auto lazy_expire_in_model = [&](const std::string& key) {
        auto it = model.find(key);
        if (it != model.end() && it->second.expire_at && now > *it->second.expire_at) {
            model.erase(it);
        }
    };

    for (int step = 0; step < 4000; ++step) {
        now += static_cast<i64>(rng() % 400);
        const std::string key = key_of(static_cast<int>(rng() % 120));
        const int op = static_cast<int>(rng() % 100);

        lazy_expire_in_model(key);  // 任何命令都会先触发惰性过期

        if (op < 40) {
            const std::string value =
                "v" + std::to_string(step % 5) +
                std::string(static_cast<std::size_t>(step % 9), 'x');
            check(s.set(key, value), "SET");
            model[key] = ModelValue{value, std::nullopt};
        } else if (op < 65) {
            const auto got = s.get(key);
            const auto it = model.find(key);
            if (it == model.end()) {
                check(!got.has_value(), "GET of a missing key");
            } else {
                check(got.has_value() && *got == it->second.value, "GET value mismatch");
            }
        } else if (op < 80) {
            const i64 ttl = static_cast<i64>(rng() % 2500) + 1;
            const auto it = model.find(key);
            const int expected = it == model.end() ? 0 : 1;
            check(s.expire_in(key, ttl) == expected, "EXPIRE result mismatch");
            if (it != model.end()) it->second.expire_at = now + ttl;
        } else if (op < 90) {
            auto it = model.find(key);
            const int expected = (it != model.end() && it->second.expire_at) ? 1 : 0;
            check(s.persist(key) == expected, "PERSIST result mismatch");
            if (expected == 1) it->second.expire_at = std::nullopt;
        } else if (op < 96) {
            const auto it = model.find(key);
            const int expected = it == model.end() ? 0 : 1;
            check(s.del(key) == expected, "DEL result mismatch");
            if (expected == 1) model.erase(key);
        } else {
            const i64 got = s.pttl(key);
            const auto it = model.find(key);
            if (it == model.end()) {
                check(got == -2, "PTTL of a missing key");
            } else if (!it->second.expire_at) {
                check(got == -1, "PTTL without TTL");
            } else {
                check(got >= 0 && got <= *it->second.expire_at - now, "PTTL out of range");
            }
        }

        if (step % 137 == 0) {
            (void)s.active_expire_cycle(2000);
            for (auto it = model.begin(); it != model.end();) {
                if (it->second.expire_at && now > *it->second.expire_at) {
                    it = model.erase(it);
                } else {
                    ++it;
                }
            }
        }

        std::string error;
        check(s.check_consistency(&error), "consistency: " + error);
        check(s.size() == model.size(), "keyspace size mismatch");
        const std::size_t ttl_count = static_cast<std::size_t>(
            std::count_if(model.begin(), model.end(),
                          [](const auto& kv) { return kv.second.expire_at.has_value(); }));
        check(s.expires_size() == ttl_count, "expires table size mismatch");
        check(s.memory_used() == s.memory_recomputed(), "memory counter drift");
    }
}

}  // namespace archdesign::test
