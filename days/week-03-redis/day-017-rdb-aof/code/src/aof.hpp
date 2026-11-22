// aof.hpp
//
// AOF 写入：append + appendfsync。
//
// 主线程只负责把 RESP 命令 write 到文件；Always 立刻 fsync，
// EverySec 在 tick/flush 时按时间间隔 fsync（本实现是同步模拟，
// 真实 Redis 把 fsync 交给 bio 线程）。

#pragma once

#include <cstddef>
#include <string>

#include "common.hpp"
#include "mem_fs.hpp"
#include "page_store.hpp"
#include "rdb.hpp"
#include "resp.hpp"

namespace archdesign {

enum class FsyncPolicy { Always, EverySec, No };

const char* fsync_policy_name(FsyncPolicy policy);

struct AofConfig {
    FsyncPolicy fsync = FsyncPolicy::EverySec;
    i64 everysec_ms = 1000;
    bool use_rdb_preamble = true;
    bool load_truncated = true;
    RdbOptions rdb;
};

struct AofLoadResult {
    std::size_t commands = 0;
    std::size_t truncated_bytes = 0;
    bool used_rdb_preamble = false;
};

void apply_command(PageStore& store, const Command& command, i64 now_ms);

class AofWriter {
public:
    AofWriter(MemFs& fs, std::string path, AofConfig config);

    void mark_start(i64 now_ms);
    void append(const Command& command);
    void flush(i64 now_ms, bool force = false);
    void tick(i64 now_ms);
    void fsync_now(i64 now_ms);

    AofLoadResult load(PageStore& store, i64 now_ms);
    void reset_after_rewrite(i64 now_ms);

    std::size_t size() const;
    std::size_t base_size() const;
    std::size_t buffered_bytes() const;
    std::size_t appended_commands() const;
    std::size_t flushes() const;
    std::size_t fsyncs() const;
    const std::string& path() const;

private:
    void maybe_fsync(i64 now_ms, bool force);

    MemFs& fs_;
    std::string path_;
    AofConfig config_;
    std::string buffer_;
    i64 last_fsync_ms_ = 0;
    std::size_t base_size_ = 0;
    std::size_t appended_commands_ = 0;
    std::size_t flushes_ = 0;
    std::size_t fsyncs_ = 0;
};

}  // namespace archdesign
