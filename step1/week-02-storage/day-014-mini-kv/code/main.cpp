// main.cpp
//
// Day 14 周复盘测试：把 WAL / MemTable / SST / compaction 组装起来，
// 验证正常读写、崩溃恢复、部分尾记录截断、校验和、compaction 和 tombstone。

#include "mini_kv.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

using archdesign::MiniKV;

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
                ("archdesign-mini-kv-" + std::to_string(stamp) + "-" +
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

void test_basic_and_reopen() {
    TempDir dir;
    MiniKV::Options options{4096, 4};

    {
        MiniKV kv(dir.path(), options);
        kv.put("a", "1");
        kv.put("b", "2");
        check(kv.get("a") == std::optional<std::string>("1"), "get a");
        kv.erase("b");
        check(kv.get("b") == std::nullopt, "erase b");
        kv.flush();
        kv.close();
    }

    MiniKV reopened(dir.path(), options);
    check(reopened.get("a") == std::optional<std::string>("1"),
          "a should survive reopen");
    check(reopened.get("b") == std::nullopt,
          "tombstone should survive reopen");
    reopened.close();
}

void test_wal_recovery_without_flush() {
    TempDir dir;
    MiniKV::Options options{4096, 4};

    {
        MiniKV kv(dir.path(), options);
        kv.put("alpha", "one");
        kv.put("beta", "two");
        // 故意不 close：析构只关闭 WAL，不 flush memtable。
    }

    MiniKV recovered(dir.path(), options);
    check(recovered.get("alpha") == std::optional<std::string>("one"),
          "WAL should recover alpha");
    check(recovered.get("beta") == std::optional<std::string>("two"),
          "WAL should recover beta");
    check(recovered.stats().recovered_wal_records >= 2,
          "WAL replay should count records");
    recovered.close();
}

void test_partial_tail_is_truncated() {
    TempDir dir;
    MiniKV::Options options{4096, 4};

    {
        MiniKV kv(dir.path(), options);
        kv.put("a", "1");
    }

    {
        std::ofstream wal(dir.path() / "wal.log",
                          std::ios::binary | std::ios::app);
        wal << "bad-tail";
    }

    MiniKV with_tail(dir.path(), options);
    check(with_tail.stats().truncated_wal_bytes == 8,
          "partial tail should be truncated");
    check(with_tail.get("a") == std::optional<std::string>("1"),
          "valid prefix should be recovered");
    with_tail.put("b", "2");
    with_tail.close();

    MiniKV reopened(dir.path(), options);
    check(reopened.get("a") == std::optional<std::string>("1"),
          "a after tail recovery");
    check(reopened.get("b") == std::optional<std::string>("2"),
          "append after truncation should work");
    reopened.close();
}

void test_checksum_corruption_truncates_tail() {
    TempDir dir;
    MiniKV::Options options{4096, 4};

    {
        MiniKV kv(dir.path(), options);
        kv.put("a", "1");
        kv.put("b", "2");
    }

    {
        std::fstream wal(dir.path() / "wal.log",
                         std::ios::binary | std::ios::in | std::ios::out);
        wal.seekp(-1, std::ios::end);
        char byte = 0;
        wal.seekg(-1, std::ios::end);
        wal.read(&byte, 1);
        byte = static_cast<char>(byte ^ 0x5A);
        wal.clear();
        wal.seekp(-1, std::ios::end);
        wal.write(&byte, 1);
    }

    MiniKV recovered(dir.path(), options);
    check(recovered.stats().truncated_wal_bytes > 0,
          "bad checksum should truncate the record tail");
    check(recovered.get("a") == std::optional<std::string>("1"),
          "record before corruption should survive");
    check(recovered.get("b") == std::nullopt,
          "corrupted record should not be applied");
    recovered.close();
}

void test_flush_and_compaction() {
    TempDir dir;
    MiniKV::Options options{160, 3};

    MiniKV kv(dir.path(), options);
    constexpr int count = 200;
    for (int i = 0; i < count; ++i) {
        const auto key = "key-" + std::to_string(i);
        const auto value = std::string(80, static_cast<char>('a' + i % 26));
        kv.put(key, value);
    }

    for (int i = 0; i < count; ++i) {
        const auto key = "key-" + std::to_string(i);
        const auto expected = std::string(80, static_cast<char>('a' + i % 26));
        check(kv.get(key) == std::optional<std::string>(expected),
              "value should survive flush/compaction");
    }

    const auto stats = kv.stats();
    check(stats.flushes > 0, "small memtable should flush");
    check(stats.compactions > 0, "SST count should trigger compaction");
    check(stats.sstables < options.compaction_trigger,
          "compaction should keep SST count below trigger");
    kv.close();

    MiniKV reopened(dir.path(), options);
    for (int i = 0; i < count; ++i) {
        const auto key = "key-" + std::to_string(i);
        const auto expected = std::string(80, static_cast<char>('a' + i % 26));
        check(reopened.get(key) == std::optional<std::string>(expected),
              "value should survive reopen");
    }
    reopened.close();
}

void test_tombstone_persistence() {
    TempDir dir;
    MiniKV::Options options{128, 3};

    MiniKV kv(dir.path(), options);
    kv.put("x", "old");
    kv.flush();
    kv.erase("x");
    kv.flush();
    check(kv.get("x") == std::nullopt, "tombstone should hide old value");
    kv.close();

    MiniKV reopened(dir.path(), options);
    check(reopened.get("x") == std::nullopt,
          "tombstone should persist across reopen");
    reopened.close();
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests = {
        {"basic_and_reopen", test_basic_and_reopen},
        {"wal_recovery_without_flush", test_wal_recovery_without_flush},
        {"partial_tail_is_truncated", test_partial_tail_is_truncated},
        {"checksum_corruption_truncates_tail", test_checksum_corruption_truncates_tail},
        {"flush_and_compaction", test_flush_and_compaction},
        {"tombstone_persistence", test_tombstone_persistence},
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
