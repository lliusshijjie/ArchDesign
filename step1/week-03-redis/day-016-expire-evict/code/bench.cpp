// bench.cpp
//
// 三组小基准：回收速率、主动过期 CPU、采样淘汰精度。

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "tests/test_support.hpp"

namespace archdesign::test {

namespace {

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
            std::chrono::duration_cast<std::chrono::microseconds>(WallClock::now() - start)
                .count());
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

}  // namespace

void run_bench() {
    bench_reclaim();
    bench_cpu();
    bench_evict();
}

}  // namespace archdesign::test
