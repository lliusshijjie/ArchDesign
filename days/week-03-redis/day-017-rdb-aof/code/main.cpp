// main.cpp
//
// Day 17 的行为测试、演示与基准：RDB 快照 + fork/COW + AOF + rewrite。
//
// 编译运行：
//   cmake -S . -B build
//   cmake --build build
//   ctest --test-dir build --output-on-failure
//   ./build/persistence_demo                 # 跑全部测试
//   ./build/persistence_demo --demo          # 机制讲解式输出
//   ./build/persistence_demo --bench         # 快照/重写/fsync 的小基准
//   ./build/persistence_demo --case rdb_roundtrip

#include "persistence.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace archdesign;

using WallClock = std::chrono::steady_clock;

constexpr i64 kEpoch = 1'700'000'000'000LL;  // 2023-11-14T22:13:20Z

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <class Fn>
void expect_throw(Fn&& fn, const std::string& needle, const std::string& message) {
    try {
        fn();
    } catch (const std::exception& ex) {
        const std::string text = ex.what();
        check(text.find(needle) != std::string::npos,
              message + ": expected '" + needle + "' in '" + text + "'");
        return;
    }
    throw std::runtime_error(message + ": expected an exception");
}

std::string key_of(int index, const char* prefix = "k") {
    return std::string(prefix) + std::to_string(index);
}

std::string value_of(int index, std::size_t size = 24) {
    std::string value = "v" + std::to_string(index) + ":";
    while (value.size() < size) value.push_back('x');
    return value;
}

std::size_t elapsed_us(WallClock::time_point start) {
    return static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(WallClock::now() - start).count());
}

std::optional<Entry> snapshot_key(const Snapshot& snapshot, std::string_view wanted) {
    std::optional<Entry> found;
    snapshot.for_each([&](const std::string& key, const Entry& entry) {
        if (key == wanted) found = entry;
    });
    return found;
}

void require_same_live(const PageStore& left, const PageStore& right, i64 now,
                       const std::string& context) {
    check(left.live_key_count(now) == right.live_key_count(now),
          context + ": live key count differs (" + std::to_string(left.live_key_count(now)) +
              " vs " + std::to_string(right.live_key_count(now)) + ")");
    for (const std::string& key : left.keys()) {
        const auto a = left.get_live(key, now);
        const auto b = right.get_live(key, now);
        check(a.has_value() == b.has_value(), context + ": presence differs for " + key);
        if (!a) continue;
        check(a->value == b->value, context + ": value differs for " + key);
        check(a->expire_at_ms == b->expire_at_ms, context + ": TTL differs for " + key);
    }
}

EngineConfig base_config(bool aof_enabled = true, std::size_t pages = 16) {
    EngineConfig config;
    config.aof_enabled = aof_enabled;
    config.page_count = pages;
    config.auto_rewrite_min_size = 4096;
    return config;
}

void seed(Engine& engine, int count, i64 now, i64 ttl_ms = -1) {
    for (int i = 0; i < count; ++i) engine.set(key_of(i), value_of(i), now, ttl_ms);
}

// ---------------------------------------------------------------------------
// 测试用例
// ---------------------------------------------------------------------------

// 1. fork 只复制页表；写哪页才复制哪页；快照对旧值完全隔离。
void test_page_store_cow() {
    check(crc64("123456789") == 0xe9c6d914c4b8d9caULL, "CRC64 known-answer vector");

    PageStore store(8);
    for (int i = 0; i < 80; ++i) store.set(key_of(i), value_of(i), -1);
    const Snapshot snapshot = store.fork();
    check(snapshot.key_count() == 80, "snapshot must see all 80 keys");
    check(store.stats().snapshots == 1, "one fork recorded");

    // 找 4 个落在不同页上的 key，确保每个页只触发一次 COW。
    std::set<std::size_t> touched_pages;
    std::vector<std::string> victims;
    for (int i = 0; i < 80 && victims.size() < 4; ++i) {
        const std::string key = key_of(i);
        const std::size_t page = store.page_of(key);
        if (touched_pages.insert(page).second) victims.push_back(key);
    }
    check(victims.size() == 4, "found keys in four distinct pages");
    for (const std::string& key : victims) store.set(key, "new-value", -1);

    for (const std::string& key : victims) {
        const auto old = snapshot_key(snapshot, key);
        check(old.has_value() && old->value.rfind("v", 0) == 0, "snapshot keeps the old value");
        const auto now = store.get(key);
        check(now.has_value() && now->value == "new-value", "store sees the new value");
    }
    check(store.stats().pages_copied == victims.size(),
          "one page copied per distinct page touched");
    check(store.stats().bytes_copied > 0, "COW copied some bytes");

    // 新 key 在快照里不存在；快照的 key 数保持不变。
    store.set("brand-new", "x", -1);
    check(snapshot.key_count() == 80, "snapshot size is stable");
    check(!snapshot_key(snapshot, "brand-new").has_value(), "new key is invisible to snapshot");
}

// 2. RDB 编码/解码往返：魔数、版本、键、TTL、CRC。
void test_rdb_roundtrip() {
    PageStore source(8);
    source.set("alpha", "one", -1);
    source.set("beta", "two", kEpoch + 10'000);
    source.set("empty", "", -1);
    const std::string bytes = rdb_encode(source.fork());

    check(bytes.size() > 20, "RDB has a body");
    check(std::string_view(bytes).substr(0, 9) == "REDIS0011", "RDB magic and version");
    check(static_cast<u8>(bytes[9]) == 0xFA, "first opcode is AUX");

    PageStore restored(8);
    const RdbDecodeResult result = rdb_decode(bytes, restored, kEpoch);
    check(result.consumed == bytes.size(), "RDB consumed all bytes");
    check(result.loaded == 3, "RDB loaded three keys");
    check(result.skipped_expired == 0, "no expired keys");
    check(restored.get("alpha")->value == "one", "alpha value");
    check(restored.get("beta")->expire_at_ms == kEpoch + 10'000, "beta TTL");
    check(restored.get("empty")->value.empty(), "empty value survives");
}

// 3. RDB 的 TTL 是绝对时间；独立 RDB 启动时跳过已过期键，并计数。
void test_rdb_ttl() {
    PageStore source(8);
    source.set("alive", "a", kEpoch + 10'000);
    source.set("dead", "d", kEpoch + 100);
    source.set("forever", "f", -1);
    const std::string bytes = rdb_encode(source.fork());

    PageStore restored(8);
    const RdbDecodeResult result = rdb_decode(bytes, restored, kEpoch + 1'000);
    check(result.loaded == 2, "two live keys loaded");
    check(result.skipped_expired == 1, "one expired key skipped");
    check(restored.get("alive").has_value(), "alive key loaded");
    check(!restored.get("dead").has_value(), "dead key skipped");
    check(restored.get("forever").has_value(), "persistent key loaded");

    // AOF 的 RDB preamble 不跳过过期键：后面的 AOF tail 仍可能引用/删除它们。
    PageStore preamble(8);
    RdbLoadOptions options;
    options.skip_expired = false;
    (void)rdb_decode(bytes, preamble, kEpoch + 1'000, options);
    check(preamble.get("dead").has_value(), "AOF preamble keeps expired keys");
    check(!preamble.get_live("dead", kEpoch + 1'000).has_value(), "but reads hide them");
}

// 4. 坏校验和/截断必须拒绝；关闭 checksum 时写 0，加载端跳过比较。
void test_rdb_checksum_and_truncation() {
    PageStore source(8);
    source.set("k", "value", -1);
    std::string bytes = rdb_encode(source.fork());

    std::string bad_checksum = bytes;
    bad_checksum.back() = static_cast<char>(bad_checksum.back() ^ 0xff);
    PageStore restored(8);
    expect_throw([&] { (void)rdb_decode(bad_checksum, restored, kEpoch); }, "CRC64 mismatch",
                 "corrupted checksum must be rejected");

    const std::string truncated = bytes.substr(0, bytes.size() - 4);
    expect_throw([&] { (void)rdb_decode(truncated, restored, kEpoch); }, "unexpected end of RDB",
                 "truncated checksum must be rejected");

    RdbOptions no_checksum;
    no_checksum.checksum = false;
    const std::string unchecked = rdb_encode(source.fork(), no_checksum);
    check(read_u64_le(unchecked, unchecked.size() - 8) == 0, "disabled checksum is zero");
    PageStore unchecked_store(8);
    (void)rdb_decode(unchecked, unchecked_store, kEpoch);
    check(unchecked_store.get("k").has_value(), "zero checksum loads without comparison");
}

// 5. BGSAVE 的临时文件 + rename：失败不覆盖旧 RDB；快照与父进程写入隔离。
void test_rdb_bgsave_and_atomic_replace() {
    MemFs fs;
    Engine engine(fs, base_config(false, 8));
    seed(engine, 100, kEpoch);
    engine.save_rdb(kEpoch);
    const std::string good = fs.read("dump.rdb");
    check(!good.empty(), "initial RDB exists");

    // 写临时文件失败：旧文件必须原样保留。
    fs.set_fail_writes(true);
    expect_throw([&] { engine.save_rdb(kEpoch); }, "injected write failure",
                 "failed RDB save must throw");
    fs.set_fail_writes(false);
    check(fs.read("dump.rdb") == good, "failed save does not overwrite the old RDB");
    check(!fs.exists("temp-dump.rdb"), "failed save cleans its temp file");

    // 快照隔离：begin 之后父进程改 50 个 key，finish 写出的仍是 begin 时的值。
    engine.begin_bgsave(kEpoch);
    for (int i = 0; i < 50; ++i) engine.set(key_of(i), "changed", kEpoch);
    engine.finish_bgsave(kEpoch);

    PageStore restored(8);
    const std::string snapshot_bytes = fs.read("dump.rdb");
    (void)rdb_decode(snapshot_bytes, restored, kEpoch);
    check(restored.get(key_of(0))->value != "changed", "RDB captured the fork-time value");
    check(engine.get(key_of(0), kEpoch)->value == "changed", "parent still sees its own write");
    check(!engine.bgsave_active(), "BGSAVE finished");
}

// 6. RESP 编码与解析：完整命令、截断尾巴、注释行。
void test_aof_resp() {
    Command set{"SET", "k", "v"};
    check(resp_encode(set) == "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n",
          "RESP SET encoding");
    Command pexpireat{"PEXPIREAT", "k", "1700000001000"};
    check(resp_encode(pexpireat) == "*3\r\n$9\r\nPEXPIREAT\r\n$1\r\nk\r\n$13\r\n1700000001000\r\n",
          "RESP PEXPIREAT encoding");

    const std::string two = resp_encode(set) + resp_encode(Command{"DEL", "k"});
    const RespParseResult complete = resp_parse(two);
    check(complete.commands.size() == 2 && !complete.truncated, "two complete commands");
    check(complete.commands[0].args[0] == "SET", "first verb");
    check(complete.consumed == two.size(), "all bytes consumed");

    const RespParseResult cut = resp_parse(two.substr(0, two.size() - 4));
    check(cut.commands.size() == 1 && cut.truncated, "last command is truncated");
    check(cut.consumed == resp_encode(set).size(), "consumed points at the last complete command");

    const std::string annotated = "#TS:1700000000\r\n" + resp_encode(set);
    const RespParseResult with_annotation = resp_parse(annotated);
    check(with_annotation.commands.size() == 1, "timestamp annotation is skipped");
}

// 7. appendfsync=always：每次写命令后都已经 fsync，掉电不丢。
void test_aof_always() {
    MemFs fs;
    EngineConfig config = base_config();
    config.aof.fsync = FsyncPolicy::Always;
    Engine engine(fs, config);
    engine.load(kEpoch);
    seed(engine, 20, kEpoch);

    fs.crash();
    Engine restored(fs, config);
    restored.load(kEpoch);
    check(restored.stats().loaded_from == "aof", "loaded from AOF");
    require_same_live(engine.store(), restored.store(), kEpoch, "always policy");
    check(restored.size(kEpoch) == 20, "all 20 keys survived");
}

// 8. appendfsync=everysec：掉电窗口最多约 1 秒；tick 之后才安全。
void test_aof_everysec() {
    MemFs fs;
    EngineConfig config = base_config();
    config.aof.fsync = FsyncPolicy::EverySec;
    Engine engine(fs, config);
    engine.load(kEpoch);

    engine.set("k0", "lost", kEpoch);
    fs.crash();
    Engine after_crash(fs, config);
    after_crash.load(kEpoch);
    check(after_crash.size(kEpoch) == 0, "un-fsynced tail is lost after crash");

    engine.set("k1", "saved", kEpoch + 500);
    engine.aof().tick(kEpoch + 1500);  // 模拟 1 秒定时 fsync
    fs.crash();
    Engine after_tick(fs, config);
    after_tick.load(kEpoch + 1500);
    check(after_tick.get("k1", kEpoch + 1500).has_value(), "fsync makes the tail durable");
    check(!after_tick.get("k0", kEpoch + 1500).has_value(), "the earlier lost key stays lost");
}

// 9. appendfsync=no：只有显式 fsync 才能扛住掉电。
void test_aof_no() {
    MemFs fs;
    EngineConfig config = base_config();
    config.aof.fsync = FsyncPolicy::No;
    Engine engine(fs, config);
    engine.load(kEpoch);
    engine.set("k", "v", kEpoch);
    fs.crash();
    Engine lost(fs, config);
    lost.load(kEpoch);
    check(lost.size(kEpoch) == 0, "no fsync: page cache is gone");

    engine.set("k2", "v2", kEpoch + 100);
    engine.aof().fsync_now(kEpoch + 200);
    fs.crash();
    Engine saved(fs, config);
    saved.load(kEpoch + 200);
    check(saved.get("k2", kEpoch + 200).has_value(), "explicit fsync saves the data");
}

// 10. AOF 尾部半条命令：默认截断到最后一条完整命令；关闭开关则拒绝启动。
void test_aof_truncated_tail() {
    MemFs fs;
    Engine engine(fs, base_config());
    engine.load(kEpoch);
    seed(engine, 3, kEpoch);

    std::string data = fs.read("appendonly.aof");
    fs.truncate("appendonly.aof", data.size() - 5);

    Engine recovered(fs, base_config());
    recovered.load(kEpoch);
    check(recovered.size(kEpoch) == 2, "only the first two commands are replayed");
    check(recovered.stats().aof_truncated_bytes > 0, "discarded tail bytes counted");
    check(fs.size("appendonly.aof") < data.size(), "AOF was repaired to the valid prefix");

    EngineConfig strict = base_config();
    strict.aof.load_truncated = false;
    fs.truncate("appendonly.aof", 0);
    fs.append("appendonly.aof", "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$3\r\nva");
    expect_throw([&] { Engine rejected(fs, strict); rejected.load(kEpoch); }, "truncated tail",
                 "strict AOF load must reject a partial command");
}

// 11. AOF 中间损坏（不是尾巴）必须拒绝，不能静默丢数据。
void test_aof_corrupt() {
    MemFs fs;
    Engine engine(fs, base_config());
    engine.load(kEpoch);
    seed(engine, 3, kEpoch);
    std::string data = fs.read("appendonly.aof");
    data[0] = 'X';  // 破坏第一条命令的 '*'
    fs.write_file("appendonly.aof", data);
    fs.fsync("appendonly.aof");

    Engine broken(fs, base_config());
    expect_throw([&] { broken.load(kEpoch); }, "expected '*'", "middle corruption must fail");
}

// 12. rewrite：压缩历史；rewrite 期间的新写入进入 tail，不能丢。
void test_aof_rewrite() {
    MemFs fs;
    Engine engine(fs, base_config());
    engine.load(kEpoch);
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < 200; ++i) engine.set(key_of(i), value_of(round * 1000 + i), kEpoch);
    }
    for (int i = 0; i < 50; ++i) engine.del(key_of(i), kEpoch);
    const std::size_t old_size = fs.size("appendonly.aof");

    engine.begin_rewrite(kEpoch);
    for (int i = 0; i < 20; ++i) engine.set(key_of(1000 + i), value_of(i), kEpoch);
    for (int i = 50; i < 70; ++i) engine.del(key_of(i), kEpoch);
    engine.set(key_of(70), "post-snapshot", kEpoch);
    check(engine.rewrite_active(), "rewrite is active");
    check(engine.rewrite_tail_bytes() > 0, "tail captured writes during rewrite");
    engine.finish_rewrite(kEpoch);

    const std::size_t new_size = fs.size("appendonly.aof");
    check(new_size < old_size, "rewrite shrinks the AOF");
    check(engine.stats().rewrites == 1, "one rewrite counted");

    fs.crash();
    Engine restored(fs, base_config());
    restored.load(kEpoch);
    require_same_live(engine.store(), restored.store(), kEpoch, "rewrite result");
    check(restored.get(key_of(70), kEpoch)->value == "post-snapshot",
          "write during rewrite is present");
}

// 13. rewrite 未完成就崩溃：旧 AOF 仍完整，新临时文件不能影响恢复。
void test_aof_rewrite_crash_before_commit() {
    MemFs fs;
    EngineConfig config = base_config();
    config.aof.fsync = FsyncPolicy::Always;
    Engine engine(fs, config);
    engine.load(kEpoch);
    seed(engine, 50, kEpoch);

    engine.begin_rewrite(kEpoch);
    for (int i = 0; i < 10; ++i) engine.set(key_of(100 + i), value_of(i), kEpoch);
    fs.crash();  // 模拟 rewrite 子进程/父进程在 rename 前掉电

    Engine restored(fs, config);
    restored.load(kEpoch);
    check(restored.size(kEpoch) == 60, "old AOF plus live writes recover everything");
    require_same_live(engine.store(), restored.store(), kEpoch, "pre-commit crash");
}

// 14. rewrite 使用 RDB preamble：新 AOF 以 REDIS0011 开头，加载时先解 RDB 再解 tail。
void test_aof_rewrite_with_rdb_preamble() {
    MemFs fs;
    EngineConfig config = base_config();
    config.aof.use_rdb_preamble = true;
    Engine engine(fs, config);
    engine.load(kEpoch);
    for (int i = 0; i < 30; ++i) engine.set(key_of(i), value_of(i), kEpoch, 10'000);
    engine.begin_rewrite(kEpoch);
    engine.set("during", "rewrite", kEpoch, 20'000);
    engine.finish_rewrite(kEpoch);

    const std::string data = fs.read("appendonly.aof");
    check(std::string_view(data).substr(0, 9) == "REDIS0011", "AOF starts with RDB preamble");
    check(data.find("during") != std::string::npos, "tail is appended after preamble");

    fs.crash();
    Engine restored(fs, config);
    restored.load(kEpoch);
    check(restored.stats().loaded_from == "aof (rdb preamble)", "loader detects preamble");
    require_same_live(engine.store(), restored.store(), kEpoch, "RDB preamble roundtrip");
    check(restored.get("during", kEpoch)->expire_at_ms == kEpoch + 20'000,
          "TTL in tail is absolute");
}

// 15. AOF 与 RDB 同时存在：AOF 优先；关闭 AOF 才回退 RDB。
void test_recovery_precedence() {
    MemFs fs;

    EngineConfig rdb_only = base_config(false, 8);
    Engine rdb_engine(fs, rdb_only);
    rdb_engine.load(kEpoch);
    rdb_engine.set("from-rdb", "r", kEpoch);
    rdb_engine.save_rdb(kEpoch);

    EngineConfig aof_config = base_config(true, 8);
    Engine aof_engine(fs, aof_config);
    aof_engine.load(kEpoch);  // AOF 文件还不存在，回退 RDB
    check(aof_engine.get("from-rdb", kEpoch).has_value(), "initial load falls back to RDB");
    aof_engine.set("from-aof", "a", kEpoch);

    Engine both(fs, aof_config);
    both.load(kEpoch);
    check(both.stats().loaded_from == "aof", "AOF wins when both exist");
    check(both.get("from-aof", kEpoch).has_value(), "AOF data loaded");
    check(!both.get("from-rdb", kEpoch).has_value(), "RDB data not replayed on top of AOF");

    Engine only_rdb(fs, rdb_only);
    only_rdb.load(kEpoch);
    check(only_rdb.stats().loaded_from == "rdb", "AOF disabled -> RDB");
    check(only_rdb.get("from-rdb", kEpoch).has_value(), "RDB data loaded");
    check(!only_rdb.get("from-aof", kEpoch).has_value(), "AOF ignored when disabled");
}

// 16. 自动 rewrite 的触发条件：增长比例 + 最小体积。
void test_auto_rewrite() {
    MemFs fs;
    EngineConfig config = base_config();
    config.auto_rewrite_min_size = 256;
    config.auto_rewrite_percentage = 100;
    Engine engine(fs, config);
    engine.load(kEpoch);

    for (int i = 0; i < 300; ++i) engine.set("same", value_of(i), kEpoch);
    check(engine.maybe_auto_rewrite(kEpoch), "growth triggers auto rewrite");
    check(engine.rewrite_active(), "auto rewrite starts a session");
    engine.finish_rewrite(kEpoch);
    check(engine.stats().rewrites == 1, "auto rewrite completed");
    check(!engine.maybe_auto_rewrite(kEpoch), "fresh base does not trigger again");
}

// 17. 大规模 COW：只复制被写到的页，快照仍完整。
void test_cow_cost() {
    PageStore store(256);
    for (int i = 0; i < 10'000; ++i) store.set(key_of(i), value_of(i, 32), -1);
    const Snapshot snapshot = store.fork();

    std::set<std::size_t> pages;
    int changed = 0;
    for (int i = 0; i < 10'000 && pages.size() < 32; ++i) {
        const std::string key = key_of(i);
        if (pages.insert(store.page_of(key)).second) {
            store.set(key, "changed", -1);
            ++changed;
        }
    }
    check(changed == 32, "touched 32 distinct pages");
    check(store.stats().pages_copied == 32, "exactly 32 pages copied");
    check(snapshot.key_count() == 10'000, "snapshot still has 10k keys");
    check(store.stats().bytes_copied > 0, "COW bytes accounted");
}

// 18. 500 步随机操作 + 反复快照/rewrite/崩溃恢复，逐步对照参考模型。
struct ModelValue {
    std::string value;
    i64 expire_at_ms = -1;
};

void test_model() {
    MemFs fs;
    EngineConfig config = base_config();
    auto engine = std::make_unique<Engine>(fs, config);
    engine->load(kEpoch);

    std::map<std::string, ModelValue> model;
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> key_dist(0, 49);
    std::uniform_int_distribution<int> op_dist(0, 9);
    i64 now = kEpoch;

    auto apply_model = [&](const std::string& key, int op, int index) {
        if (op < 6) {
            model[key] = ModelValue{value_of(index), -1};
        } else if (op == 6) {
            model.erase(key);
        } else if (op == 7) {
            auto it = model.find(key);
            if (it != model.end()) it->second.expire_at_ms = now + 5'000;
        } else {
            auto it = model.find(key);
            if (it != model.end()) it->second.expire_at_ms = -1;
        }
    };

    auto verify = [&](const std::string& context) {
        std::size_t live = 0;
        for (const auto& [key, value] : model) {
            if (value.expire_at_ms != -1 && value.expire_at_ms <= now) continue;
            ++live;
            const auto actual = engine->get(key, now);
            check(actual.has_value(), context + ": missing key " + key);
            check(actual->value == value.value, context + ": value mismatch for " + key);
            check(actual->expire_at_ms == value.expire_at_ms, context + ": TTL mismatch for " + key);
        }
        check(engine->size(now) == live, context + ": live count mismatch");
    };

    for (int step = 0; step < 500; ++step) {
        now += 100;
        const std::string key = key_of(key_dist(rng));
        const int op = op_dist(rng);
        if (op < 6) {
            engine->set(key, value_of(step), now);
        } else if (op == 6) {
            engine->del(key, now);
        } else if (op == 7) {
            engine->expire(key, now, 5'000);
        } else {
            engine->persist(key, now);
        }
        apply_model(key, op, step);

        if (step % 50 == 49) {
            engine->save_rdb(now);
            engine->begin_rewrite(now);
            engine->set("during-rewrite", value_of(step), now);
            model["during-rewrite"] = ModelValue{value_of(step), -1};
            engine->finish_rewrite(now);

            fs.crash();
            engine = std::make_unique<Engine>(fs, config);
            engine->load(now);
            verify("after reload");
            check(engine->size(now) == engine->store().live_key_count(now),
                  "reloaded live count matches store");
        }
    }
    verify("final");
}

// ---------------------------------------------------------------------------
// 演示
// ---------------------------------------------------------------------------

void print_hex_prefix(const std::string& data, std::size_t limit = 48) {
    for (std::size_t i = 0; i < std::min(limit, data.size()); ++i) {
        const auto byte = static_cast<unsigned>(static_cast<u8>(data[i]));
        std::cout << std::hex << std::setw(2) << std::setfill('0') << byte << std::dec;
        if (i + 1 < std::min(limit, data.size())) std::cout << ' ';
    }
    std::cout << std::setfill(' ') << std::right << '\n';
}

void demo() {
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
              << "，复制字节≈" << store.stats().bytes_copied << " B（不是整个 20k 数据集）\n\n";

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

// ---------------------------------------------------------------------------
// 基准
// ---------------------------------------------------------------------------

void bench() {
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

// ---------------------------------------------------------------------------
// 测试注册
// ---------------------------------------------------------------------------

struct TestCase {
    const char* name;
    void (*fn)();
};

const std::vector<TestCase>& test_cases() {
    static const std::vector<TestCase> cases = {
        {"page_store_cow", test_page_store_cow},
        {"rdb_roundtrip", test_rdb_roundtrip},
        {"rdb_ttl", test_rdb_ttl},
        {"rdb_checksum", test_rdb_checksum_and_truncation},
        {"rdb_bgsave", test_rdb_bgsave_and_atomic_replace},
        {"aof_resp", test_aof_resp},
        {"aof_always", test_aof_always},
        {"aof_everysec", test_aof_everysec},
        {"aof_no", test_aof_no},
        {"aof_truncated", test_aof_truncated_tail},
        {"aof_corrupt", test_aof_corrupt},
        {"aof_rewrite", test_aof_rewrite},
        {"rewrite_crash", test_aof_rewrite_crash_before_commit},
        {"rewrite_preamble", test_aof_rewrite_with_rdb_preamble},
        {"recovery_precedence", test_recovery_precedence},
        {"auto_rewrite", test_auto_rewrite},
        {"cow_cost", test_cow_cost},
        {"model", test_model},
    };
    return cases;
}

int run_case(const std::string& name) {
    for (const TestCase& test : test_cases()) {
        if (name == test.name) {
            try {
                test.fn();
                std::cout << "[PASS] " << name << '\n';
                return 0;
            } catch (const std::exception& ex) {
                std::cout << "[FAIL] " << name << ": " << ex.what() << '\n';
                return 1;
            }
        }
    }
    std::cout << "[FAIL] unknown case: " << name << '\n';
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::ios::sync_with_stdio(false);
    if (argc > 1) {
        const std::string arg = argv[1];
        if (arg == "--demo") {
            demo();
            return 0;
        }
        if (arg == "--bench") {
            bench();
            return 0;
        }
        if (arg == "--list") {
            for (const TestCase& test : test_cases()) std::cout << test.name << '\n';
            return 0;
        }
        if (arg == "--case" && argc > 2) return run_case(argv[2]);
        std::cout << "usage: " << argv[0] << " [--demo|--bench|--list|--case <name>]\n";
        return 2;
    }

    int failures = 0;
    for (const TestCase& test : test_cases()) {
        try {
            test.fn();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& ex) {
            std::cout << "[FAIL] " << test.name << ": " << ex.what() << '\n';
            ++failures;
        }
    }
    std::cout << (failures == 0 ? "all tests passed" : std::to_string(failures) + " tests failed")
              << '\n';
    return failures == 0 ? 0 : 1;
}
