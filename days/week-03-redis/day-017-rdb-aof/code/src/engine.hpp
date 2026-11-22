// engine.hpp
//
// 把 PageStore、RDB、AOF、COW、rewrite 串起来的持久化引擎。
//
// 这是“门面（facade）”：命令层只调用 set/del/expire，
// 不关心 RDB 的字节格式、AOF 的 fsync 策略，也不关心 rewrite 的临时文件。

#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include "aof.hpp"
#include "common.hpp"
#include "mem_fs.hpp"
#include "page_store.hpp"
#include "rdb.hpp"

namespace archdesign {

struct EngineConfig {
    bool aof_enabled = true;
    std::size_t page_count = 16;
    std::string rdb_path = "dump.rdb";
    std::string aof_path = "appendonly.aof";
    AofConfig aof;
    RdbOptions rdb;
    std::size_t auto_rewrite_min_size = 1024;
    int auto_rewrite_percentage = 100;
};

struct EngineStats {
    std::string loaded_from = "empty";
    std::size_t loaded_keys = 0;
    std::size_t skipped_expired = 0;
    std::size_t aof_truncated_bytes = 0;
    std::size_t bgsaves = 0;
    std::size_t rewrites = 0;
    std::size_t rewrite_tail_commands = 0;
    std::size_t rewrite_tail_bytes = 0;
};

class Engine {
public:
    Engine(MemFs& fs, EngineConfig config);

    // ---- 命令层 ----
    void set(const std::string& key, const std::string& value, i64 now_ms, i64 ttl_ms = -1);
    bool del(const std::string& key, i64 now_ms);
    bool expire(const std::string& key, i64 now_ms, i64 ttl_ms);
    bool persist(const std::string& key, i64 now_ms);
    std::optional<Entry> get(const std::string& key, i64 now_ms) const;
    std::size_t size(i64 now_ms) const;

    // ---- RDB ----
    void save_rdb(i64 now_ms);
    void begin_bgsave(i64 now_ms);
    void finish_bgsave(i64 now_ms);
    bool bgsave_active() const;

    // ---- AOF rewrite ----
    void begin_rewrite(i64 now_ms);
    void finish_rewrite(i64 now_ms);
    void abort_rewrite();
    bool rewrite_active() const;
    std::size_t rewrite_tail_bytes() const;
    std::size_t rewrite_tail_commands() const;
    bool maybe_auto_rewrite(i64 now_ms);

    // ---- 启动恢复 ----
    void load(i64 now_ms);

    const PageStore& store() const;
    const EngineStats& stats() const;
    const EngineConfig& config() const;
    MemFs& fs();
    AofWriter& aof();

private:
    void append_to_aof(const Command& command, i64 now_ms);
    std::string encode_snapshot_as_aof(const Snapshot& snapshot) const;
    void write_rdb_snapshot(const Snapshot& snapshot);

    MemFs& fs_;
    EngineConfig config_;
    PageStore store_;
    AofWriter aof_;
    std::optional<Snapshot> bgsave_snapshot_;
    bool bgsave_active_ = false;
    std::optional<Snapshot> rewrite_snapshot_;
    std::string rewrite_base_;
    std::string rewrite_tail_;
    bool rewrite_active_ = false;
    EngineStats stats_;
};

}  // namespace archdesign
