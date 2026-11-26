// bench.cpp
//
// 小基准：哈希槽计算吞吐、三主槽分布、客户端路由跳数。

#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <string>

#include "tests/test_support.hpp"

namespace archdesign::test {

namespace {
using Clock = std::chrono::steady_clock;

std::size_t elapsed_us(Clock::time_point start) {
    return static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
}
}  // namespace

void run_bench() {
    constexpr int kKeys = 1'000'000;

    const auto start = Clock::now();
    std::size_t checksum = 0;
    for (int i = 0; i < kKeys; ++i) checksum += key_hash_slot("key-" + std::to_string(i));
    const auto hash_us = elapsed_us(start);
    std::cout << "== 槽计算 ==\n"
              << "  " << kKeys << " 个 key：" << hash_us << "us，checksum=" << checksum << "\n\n";

    Cluster cluster = three_masters();
    std::size_t counts[3] = {0, 0, 0};
    for (int i = 0; i < kKeys; ++i) {
        const Slot slot = key_hash_slot("key-" + std::to_string(i));
        const NodeId& owner = cluster.slots().owner(slot);
        if (owner == "M1") ++counts[0];
        else if (owner == "M2") ++counts[1];
        else ++counts[2];
    }
    std::cout << "== 三主槽分布（" << kKeys << " keys）==\n"
              << "  M1=" << counts[0] << "  M2=" << counts[1] << "  M3=" << counts[2] << "\n\n";

    ClusterClient client(cluster);
    const auto route_start = Clock::now();
    for (int i = 0; i < 100'000; ++i) {
        (void)client.execute(Command{{"GET", "key-" + std::to_string(i)}});
    }
    const auto route_us = elapsed_us(route_start);
    std::cout << "== 客户端路由 ==\n"
              << "  100k GET：" << route_us << "us；MOVED=" << client.moved_count()
              << "  ASK=" << client.ask_count() << "\n";
}

}  // namespace archdesign::test
