// main.cpp
//
// Day 16 的行为测试、演示与基准实验：过期删除 + 内存淘汰。
//
// 编译运行：
//   cmake -S . -B build
//   cmake --build build
//   ctest --test-dir build --output-on-failure
//   ./build/expire_evict_demo                 # 跑全部测试
//   ./build/expire_evict_demo --demo          # 三条机制的讲解式输出
//   ./build/expire_evict_demo --bench         # 回收速率 / CPU / 淘汰质量
//   ./build/expire_evict_demo --case lazy_get # 单跑一个用例

#include "expire_evict.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace archdesign;

using WallClock = std::chrono::steady_clock;

constexpr i64 kEpoch = 1'700'000'000'000LL;  // 2023-11-14T22:13:20Z，随便挑的

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string key_of(int index, const char* prefix = "k") {
    return std::string(prefix) + std::to_string(index);
}

std::string value_of(std::size_t size) { return std::string(size, 'v'); }

std::size_t elapsed_ms(WallClock::time_point start) {
    return static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(WallClock::now() - start).count());
}

double as_mb(u64 bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

std::string human_bytes(u64 bytes) {
    if (bytes >= 1024ULL * 1024ULL) return std::to_string(bytes / (1024ULL * 1024ULL)) + " MB";
    if (bytes >= 1024ULL) return std::to_string(bytes / 1024ULL) + " KB";
    return std::to_string(bytes) + " B";
}

// ---------------------------------------------------------------------------
// 测试用例
// ---------------------------------------------------------------------------

// 1. 命令语义：TTL、PERSIST、SET 清 TTL、NX/XX/GT/LT、过去时间立刻删除。
void test_semantics() {
    i64 now = kEpoch;
    Config cfg;
    Store s(cfg, Clock::manual(&now));

    check(!s.get("missing").has_value(), "GET of a missing key");
    check(s.pttl("missing") == -2, "PTTL of a missing key");
    check(s.ttl("missing") == -2, "TTL of a missing key");

    check(s.set("k", "v0"), "SET must succeed");
    check(s.pttl("k") == -1, "a new key must have no TTL");
    check(s.expire_in("k", 5000) == 1, "EXPIRE must return 1");
    check(s.pttl("k") == 5000, "PTTL right after EXPIRE");
    check(s.ttl("k") == 5, "TTL rounds to 5 seconds");

    now += 1000;
    check(s.pttl("k") == 4000, "PTTL must count down");
    check(s.ttl("k") == 4, "TTL must count down");

    check(s.expire_in("k", 9000, ExpireFlags::Nx) == 0, "NX must not overwrite an existing TTL");
    check(s.expire_in("k", 9000, ExpireFlags::Xx) == 1, "XX must overwrite an existing TTL");
    check(s.expire_in("k", 12000, ExpireFlags::Gt) == 1, "GT with a later deadline");
    check(s.expire_in("k", 8000, ExpireFlags::Gt) == 0, "GT with an earlier deadline");
    check(s.expire_in("k", 8000, ExpireFlags::Lt) == 1, "LT with an earlier deadline");
    check(s.expire_in("k", 20000, ExpireFlags::Lt) == 0, "LT with a later deadline");
    check(s.expire_in("k", 1000, ExpireFlags::Nx | ExpireFlags::Gt) == -1,
          "NX and GT are mutually exclusive");

    check(s.persist("k") == 1, "PERSIST removes the TTL");
    check(s.persist("k") == 0, "PERSIST on a key without TTL");
    check(s.pttl("k") == -1, "no TTL after PERSIST");

    check(s.expire_in("k", 5000) == 1, "EXPIRE again");
    check(s.set("k", "v1"), "plain SET must succeed");
    check(s.pttl("k") == -1, "plain SET must clear the TTL");

    check(s.expire_in("k", 5000) == 1, "EXPIRE for KEEPTTL test");
    check(s.set("k", "v2", /*keep_ttl=*/true), "SET KEEPTTL must succeed");
    check(s.pttl("k") == 5000, "SET KEEPTTL must preserve the TTL");

    now += 6000;
    check(s.del("k") == 0, "DEL of an already-expired key returns 0, like Redis");

    check(s.set("gone", "v"), "SET for the past-deadline test");
    check(s.expire_in("gone", -1) == 1, "EXPIRE in the past returns 1");
    check(!s.exists("gone"), "EXPIRE in the past deletes immediately");

    check(s.set("round", "v"), "SET for TTL rounding");
    check(s.expire_in("round", 1900) == 1, "EXPIRE 1900ms");
    check(s.ttl("round") == 2, "1900ms must round to 2 seconds");
    check(s.expire_in("round", 1400) == 1, "EXPIRE 1400ms");
    check(s.ttl("round") == 1, "1400ms must round to 1 second");
}

// 2. 惰性删除：过期键只有被访问才消失；没人访问就一直占内存。
void test_lazy_get() {
    i64 now = kEpoch;
    Config cfg;
    Store s(cfg, Clock::manual(&now));

    for (int i = 0; i < 10; ++i) {
        check(s.set(key_of(i), value_of(32)), "SET for lazy test");
        check(s.expire_in(key_of(i), 1000) == 1, "EXPIRE for lazy test");
    }
    check(s.size() == 10, "10 keys before expiry");

    now += 1500;
    const u64 before = s.memory_used();
    check(!s.get("k0").has_value(), "an expired key must read as missing");
    check(s.size() == 9, "lazy expiration deletes the key");
    check(s.memory_used() < before, "lazy expiration frees memory");
    check(s.stats().lazy_expired == 1, "one lazy expiration");
    check(s.stats().expired_keys == 1, "expired_keys counts lazy deletions");

    check(!s.get("k0").has_value(), "the key stays missing");
    check(s.stats().lazy_expired == 1, "a second GET must not expire it again");
}

// 3. 写路径也检查过期：不能把过期键当成普通更新。
void test_lazy_write() {
    i64 now = kEpoch;
    Config cfg;
    Store s(cfg, Clock::manual(&now));

    check(s.set("k", "old"), "SET");
    check(s.expire_in("k", 1000) == 1, "EXPIRE");
    now += 2000;

    check(s.set("k", "new"), "overwriting an expired key");
    check(s.size() == 1, "overwrite must reuse the slot, not duplicate it");
    check(s.pttl("k") == -1, "a plain SET must not inherit the old TTL");
    check(s.get("k") == std::optional<std::string>("new"), "the new value must be visible");
}

// 4. 主动过期能把没人访问的过期键全部清掉。
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

// 5. 时间预算为 0 时，周期只做一轮就退出——预算是兜底，不是常态约束。
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

// 6. 抽样里过期比例低时立刻收工，不会为了几个过期键扫全表。
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

// 7. noeviction：宁可直接拒绝写入，也不淘汰数据。
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

// 8. allkeys-lru：冷 key 被淘汰，热 key 留下（采样是近似，但方向必须对）。
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

// 9. volatile-*：只在有 TTL 的 key 里淘汰；volatile 池空了就只能 OOM。
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

// 10. allkeys-lfu：被访问得多的 key，即使更老，也不该被淘汰。
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

// 11. volatile-ttl：剩余寿命最短的先走。
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

// 12. 从库不自己删过期键：逻辑上隐藏，物理上等主库的 DEL。
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

// 13. 淘汰池的排序/丢弃规则。
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

// 14. 随机操作 + 参考模型：语义、内存账、expires 表必须始终一致。
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
                "v" + std::to_string(step % 5) + std::string(static_cast<std::size_t>(step % 9), 'x');
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
        const std::size_t ttl_count =
            static_cast<std::size_t>(std::count_if(model.begin(), model.end(), [](const auto& kv) {
                return kv.second.expire_at.has_value();
            }));
        check(s.expires_size() == ttl_count, "expires table size mismatch");
        check(s.memory_used() == s.memory_recomputed(), "memory counter drift");
    }
}

struct TestCase {
    const char* name;
    void (*fn)();
};

const TestCase kCases[] = {
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

// ---------------------------------------------------------------------------
// 讲解式演示
// ---------------------------------------------------------------------------

void demo_lazy() {
    i64 now = kEpoch;
    Config cfg;
    Store s(cfg, Clock::manual(&now));

    std::cout << "\n== 演示 1：惰性删除 ==\n";
    for (int i = 0; i < 3; ++i) {
        s.set(key_of(i), value_of(16));
        s.expire_in(key_of(i), 1000);
    }
    std::cout << "插入 3 个 key，TTL=1s；2 秒后（没有人访问）：\n";
    now += 2000;
    std::cout << "  size=" << s.size() << "  expires=" << s.expires_size()
              << "  memory=" << human_bytes(s.memory_used()) << "  （全部过期，但都还在内存里）\n";
    (void)s.get("k0");
    std::cout << "GET k0 -> nil；这会顺手删掉它：size=" << s.size()
              << "  lazy_expired=" << s.stats().lazy_expired << "\n";
    std::cout << "结论：惰性删除只在有人访问时发生，没人访问的 key 会一直占着内存。\n";
}

void demo_active() {
    i64 now = kEpoch;
    Config cfg;
    Store s(cfg, Clock::manual(&now));

    std::cout << "\n== 演示 2：主动过期（cron 抽样）==\n";
    for (int i = 0; i < 2000; ++i) {
        s.set(key_of(i), value_of(16));
        s.expire_in(key_of(i), 1000);
    }
    now += 1500;

    std::cout << "插入 2000 个 TTL 已过期的 key，然后跑第一个周期：\n";
    int cycle = 0;
    while (s.expires_size() > 0 && cycle < 50) {
        const auto r = s.active_expire_cycle();
        ++cycle;
        if (cycle <= 3) {
            std::cout << "  cycle " << cycle << ": iterations=" << r.iterations
                      << " sampled=" << r.sampled << " expired=" << r.expired
                      << " elapsed=" << r.elapsed_us << "us\n";
        }
        if (r.sampled == 0) break;
    }
    std::cout << "共 " << cycle << " 个周期清空 2000 个 key；expires=" << s.expires_size()
              << "；每周期最多 16*20=320 个样本。\n";
    std::cout << "结论：回收速度上限 = 抽样率，不是 CPU 预算。\n";
}

void demo_evict() {
    i64 now = kEpoch;
    Config cfg;
    cfg.policy = Policy::AllKeysLru;
    cfg.maxmemory_samples = 5;
    cfg.maxmemory = 1LL << 40;
    Store s(cfg, Clock::manual(&now));

    std::cout << "\n== 演示 3：淘汰（采样近似 LRU）==\n";
    for (int i = 0; i < 100; ++i) {
        s.set(key_of(i), value_of(64));
    }
    for (int i = 0; i < 100; ++i) {
        now += 1000;
        if (i % 10 == 0) (void)s.get(key_of(i));  // 每 10 个里有一个是热的
    }
    const u64 used = s.memory_used();
    const auto r = s.perform_evictions(used / 2);
    std::cout << "100 个 key，一半内存要腾出来：evicted=" << r.evicted
              << "，耗时 " << r.elapsed_us << "us\n";
    int hot_survivors = 0;
    for (int i = 0; i < 100; i += 10) hot_survivors += s.raw_contains(key_of(i)) ? 1 : 0;
    std::cout << "10 个被反复访问的热 key 里，活着 " << hot_survivors
              << " 个；淘汰只是近似，不保证一个不误杀。\n";
}

// ---------------------------------------------------------------------------
// 基准实验
// ---------------------------------------------------------------------------

void bench_reclaim() {
    constexpr int kKeys = 100000;
    std::cout << "\n== 实验 1：10 万个过期键，惰性 vs 主动 ==\n";
    std::cout << "effort | 惰性访问1000次后剩余 | 主动清空周期 | 主动CPU耗时 | 模拟墙钟\n";
    std::cout << "-------+------------------------+--------------+-------------+---------\n";

    for (const int effort : {1, 10}) {
        i64 now = kEpoch;
        Config cfg;
        cfg.hz = 10;
        cfg.active_expire_effort = effort;
        cfg.seed = 11;
        Store s(cfg, Clock::manual(&now));

        for (int i = 0; i < kKeys; ++i) {
            s.set(key_of(i), value_of(64));
            s.expire_in(key_of(i), 60000);
        }
        now += 61000;
        const u64 full = s.memory_used();

        for (int i = 0; i < 1000; ++i) {
            (void)s.get(key_of((i * 97) % kKeys));
        }
        const u64 after_lazy = s.memory_used();
        const std::size_t lazy_remaining = s.expires_size();

        const auto start = WallClock::now();
        int cycles = 0;
        while (s.expires_size() > 0 && cycles < 100000) {
            const auto r = s.active_expire_cycle();
            ++cycles;
            if (r.sampled == 0) break;
        }
        const std::size_t drain_ms = elapsed_ms(start);
        const double simulated_seconds = static_cast<double>(cycles) * 0.1;  // hz=10

        std::cout << std::setw(5) << effort << "  | " << std::setw(12) << as_mb(after_lazy)
                  << " MB (" << lazy_remaining << " 剩余) | " << std::setw(9) << cycles
                  << "    | " << std::setw(7) << drain_ms << " ms  | " << std::setw(6)
                  << simulated_seconds << " s\n";
        check(full > after_lazy, "memory must drop as keys are reclaimed");
        check(lazy_remaining > 0, "lazy-only path must leave keys behind");
    }
    std::cout << "注：模拟墙钟 = 周期数 × 100ms（hz=10）。真实 Redis 还要和网络/命令抢 CPU。\n";
}

void bench_cpu() {
    std::cout << "\n== 实验 2：主动过期的 CPU 占用 ==\n";
    constexpr int kKeys = 200000;

    {
        i64 now = kEpoch;
        Config cfg;
        cfg.hz = 10;
        Store s(cfg, Clock::manual(&now));
        for (int i = 0; i < kKeys; ++i) {
            s.set(key_of(i), value_of(32));
            s.expire_in(key_of(i), i < 2000 ? 1000 : 1'000'000'000);
        }
        now += 2000;

        const auto start = WallClock::now();
        constexpr int kCycles = 2000;
        for (int i = 0; i < kCycles; ++i) (void)s.active_expire_cycle();
        const std::size_t total_us = static_cast<std::size_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(WallClock::now() - start).count());
        const double per_cycle = static_cast<double>(total_us) / kCycles;
        std::cout << "200k key，1% 已过期：每周期 " << std::fixed << std::setprecision(2)
                  << per_cycle << "us，占 100ms 周期的 " << std::setprecision(4)
                  << (per_cycle / 100000.0 * 100.0) << "%（预算 25ms）\n";
    }

    {
        i64 now = kEpoch;
        Config cfg;
        cfg.hz = 10;
        Store s(cfg, Clock::manual(&now));
        for (int i = 0; i < kKeys; ++i) {
            s.set(key_of(i), value_of(32));
            s.expire_in(key_of(i), 1000);
        }
        now += 2000;
        const auto start = WallClock::now();
        const auto r = s.active_expire_cycle();
        const auto us = static_cast<std::size_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(WallClock::now() - start)
                .count());
        std::cout << "200k key，全部过期：单周期删 " << r.expired << " 个，耗时 " << us
                  << "us（预算 25ms，只用了 " << std::fixed << std::setprecision(2)
                  << (static_cast<double>(us) / 25000.0 * 100.0) << "%）\n";
        std::cout << "结论：常态下 320 次抽样远没到 25ms，真正限制回收速度的是抽样数。\n";
    }
}

void bench_evict() {
    std::cout << "\n== 实验 3：采样淘汰的精度（20k key，随机访问 + 500 个热 key）==\n";
    std::cout << "场景          | 采样 | 淘汰数 | 命中真冷20% | 平均分位 | 热类误杀\n";
    std::cout << "--------------+------+--------+-------------+----------+---------\n";

    struct Scenario {
        const char* name;
        Policy policy;
        int samples;
    };
    const Scenario scenarios[] = {
        {"allkeys-lru", Policy::AllKeysLru, 5},
        {"allkeys-lru", Policy::AllKeysLru, 16},
        {"allkeys-lru", Policy::AllKeysLru, 64},
        {"allkeys-lfu", Policy::AllKeysLfu, 5},
        {"allkeys-lfu", Policy::AllKeysLfu, 16},
        {"allkeys-rand", Policy::AllKeysRandom, 0},
    };

    constexpr int kKeys = 20000;
    constexpr int kHot = 500;
    constexpr int kRounds = 60;
    constexpr int kVictims = kKeys / 5;

    for (const auto& scenario : scenarios) {
        i64 now = kEpoch;
        Config cfg;
        cfg.policy = scenario.policy;
        cfg.maxmemory_samples = std::max(1, scenario.samples);
        cfg.maxmemory = 1LL << 40;
        cfg.seed = 99;
        Store s(cfg, Clock::manual(&now));

        std::vector<i64> last_tick(kKeys, 0);
        i64 access_tick = 0;
        for (int i = 0; i < kKeys; ++i) {
            s.set(key_of(i), value_of(48));
        }

        std::mt19937 rng(777);  // 每个场景同样的访问序列
        for (int round = 0; round < kRounds; ++round) {
            now += 1000;
            for (int n = 0; n < 3000; ++n) {  // 每轮随机访问 3000 个 key
                const int i = static_cast<int>(rng() % kKeys);
                (void)s.get(key_of(i));
                last_tick[i] = ++access_tick;
            }
            for (int i = 0; i < kHot; ++i) {  // 热 key 每轮都访问
                (void)s.get(key_of(i));
                last_tick[i] = ++access_tick;
            }
        }

        // 真实 LRU 顺序：按最后一次访问的逻辑时刻排名（tick 唯一，没有并列）。
        std::vector<int> order(kKeys);
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(),
                  [&](int a, int b) { return last_tick[a] < last_tick[b]; });
        std::vector<int> rank(kKeys, 0);
        for (int pos = 0; pos < kKeys; ++pos) rank[order[pos]] = pos;

        const u64 used = s.memory_used();
        const auto result = s.perform_evictions(used / 5);

        int evicted = 0;
        int cold_hits = 0;
        int hot_hits = 0;
        double rank_sum = 0.0;
        for (int i = 0; i < kKeys; ++i) {
            if (s.raw_contains(key_of(i))) continue;
            ++evicted;
            cold_hits += (rank[i] < kVictims) ? 1 : 0;
            hot_hits += (i < kHot) ? 1 : 0;
            rank_sum += 100.0 * rank[i] / kKeys;
        }
        const double cold_pct = evicted > 0 ? 100.0 * cold_hits / evicted : 0.0;
        const double hot_pct = evicted > 0 ? 100.0 * hot_hits / evicted : 0.0;
        const double mean_rank = evicted > 0 ? rank_sum / evicted : 0.0;

        std::cout << std::left << std::setw(13) << scenario.name << " | " << std::right
                  << std::setw(4) << scenario.samples << " | " << std::setw(6) << evicted << " | "
                  << std::setw(11) << std::fixed << std::setprecision(1) << cold_pct << "% | "
                  << std::setw(8) << mean_rank << "% | " << std::setw(8) << hot_pct << "%\n";
        check(result.ok, "eviction should reach the target");
    }
    std::cout << "注：精确 LRU 应命中真冷 20% 的 100%、平均分位 10%；随机策略约 20% / 50%。\n";
}

// ---------------------------------------------------------------------------
// 调度
// ---------------------------------------------------------------------------

const TestCase* find_case(const std::string& name) {
    for (const auto& test : kCases) {
        if (name == test.name) return &test;
    }
    return nullptr;
}

int run_named(const std::string& name) {
    const TestCase* test = find_case(name);
    if (test == nullptr) {
        std::cerr << "unknown case: " << name << "\n";
        return 2;
    }
    try {
        test->fn();
        std::cout << "[PASS] " << test->name << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << test->name << ": " << e.what() << "\n";
        return 1;
    }
}

int run_all() {
    int failed = 0;
    for (const auto& test : kCases) {
        try {
            test.fn();
            std::cout << "[PASS] " << test.name << "\n";
        } catch (const std::exception& e) {
            ++failed;
            std::cerr << "[FAIL] " << test.name << ": " << e.what() << "\n";
        }
    }
    std::cout << "\n" << (std::size(kCases) - static_cast<std::size_t>(failed)) << "/"
              << std::size(kCases) << " cases passed\n";
    return failed == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        const std::string arg = argv[1];
        if (arg == "--case" && argc > 2) return run_named(argv[2]);
        if (arg == "--demo") {
            demo_lazy();
            demo_active();
            demo_evict();
            return 0;
        }
        if (arg == "--bench") {
            bench_reclaim();
            bench_cpu();
            bench_evict();
            return 0;
        }
        std::cerr << "usage: " << argv[0] << " [--case NAME | --demo | --bench]\n";
        return 2;
    }
    return run_all();
}
