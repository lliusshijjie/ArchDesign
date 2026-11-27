// engine.hpp
//
// 工作坊 A 的被测系统：一个“会坏”的 Mini KV。
//
// 与 Day 14 相比，多出来的不是功能，而是**顺序和不变量**：
//   1. WAL 记录带 CRC32，恢复时截断坏尾；
//   2. SST 先写临时文件、fsync、再 rename 发布；
//   3. MANIFEST 记录“哪些 SST 是有效的”，用 tmp + fsync + 原子 rename 提交；
//   4. 只有 manifest 提交之后，才允许清 MemTable、截断 WAL；
//   5. 每一步都埋了崩溃点，方便把“顺序”变成可注入的故障。
//
// 它仍然不是生产引擎：单线程、无锁、无 MVCC、全量 compaction、无 block index。
// 这些“缺口”在报告里逐条列出，不假装它是 RocksDB。

#pragma once

#include "codec.hpp"
#include "crash.hpp"
#include "disk.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace arch {

struct KVEntry {
    std::string key;
    std::string value;
    bool tombstone = false;
};

// key -> 最新值；nullopt 表示“被删除”（tombstone 还在，但对外不可见）。
using Snapshot = std::map<std::string, std::optional<std::string>>;

class Engine {
public:
    struct Options {
        std::size_t memtable_limit = 512;    // 估算字节数，超过就 flush
        std::size_t compaction_trigger = 3;  // SST 数量达到该值触发全量合并
        bool fsync_wal = true;
        bool fsync_sst = true;
        bool fsync_manifest = true;
        std::size_t group_commit = 1;        // 每 N 条 WAL 记录 fsync 一次
    };

    struct Stats {
        std::uint64_t puts = 0;
        std::uint64_t erases = 0;
        std::uint64_t flushes = 0;
        std::uint64_t compactions = 0;
        std::uint64_t user_bytes = 0;
        std::uint64_t wal_bytes = 0;
        std::uint64_t sst_bytes = 0;
        std::uint64_t manifest_bytes = 0;
        std::uint64_t wal_records_replayed = 0;
        std::uint64_t wal_truncated_bytes = 0;
        std::uint64_t fsyncs = 0;
        std::uint64_t orphan_ssts_removed = 0;
        std::uint64_t temp_files_removed = 0;
        std::size_t ssts = 0;
        double open_ms = 0.0;

        [[nodiscard]] std::uint64_t total_write_bytes() const {
            return wal_bytes + sst_bytes + manifest_bytes;
        }
    };

    static constexpr std::string_view wal_file = "WAL";
    static constexpr std::string_view manifest_file = "MANIFEST";
    static constexpr std::string_view manifest_temp = "MANIFEST.tmp";

    Engine(fs::path directory, Options options, CrashInjection* crash = nullptr);
    ~Engine() = default;

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void put(const std::string& key, std::string_view value);
    void erase(const std::string& key);
    [[nodiscard]] std::optional<std::string> get(const std::string& key) const;
    // 合并后的全量视图，用于实验里检查“有没有多出没写过的 key”。
    [[nodiscard]] Snapshot dump() const;

    void flush();
    void close();
    // 模拟断电：把没 fsync 的字节全部丢掉，之后对象不再可用。
    void power_loss();

    [[nodiscard]] Stats stats() const;
    [[nodiscard]] const Options& options() const { return options_; }

private:
    struct SSTable {
        std::uint64_t id = 0;
        std::vector<KVEntry> entries;
    };

    struct SSTRef {
        std::uint64_t id = 0;
        std::uint64_t entries = 0;
    };

    static constexpr std::uint32_t kWalMagic = 0x314C4157u;  // "WAL1"
    static constexpr std::uint32_t kSstMagic = 0x31545353u;  // "SST1"
    static constexpr std::string_view kManifestMagic = "ADMANIFEST 1";

    static void hit(CrashInjection* crash, std::string_view point);
    static std::string sst_name(std::uint64_t id);

    void recover();
    void load_manifest();
    void load_sst(std::uint64_t id);
    void replay_wal();
    void remove_extra_files();

    [[nodiscard]] std::string encode_wal_record(bool tombstone, std::string_view key,
                                                std::string_view value) const;
    [[nodiscard]] std::string serialize_sst(const std::vector<KVEntry>& entries) const;
    [[nodiscard]] std::string serialize_manifest() const;

    void append_wal(bool tombstone, std::string_view key, std::string_view value);
    void commit_manifest(std::string_view before_commit, std::string_view after_commit);
    void compact();
    void apply_memtable(const std::string& key, std::optional<std::string> value);
    [[nodiscard]] std::size_t entry_cost(const std::string& key,
                                        const std::optional<std::string>& value) const;
    [[nodiscard]] std::optional<std::string> lookup_in_ssts(const std::string& key) const;
    void ensure_open() const;

    Disk disk_;
    Options options_;
    CrashInjection* crash_ = nullptr;
    std::vector<SSTRef> manifest_;
    std::vector<SSTable> ssts_;
    Snapshot memtable_;
    std::size_t memtable_bytes_ = 0;
    std::uint64_t next_sst_id_ = 1;
    std::uint64_t wal_records_since_sync_ = 0;
    Stats stats_;
    bool closed_ = false;
};

} // namespace arch
