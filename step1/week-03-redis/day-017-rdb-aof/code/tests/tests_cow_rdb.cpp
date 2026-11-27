// tests_cow_rdb.cpp
//
// COW 快照与 RDB 编解码：页隔离、临时文件原子替换、CRC、TTL。

#include <set>
#include <string>
#include <vector>

#include "test_support.hpp"

namespace archdesign::test {

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

}  // namespace archdesign::test
