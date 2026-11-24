// bench.cpp
//
// 小基准：backlog 的写入/续传吞吐，以及不同 lag 下的 PSYNC 语义。

#include <algorithm>
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
    constexpr int kCommands = 100'000;
    constexpr std::size_t kBacklog = 1 << 20;

    Master master("R1", kBacklog);
    const auto start = Clock::now();
    for (int i = 0; i < kCommands; ++i) {
        master.set("key-" + std::to_string(i), "value-" + std::to_string(i), kEpoch);
    }
    const auto append_us = elapsed_us(start);
    std::cout << "== backlog 写入 ==\n"
              << "  " << kCommands << " SET：offset=" << master.offset()
              << "  backlog=" << master.backlog().size() << "/" << master.backlog().capacity()
              << "  用时=" << append_us << "us\n\n";

    // 模拟 replica 落后 1/10 个流，测量批量续传拷贝。
    const Offset lag = master.offset() / 10;
    const Offset from = master.offset() - lag;
    const auto read_start = Clock::now();
    const std::string bytes = master.backlog().read_from(from);
    const auto read_us = elapsed_us(read_start);
    std::cout << "== 部分续传 ==\n"
              << "  从 offset=" << from << " 读 " << bytes.size() << " B，用时=" << read_us
              << "us\n\n";

    std::cout << "== 语义检查 ==\n";
    const auto at_end = master.psync("R1", master.offset(), kEpoch);
    const auto too_old = master.psync("R1", std::max<Offset>(0, master.backlog().start() - 1), kEpoch);
    std::cout << "  PSYNC at end -> "
              << (at_end.type == PsyncResult::Type::Continue ? "CONTINUE" : "FULLRESYNC")
              << "；PSYNC before backlog -> "
              << (too_old.type == PsyncResult::Type::Full ? "FULLRESYNC" : "CONTINUE") << "\n";
}

}  // namespace archdesign::test
