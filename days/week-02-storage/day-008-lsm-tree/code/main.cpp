// main.cpp
//
// Day 08 的最小 LSM 引擎测试：put/get/remove、WAL 恢复、flush、
// compaction 和 tombstone。

#include "mini_lsm.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

using namespace std::chrono_literals;
using archdesign::MiniLSM;

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class TempDir {
public:
    TempDir() {
        static std::atomic<unsigned long long> counter{0};
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("archdesign-lsm-" + std::to_string(stamp) + "-" +
                 std::to_string(counter.fetch_add(1)));
        std::filesystem::remove_all(path_);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void test_basic_put_get_remove() {
    TempDir dir;
    MiniLSM lsm(dir.path(), MiniLSM::Options{4096, 4});

    lsm.put("a", "1");
    lsm.put("b", "2");
    lsm.put("c", "3");

    check(lsm.get("a") == std::optional<std::string>("1"), "get a");
    check(lsm.get("b") == std::optional<std::string>("2"), "get b");
    check(lsm.get("missing") == std::nullopt, "missing key should be nullopt");

    lsm.remove("b");
    check(lsm.get("b") == std::nullopt, "removed key should be gone");

    lsm.flush();
    check(lsm.get("a") == std::optional<std::string>("1"),
          "get after flush");
    check(lsm.get("b") == std::nullopt, "tombstone after flush");
    lsm.close();

    MiniLSM reopened(dir.path(), MiniLSM::Options{4096, 4});
    check(reopened.get("a") == std::optional<std::string>("1"),
          "get after reopen");
    check(reopened.get("c") == std::optional<std::string>("3"),
          "get c after reopen");
    check(reopened.get("b") == std::nullopt, "tombstone after reopen");
    reopened.close();
}

void test_wal_recovery() {
    TempDir dir;

    {
        MiniLSM lsm(dir.path(), MiniLSM::Options{4096, 4});
        lsm.put("alpha", "one");
        lsm.put("beta", "two");
        lsm.remove("alpha");
        // 故意不调用 close()：析构只关闭 WAL，不把 memtable 写成 SST。
    }

    MiniLSM recovered(dir.path(), MiniLSM::Options{4096, 4});
    check(recovered.get("alpha") == std::nullopt,
          "WAL should recover tombstone");
    check(recovered.get("beta") == std::optional<std::string>("two"),
          "WAL should recover value");
    check(recovered.stats().recovered_wal_records >= 3,
          "WAL replay should count records");
    recovered.close();
}

void test_flush_and_compaction() {
    TempDir dir;
    MiniLSM::Options options;
    options.memtable_limit = 160;
    options.compaction_trigger = 3;

    MiniLSM lsm(dir.path(), options);
    constexpr int count = 200;

    for (int i = 0; i < count; ++i) {
        const auto key = "key-" + std::to_string(i);
        const auto value = std::string(80, static_cast<char>('a' + i % 26));
        lsm.put(key, value);
    }

    for (int i = 0; i < count; ++i) {
        const auto key = "key-" + std::to_string(i);
        const auto expected = std::string(80, static_cast<char>('a' + i % 26));
        const auto value = lsm.get(key);
        check(value.has_value() && *value == expected,
              "value should survive flush/compaction");
    }

    const auto stats = lsm.stats();
    check(stats.flushes > 0, "small memtable should trigger flushes");
    check(stats.compactions > 0, "SST count should trigger compaction");
    check(stats.sstables < options.compaction_trigger,
          "compaction should keep SST count below the trigger");

    lsm.close();

    MiniLSM reopened(dir.path(), options);
    for (int i = 0; i < count; ++i) {
        const auto key = "key-" + std::to_string(i);
        const auto expected = std::string(80, static_cast<char>('a' + i % 26));
        const auto value = reopened.get(key);
        check(value.has_value() && *value == expected,
              "value should survive reopen after compaction");
    }
    reopened.close();
}

void test_overwrite_and_tombstone_persistence() {
    TempDir dir;
    MiniLSM::Options options{128, 3};

    MiniLSM lsm(dir.path(), options);
    lsm.put("x", "old");
    lsm.put("x", "new");
    lsm.flush();
    lsm.remove("x");
    lsm.flush();

    check(lsm.get("x") == std::nullopt, "tombstone should hide old value");
    lsm.close();

    MiniLSM reopened(dir.path(), options);
    check(reopened.get("x") == std::nullopt,
          "tombstone should persist across reopen");
    reopened.close();
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests = {
        {"basic_put_get_remove", test_basic_put_get_remove},
        {"wal_recovery", test_wal_recovery},
        {"flush_and_compaction", test_flush_and_compaction},
        {"overwrite_and_tombstone_persistence", test_overwrite_and_tombstone_persistence},
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
