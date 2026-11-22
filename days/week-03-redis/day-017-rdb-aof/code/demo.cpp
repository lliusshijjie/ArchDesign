// demo.cpp
//
// 讲解式演示：RDB 字节、COW、rewrite 压缩、appendfsync 掉电窗口。
// 和测试分开，避免 main.cpp 变成“测试 + 演示 + 基准”的混合体。

#include <iomanip>
#include <iostream>
#include <set>
#include <string>

#include "tests/test_support.hpp"

namespace archdesign::test {

namespace {

void print_hex_prefix(const std::string& data, std::size_t limit = 48) {
    for (std::size_t i = 0; i < std::min(limit, data.size()); ++i) {
        const auto byte = static_cast<unsigned>(static_cast<u8>(data[i]));
        std::cout << std::hex << std::setw(2) << std::setfill('0') << byte << std::dec;
        if (i + 1 < std::min(limit, data.size())) std::cout << ' ';
    }
    std::cout << std::setfill(' ') << std::right << '\n';
}

}  // namespace

void run_demo() {
    std::cout << "== 演示 1：RDB 文件长什么样 ==\n";
    PageStore source(8);
    source.set("user:1", "Alice", -1);
    source.set("session:1", "token", kEpoch + 60'000);
    const std::string rdb = rdb_encode(source.fork());
    std::cout << "  size=" << rdb.size() << " bytes, magic=" << rdb.substr(0, 9) << "\n  hex: ";
    print_hex_prefix(rdb, 64);
    std::cout << "  EOF 后的 8 字节是 CRC64；先写 temp、fsync、rename，才覆盖 dump.rdb。\n\n";

    std::cout << "== 演示 2：fork 之后的 COW ==\n";
    PageStore store(256);
    for (int i = 0; i < 20'000; ++i) store.set(key_of(i), value_of(i, 24), -1);
    const Snapshot snap = store.fork();
    std::set<std::size_t> pages;
    for (int i = 0; i < 20'000 && pages.size() < 32; ++i) {
        if (pages.insert(store.page_of(key_of(i))).second) store.set(key_of(i), "changed", -1);
    }
    std::cout << "  20k keys / 256 pages -> fork 后改 32 个不同页\n"
              << "  快照仍看到 " << snap.key_count() << " 个 key\n"
              << "  COW 复制页数=" << store.stats().pages_copied
              << "，复制字节≈" << store.stats().bytes_copied << " B\n\n";

    std::cout << "== 演示 3：AOF rewrite 压缩历史 ==\n";
    MemFs fs;
    Engine engine(fs, base_config());
    engine.load(kEpoch);
    for (int round = 0; round < 4; ++round) {
        for (int i = 0; i < 2000; ++i) engine.set(key_of(i), value_of(round * 10000 + i), kEpoch);
    }
    const std::size_t before = fs.size("appendonly.aof");
    engine.begin_rewrite(kEpoch);
    for (int i = 0; i < 50; ++i) engine.set(key_of(5000 + i), value_of(i), kEpoch);
    engine.finish_rewrite(kEpoch);
    const std::size_t after = fs.size("appendonly.aof");
    std::cout << "  4 轮覆盖写 2000 个 key：AOF " << before << " B -> " << after << " B"
              << "（压缩到 " << (after * 100 / before) << "%）\n"
              << "  rewrite 期间 50 条写入进入 tail，已包含在新文件中\n\n";

    std::cout << "== 演示 4：appendfsync 的掉电窗口 ==\n";
    for (FsyncPolicy policy : {FsyncPolicy::Always, FsyncPolicy::EverySec, FsyncPolicy::No}) {
        MemFs policy_fs;
        EngineConfig config = base_config();
        config.aof.fsync = policy;
        Engine policy_engine(policy_fs, config);
        policy_engine.load(kEpoch);
        for (int i = 0; i < 100; ++i) policy_engine.set(key_of(i), "v", kEpoch);
        policy_fs.crash();
        Engine recovered(policy_fs, config);
        recovered.load(kEpoch);
        std::cout << "  " << std::setw(8) << std::left << fsync_policy_name(policy) << std::right
                  << " fsyncs=" << policy_engine.aof().fsyncs()
                  << "  crash 后保留=" << recovered.size(kEpoch) << "/100\n";
    }
}

}  // namespace archdesign::test
