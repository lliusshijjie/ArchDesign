// demo.cpp
//
// 三条机制的讲解式输出：惰性删除、主动过期、采样淘汰。

#include <iomanip>
#include <iostream>
#include <string>

#include "tests/test_support.hpp"

namespace archdesign::test {

void run_demo() {
    {
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
                  << "  memory=" << human_bytes(s.memory_used())
                  << "  （全部过期，但都还在内存里）\n";
        (void)s.get("k0");
        std::cout << "GET k0 -> nil；这会顺手删掉它：size=" << s.size()
                  << "  lazy_expired=" << s.stats().lazy_expired << "\n";
        std::cout << "结论：惰性删除只在有人访问时发生，没人访问的 key 会一直占着内存。\n";
    }

    {
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

    {
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
}

}  // namespace archdesign::test
