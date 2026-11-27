// main.cpp
//
// Day 12 的 MVCC 测试：快照隔离、自己的写、回滚、删除可见性、
// 写冲突和 purge。

#include "mvcc_store.hpp"

#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using archdesign::MVCCStore;
using archdesign::MVCCWriteConflict;

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_snapshot_isolation() {
    MVCCStore store;

    const auto t1 = store.begin();
    store.put(t1, "k", "v1");
    store.commit(t1);

    const auto reader = store.begin();

    const auto writer = store.begin();
    store.put(writer, "k", "v2");
    store.commit(writer);

    check(store.get(reader, "k") == std::optional<std::string>("v1"),
          "reader should keep its old snapshot");
    check(store.version_count("k") == 2, "two versions should exist");

    const auto fresh = store.begin();
    check(store.get(fresh, "k") == std::optional<std::string>("v2"),
          "new transaction should see the new version");

    store.commit(reader);
    store.commit(fresh);
}

void test_own_write_and_rollback() {
    MVCCStore store;

    const auto t1 = store.begin();
    store.put(t1, "a", "1");
    check(store.get(t1, "a") == std::optional<std::string>("1"),
          "own write should be visible");
    store.put(t1, "a", "2");
    check(store.get(t1, "a") == std::optional<std::string>("2"),
          "own latest write should be visible");
    store.erase(t1, "a");
    check(store.get(t1, "a") == std::nullopt,
          "own delete should hide the value");
    store.abort(t1);

    const auto t2 = store.begin();
    check(store.get(t2, "a") == std::nullopt,
          "aborted insert should not create a visible key");
    store.commit(t2);
}

void test_aborted_update_restores_old_value() {
    MVCCStore store;

    const auto t1 = store.begin();
    store.put(t1, "k", "old");
    store.commit(t1);

    const auto t2 = store.begin();
    store.put(t2, "k", "new");
    store.abort(t2);

    const auto t3 = store.begin();
    check(store.get(t3, "k") == std::optional<std::string>("old"),
          "aborted update should restore the old version");
    store.commit(t3);
}

void test_delete_visibility() {
    MVCCStore store;

    const auto t1 = store.begin();
    store.put(t1, "k", "v1");
    store.commit(t1);

    const auto reader = store.begin();

    const auto deleter = store.begin();
    store.erase(deleter, "k");
    store.commit(deleter);

    check(store.get(reader, "k") == std::optional<std::string>("v1"),
          "reader should not see a delete committed after its snapshot");

    const auto fresh = store.begin();
    check(store.get(fresh, "k") == std::nullopt,
          "new transaction should see the delete");

    store.commit(reader);
    store.commit(fresh);

    check(store.version_count("k") == 2, "value + tombstone");
    const auto removed = store.purge();
    check(removed >= 1, "purge should remove dead versions");
    check(store.version_count("k") == 0, "purged tombstone should remove key");
}

void test_write_conflict() {
    MVCCStore store;

    const auto t1 = store.begin();
    store.put(t1, "k", "v1");

    const auto t2 = store.begin();
    bool conflict = false;
    try {
        store.put(t2, "k", "v2");
    } catch (const MVCCWriteConflict&) {
        conflict = true;
    }
    check(conflict, "concurrent writers should conflict");

    store.commit(t1);
    store.put(t2, "k", "v2");
    store.commit(t2);

    const auto t3 = store.begin();
    check(store.get(t3, "k") == std::optional<std::string>("v2"),
          "writer after conflict should see its own committed value");
    store.commit(t3);
}

void test_purge_horizon() {
    MVCCStore store;

    const auto t1 = store.begin();
    store.put(t1, "k", "v1");
    store.commit(t1);

    const auto reader = store.begin();

    const auto t2 = store.begin();
    store.put(t2, "k", "v2");
    store.commit(t2);

    check(store.purge() == 0, "purge must not run with an active snapshot");
    check(store.get(reader, "k") == std::optional<std::string>("v1"),
          "reader snapshot should still see v1");

    store.commit(reader);
    const auto removed = store.purge();
    check(removed >= 1, "purge should remove old versions after snapshot ends");
    check(store.version_count("k") == 1, "only latest version should remain");
    check(store.stats().versions == 1, "stats should reflect purge");

    const auto fresh = store.begin();
    check(store.get(fresh, "k") == std::optional<std::string>("v2"),
          "purged store should still return latest value");
    store.commit(fresh);
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests = {
        {"snapshot_isolation", test_snapshot_isolation},
        {"own_write_and_rollback", test_own_write_and_rollback},
        {"aborted_update_restores_old_value", test_aborted_update_restores_old_value},
        {"delete_visibility", test_delete_visibility},
        {"write_conflict", test_write_conflict},
        {"purge_horizon", test_purge_horizon},
    };

    int failed = 0;
    for (const auto& [name, run] : tests) {
        try {
            run();
            std::cout << "[  OK  ] " << name << '\n' << std::flush;
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "[FAILED] " << name << ": " << error.what() << '\n'
                      << std::flush;
        } catch (...) {
            ++failed;
            std::cerr << "[FAILED] " << name << ": unknown exception\n"
                      << std::flush;
        }
    }

    if (failed != 0) {
        std::cerr << failed << " test(s) failed\n";
        return 1;
    }
    std::cout << "\nAll " << tests.size() << " tests passed.\n";
    return 0;
}
