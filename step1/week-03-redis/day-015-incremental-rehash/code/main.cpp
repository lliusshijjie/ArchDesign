// main.cpp
//
// Day 15 的行为测试与演示：渐进式 rehash 的每一步到底发生了什么。
//
// 编译运行：
//   cmake -S . -B build -G Ninja
//   cmake --build build
//   ctest --test-dir build --output-on-failure
//   ./build/dict_demo            # 跑全部测试
//   ./build/dict_demo --trace    # 看一次 4 -> 8 的逐桶搬迁
//   ./build/dict_demo --bench    # 对比“渐进式”与“停世界”的最大停顿

#include "incremental_dict.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using archdesign::IncrementalDict;
using archdesign::RehashPause;
using Clock = std::chrono::steady_clock;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string key_of(int index) { return "key:" + std::to_string(index); }
std::string value_of(int index) { return "val:" + std::to_string(index); }

std::size_t elapsed_ns(Clock::time_point start) {
    return static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

// 1. 基本语义：插入、查找、重复插入、删除、不存在。
void test_basic() {
    IncrementalDict dict;
    check(dict.slots(0) == 0, "a fresh dict must not allocate a table");
    check(dict.size() == 0, "a fresh dict must be empty");

    for (int i = 0; i < 1000; ++i) {
        check(dict.insert(key_of(i), value_of(i)), "insert of a new key must succeed");
    }
    check(dict.size() == 1000, "size after 1000 inserts");

    for (int i = 0; i < 1000; ++i) {
        const auto* entry = dict.find(key_of(i));
        check(entry != nullptr, "every inserted key must be found");
        check(entry->value == value_of(i), "value must round-trip");
    }
    check(dict.find("nope") == nullptr, "a missing key must not be found");

    check(!dict.insert(key_of(7), "dup"), "duplicate insert must be rejected");
    check(dict.size() == 1000, "a rejected insert must not change size");
    check(dict.find(key_of(7))->value == value_of(7), "a rejected insert must not overwrite");

    for (int i = 0; i < 1000; i += 2) {
        check(dict.erase(key_of(i)), "erase of an existing key must succeed");
    }
    check(dict.size() == 500, "size after erasing half");
    for (int i = 0; i < 1000; ++i) {
        const bool expected = (i % 2) == 1;
        check((dict.find(key_of(i)) != nullptr) == expected, "membership after erase");
    }
    check(!dict.erase("nope"), "erasing a missing key must fail");
}

// 2. 扩容是渐进的：第 5 次插入只“开个头”，不会把 4 个桶一次搬完。
void test_growth() {
    IncrementalDict dict;
    for (int i = 0; i < 4; ++i) {
        dict.insert(key_of(i), value_of(i));
    }
    check(!dict.rehashing(), "4 keys fit in 4 buckets, no rehash yet");
    check(dict.slots(0) == 4, "initial table size is DICT_HT_INITIAL_SIZE");

    dict.insert(key_of(4), value_of(4));
    check(dict.rehashing(), "the 5th insert must start a rehash");
    check(dict.slots(0) == 4 && dict.slots(1) == 8, "the new table is twice as large");
    check(dict.rehash_index() == 0, "the cursor starts at bucket 0");
    check(dict.used(0) == 4 && dict.used(1) == 1, "the entry goes to the new table");

    const std::size_t before = dict.stats().buckets_moved;
    dict.find(key_of(0));
    const std::size_t after = dict.stats().buckets_moved;
    check(after >= before, "lookup must not move buckets backwards");
    check(after - before <= 1, "one lookup moves at most one bucket");
}

// 3. 任何单次操作都不会搬完整张表。
void test_no_stall() {
    IncrementalDict dict;
    for (int i = 0; i < 20000; ++i) {
        dict.insert(key_of(i), value_of(i));
        check(dict.stats().max_buckets_per_step <= 1,
              "a single insert must never move more than one bucket");
    }
    check(dict.size() == 20000, "all inserts must be accounted for");
    check(dict.count_entries() == dict.size(), "used counters must match the real entry count");
    check(dict.rehashing(), "20k inserts should leave a large rehash in progress");
}

// 4. rehash 期间查找必须同时看旧表和新表，并最终把 rehash 排空。
void test_lookup_both_tables() {
    IncrementalDict dict;
    for (int i = 0; i < 5000; ++i) {
        dict.insert(key_of(i), value_of(i));
    }
    check(dict.rehashing(), "expected an in-flight rehash");
    check(dict.used(0) > 0 && dict.used(1) > 0, "both tables must hold entries");

    for (int i = 0; i < 5000; ++i) {
        const auto* entry = dict.find(key_of(i));
        check(entry != nullptr, "a lookup during rehash must find keys in either table");
        check(entry->value == value_of(i), "value must survive the move");
    }
    check(!dict.rehashing(), "lookups alone must be able to drain the rehash");
    check(dict.size() == 5000, "nothing may be lost while draining");
}

// 5. 暂停期间的新写入落在新表，且不会推进游标。
void test_insert_new_table() {
    IncrementalDict dict;
    for (int i = 0; i < 5; ++i) {
        dict.insert(key_of(i), value_of(i));
    }
    check(dict.rehashing(), "rehash must be running");

    const std::size_t used1 = dict.used(1);
    {
        const RehashPause pause(dict);
        check(dict.insert("late", "42"), "insert while paused must succeed");
        check(dict.used(1) == used1 + 1, "a new key must land in the new table");
        check(dict.used(0) == 4, "the old table must not receive new keys");
        check(dict.rehash_index() == 0, "a paused rehash must not advance");
        check(dict.find("late") != nullptr, "find must work while paused");
    }
    check(dict.find("late") != nullptr, "the new key must still be there after resume");
}

// 6. 暂停期间可以从旧表和新表删除；两张表都空了也要正确收尾。
void test_erase_both_tables() {
    IncrementalDict dict;
    for (int i = 0; i < 5; ++i) {
        dict.insert(key_of(i), value_of(i));
    }
    check(dict.rehashing(), "rehash must be running");

    {
        const RehashPause pause(dict);
        check(dict.erase(key_of(4)), "erase the key that lives in the new table");
        check(dict.used(1) == 0, "the new table must be empty");
        check(dict.erase(key_of(0)), "erase a key that still lives in the old table");
        check(dict.used(0) == 3, "the old table count must drop");

        for (int i = 1; i < 4; ++i) {
            check(dict.erase(key_of(i)), "erase the rest of the old table");
        }
        check(dict.size() == 0, "everything is erased");
        check(dict.used(0) == 0 && dict.used(1) == 0, "both tables are empty");
        check(dict.rehashing(), "the cursor is still set until the next step");
    }

    // find() 在空字典上会直接返回，所以要显式走一步来收尾。
    check(!dict.rehash_buckets(1), "an empty old table must finish the rehash");
    check(!dict.rehashing(), "the cursor must be cleared");
    check(dict.slots(0) == 8, "the new table is promoted");
    check(dict.find("anything") == nullptr, "an empty dict returns null");
}

// 7. 缩容走同一套机制：负载因子低于 10% 时，表缩到能装下 used 的 2 的幂。
void test_shrink() {
    IncrementalDict dict;
    for (int i = 0; i < 1024; ++i) {
        dict.insert(key_of(i), value_of(i));
    }
    dict.rehash_milliseconds(1000);
    check(!dict.rehashing(), "the growth rehash must be drained");
    check(dict.slots(0) == 1024, "table size after 1024 inserts");
    check(dict.size() == 1024, "entry count");

    for (int i = 0; i < 1000; ++i) {
        check(dict.erase(key_of(i)), "erase most of the table");
    }
    check(dict.size() == 24, "24 keys left");
    check(dict.needs_shrink(), "load factor is below 10%");

    check(dict.shrink(), "shrink must start a rehash");
    check(dict.rehashing(), "shrink is incremental too");
    check(dict.slots(1) == 32, "the new table is the next power of two >= used");
    check(dict.find(key_of(1000)) != nullptr, "old keys are still reachable during shrink");

    dict.rehash_milliseconds(1000);
    check(!dict.rehashing(), "the shrink rehash must be drained");
    check(dict.slots(0) == 32, "the smaller table is promoted");
    for (int i = 1000; i < 1024; ++i) {
        check(dict.find(key_of(i)) != nullptr, "survivors must survive the shrink");
    }
}

// 8. 时间预算版本能在有限预算内排空大表，且不丢条目。
void test_time_budget() {
    IncrementalDict dict;
    for (int i = 0; i < 50000; ++i) {
        dict.insert(key_of(i), value_of(i));
    }
    check(dict.rehashing(), "expected an in-flight rehash");

    const std::size_t before = dict.size();
    int rounds = 0;
    while (dict.rehashing() && rounds < 1000) {
        dict.rehash_milliseconds(1);
        ++rounds;
    }
    check(!dict.rehashing(), "the rehash must finish");
    check(dict.size() == before, "no entry may be lost");
    check(dict.count_entries() == before, "the real entry count must match used");
    for (int i = 0; i < 50000; ++i) {
        check(dict.find(key_of(i)) != nullptr, "all keys must still be reachable");
    }
}

// 9. 没在 rehash 时，SCAN 不重不漏。
void test_scan_stable() {
    IncrementalDict dict;
    for (int i = 0; i < 1000; ++i) {
        dict.insert(key_of(i), value_of(i));
    }
    dict.rehash_milliseconds(1000);
    check(!dict.rehashing(), "the scan test wants a stable table");

    std::map<std::string, int> seen;
    std::uint64_t cursor = 0;
    std::size_t rounds = 0;
    do {
        cursor = dict.scan(cursor, [&seen](const IncrementalDict::Entry& entry) {
            ++seen[entry.key];
        });
        check(++rounds < 100000, "scan must terminate");
    } while (cursor != 0);

    check(seen.size() == 1000, "every key must be visited");
    for (const auto& item : seen) {
        check(item.second == 1, "a stable table must not return duplicates");
    }
}

// 10. rehash 期间（甚至扫描途中又扩容）SCAN 不能漏掉旧 key。
void test_scan_during_rehash() {
    IncrementalDict dict;
    for (int i = 0; i < 4096; ++i) {
        dict.insert(key_of(i), value_of(i));
    }
    // 4096 次插入可能已经把上一次 rehash 排空了；这里显式制造一次大表迁移，
    // 让 SCAN 一定跨越“两张表”的状态。
    if (!dict.rehashing()) {
        check(dict.expand(dict.slots(0) * 2), "force a large rehash before scanning");
    }
    check(dict.rehashing(), "expected an in-flight rehash");

    std::set<std::string> seen;
    std::size_t visits = 0;
    std::uint64_t cursor = 0;
    std::size_t rounds = 0;
    int inserted = 0;
    do {
        cursor = dict.scan(cursor, [&](const IncrementalDict::Entry& entry) {
            seen.insert(entry.key);
            ++visits;
        });
        // 两次 scan 之间：插入新 key（可能触发扩容），再让 rehash 前进一格。
        if (rounds % 3 == 0 && inserted < 500) {
            const int index = 4096 + inserted;
            check(dict.insert(key_of(index), value_of(index)), "insert during scan");
            ++inserted;
        }
        dict.find(key_of(1));
        check(++rounds < 100000, "scan must terminate");
    } while (cursor != 0);

    check(cursor == 0, "the cursor must eventually return to zero");
    check(visits >= 4096, "all entries must be emitted");
    for (int i = 0; i < 4096; ++i) {
        // 只保证“至少一次”：rehash 期间同一个 key 可能被访问两次。
        check(seen.count(key_of(i)) >= 1, "an element present for the whole scan must appear");
    }
}

// 11. 随机操作序列与 std::map 对拍，覆盖扩容、缩容、删除、查找的交错。
void test_model() {
    std::mt19937 rng(20261118U);
    IncrementalDict dict;
    std::map<std::string, std::string> model;

    for (int op = 0; op < 20000; ++op) {
        const int action = static_cast<int>(rng() % 100);
        const std::string key = key_of(static_cast<int>(rng() % 3000));

        if (action < 50) {
            const std::string value = value_of(static_cast<int>(rng() % 3000));
            const bool inserted = dict.insert(key, value);
            const bool expected = model.emplace(key, value).second;
            check(inserted == expected, "insert result must match the model");
            if (expected) {
                check(dict.find(key)->value == value, "inserted value must match");
            } else {
                // 已存在：值不能被覆盖。
                check(dict.find(key)->value == model[key], "duplicate insert must not overwrite");
            }
        } else if (action < 80) {
            const bool erased = dict.erase(key);
            const bool expected = model.erase(key) > 0;
            check(erased == expected, "erase result must match the model");
        } else {
            const auto it = model.find(key);
            const auto* entry = dict.find(key);
            check((it != model.end()) == (entry != nullptr), "membership must match the model");
            if (entry != nullptr) {
                check(entry->value == it->second, "value must match the model");
            }
        }
        check(dict.size() == model.size(), "size must match the model");
        check(dict.count_entries() == model.size(), "used counters must match real entries");

        if (op % 500 == 0) {
            dict.rehash_milliseconds(0);  // 加快切片，混入大表状态
        }
    }

    dict.rehash_milliseconds(1000);
    check(!dict.rehashing(), "the model test must end with a drained table");
    for (const auto& [key, value] : model) {
        const auto* entry = dict.find(key);
        check(entry != nullptr, "every model key must be found");
        check(entry->value == value, "every model value must match");
    }
    check(dict.count_entries() == model.size(), "final entry count");
}

// ------------------------------- 演示 --------------------------------------

void print_table(const IncrementalDict& dict, int table) {
    const auto keys = dict.bucket_keys(table);
    std::cout << "  table " << table << ": slots=" << keys.size()
              << " used=" << dict.used(table) << '\n';
    for (std::size_t i = 0; i < keys.size(); ++i) {
        std::cout << "    [" << i << "]";
        for (const auto& key : keys[i]) {
            std::cout << ' ' << key;
        }
        std::cout << '\n';
    }
}

void run_trace() {
    IncrementalDict dict;
    for (int i = 0; i < 5; ++i) {
        dict.insert(key_of(i), value_of(i));
    }

    std::cout << "== 5 次 SET 之后：旧表 4 桶，新表 8 桶，rehash 刚开头 ==\n";
    print_table(dict, 0);
    print_table(dict, 1);
    std::cout << "  rehash_index=" << dict.rehash_index()
              << "（旧表里下一个待搬的桶号）\n\n";

    std::cout << "== 每次 GET 顺手搬一个桶 ==\n";
    for (int i = 0; i < 8; ++i) {
        const std::string key = key_of(i % 5);
        const bool hit = dict.find(key) != nullptr;
        const auto stats = dict.stats();
        std::cout << "GET " << key << (hit ? " 命中" : " 未命中")
                  << " -> rehash_index=" << stats.rehash_index
                  << " 搬走桶=" << dict.last_moved_bucket()
                  << " 条目=" << dict.last_moved_entries()
                  << " table0 used=" << stats.used[0]
                  << " table1 used=" << stats.used[1] << '\n';
        if (!dict.rehashing()) {
            std::cout << "  rehash 完成：旧表被释放，table1 升格为 table0\n";
            break;
        }
    }

    std::cout << "\n== 收尾 ==\n";
    print_table(dict, 0);
    const auto stats = dict.stats();
    std::cout << "  buckets moved=" << stats.buckets_moved
              << " entries moved=" << stats.entries_moved
              << " empty buckets skipped=" << stats.empty_buckets_skipped
              << " max buckets/step=" << stats.max_buckets_per_step << '\n';
}

void run_bench() {
    constexpr int kCount = 200000;
    IncrementalDict dict;
    std::vector<std::size_t> samples;
    samples.reserve(kCount);

    for (int i = 0; i < kCount; ++i) {
        const auto start = Clock::now();
        dict.insert(key_of(i), value_of(i));
        samples.push_back(elapsed_ns(start));
    }
    std::sort(samples.begin(), samples.end());

    const auto stats = dict.stats();
    auto percentile = [&samples](double p) {
        const auto index = static_cast<std::size_t>(p * static_cast<double>(samples.size() - 1));
        return samples[index];
    };

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "inserts             = " << kCount << '\n'
              << "slots               = " << stats.slots[0] << " -> " << stats.slots[1] << '\n'
              << "entries             = " << dict.size() << '\n'
              << "rehashing           = " << (dict.rehashing() ? "yes" : "no") << '\n'
              << "buckets moved       = " << stats.buckets_moved << '\n'
              << "entries moved       = " << stats.entries_moved << '\n'
              << "empty buckets skip  = " << stats.empty_buckets_skipped << '\n'
              << "max buckets / step  = " << stats.max_buckets_per_step << '\n'
              << "max step time       = " << stats.max_step_time_ns / 1000.0 << " us\n"
              << "avg step time       = "
              << (stats.steps == 0 ? 0.0
                                   : static_cast<double>(stats.step_time_ns) /
                                         static_cast<double>(stats.steps) / 1000.0)
              << " us\n"
              << "insert p50 / p99    = " << percentile(0.50) / 1000.0 << " / "
              << percentile(0.99) / 1000.0 << " us\n"
              << "insert max          = " << static_cast<double>(samples.back()) / 1000.0
              << " us\n";

    // 一次只搬一桶地排空，看看“最后一个 step”有多贵：
    // 它除了搬桶，还要把旧表的桶全部析构掉。
    std::size_t final_step_ns = 0;
    while (dict.rehashing()) {
        const auto start = Clock::now();
        dict.rehash_buckets(1);
        if (!dict.rehashing()) {
            final_step_ns = elapsed_ns(start);
        }
    }
    std::cout << "final step          = " << static_cast<double>(final_step_ns) / 1000.0
              << " us (frees the old table)\n";

    // 对照组：把同样多的桶一次性搬完，看“停世界”版本的停顿。
    const std::size_t slots = dict.slots(0);
    if (dict.expand(slots * 2)) {
        const auto start = Clock::now();
        dict.full_rehash_now();
        const std::size_t full_ns = elapsed_ns(start);
        std::cout << "stop-the-world      = " << static_cast<double>(full_ns) / 1e6 << " ms ("
                  << slots << " buckets, one shot)\n";
    }
}

struct TestCase {
    const char* name;
    void (*run)();
};

const TestCase kTests[] = {
    {"basic", test_basic},
    {"growth", test_growth},
    {"no_stall", test_no_stall},
    {"lookup_both_tables", test_lookup_both_tables},
    {"insert_new_table", test_insert_new_table},
    {"erase_both_tables", test_erase_both_tables},
    {"shrink", test_shrink},
    {"time_budget", test_time_budget},
    {"scan_stable", test_scan_stable},
    {"scan_during_rehash", test_scan_during_rehash},
    {"model", test_model},
};

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);

    if (!args.empty() && args.front() == "--trace") {
        run_trace();
        return 0;
    }
    if (!args.empty() && args.front() == "--bench") {
        run_bench();
        return 0;
    }

    std::string selected;
    if (args.size() == 2 && args.front() == "--case") {
        selected = args[1];
    } else if (!args.empty()) {
        std::cerr << "usage: dict_demo [--trace|--bench|--case <name>]\n";
        return 2;
    }

    std::size_t ran = 0;
    std::size_t failed = 0;
    for (const TestCase& test : kTests) {
        if (!selected.empty() && selected != test.name) {
            continue;
        }
        ++ran;
        try {
            test.run();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cout << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }

    if (ran == 0) {
        std::cerr << "unknown test case: " << selected << '\n';
        return 2;
    }
    if (failed != 0) {
        std::cout << failed << " of " << ran << " test(s) failed\n";
        return 1;
    }
    std::cout << "all " << ran << " test(s) passed\n";
    return 0;
}
