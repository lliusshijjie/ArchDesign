// bench.cpp
//
// 小基准：RDB 编解码、COW 复制量、AOF 写入 + rewrite。
// 不参与正确性断言，只给出量级和相对关系。

#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <set>
#include <string>

#include "tests/test_support.hpp"

namespace archdesign::test {

namespace {

std::size_t elapsed_us(WallClock::time_point start) {
    return static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(WallClock::now() - start).count());
}

}  // namespace

void run_bench() {
    std::cout << "== 基准 1：RDB 编码/解码（5 万 key）==\n";
    PageStore source(1024);
    for (int i = 0; i < 50'000; ++i) source.set(key_of(i), value_of(i, 32), kEpoch + 60'000);
    const Snapshot snap = source.fork();

    auto start = WallClock::now();
    const std::string rdb = rdb_encode(snap);
    const auto encode_us = elapsed_us(start);

    start = WallClock::now();
    PageStore restored(1024);
    const RdbDecodeResult decoded = rdb_decode(rdb, restored, kEpoch);
    const auto decode_us = elapsed_us(start);
    std::cout << "  bytes=" << rdb.size() << "  encode=" << encode_us << "us  decode=" << decode_us
              << "us  loaded=" << decoded.loaded << "\n\n";

    std::cout << "== 基准 2：COW（5 万 key / 1024 页，改 64 页）==\n";
    const Snapshot before_cow = source.fork();
    std::set<std::size_t> pages;
    start = WallClock::now();
    for (int i = 0; i < 50'000 && pages.size() < 64; ++i) {
        if (pages.insert(source.page_of(key_of(i))).second) source.set(key_of(i), "changed", kEpoch);
    }
    const auto cow_us = elapsed_us(start);
    std::cout << "  pages_copied=" << source.stats().pages_copied
              << "  bytes_copied=" << source.stats().bytes_copied << "  time=" << cow_us
              << "us  snapshot_keys=" << before_cow.key_count() << "\n\n";

    std::cout << "== 基准 3：AOF 写入 + rewrite（2 万 key，3 轮覆盖）==\n";
    MemFs fs;
    Engine engine(fs, base_config());
    engine.load(kEpoch);
    start = WallClock::now();
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < 20'000; ++i) engine.set(key_of(i), value_of(round * 10000 + i), kEpoch);
    }
    const auto write_us = elapsed_us(start);
    const std::size_t before = fs.size("appendonly.aof");

    start = WallClock::now();
    engine.begin_rewrite(kEpoch);
    engine.finish_rewrite(kEpoch);
    const auto rewrite_us = elapsed_us(start);
    const std::size_t after = fs.size("appendonly.aof");
    std::cout << "  60k SET 写入=" << write_us << "us  AOF " << before << " B -> " << after
              << " B  rewrite=" << rewrite_us << "us\n";
}

}  // namespace archdesign::test
