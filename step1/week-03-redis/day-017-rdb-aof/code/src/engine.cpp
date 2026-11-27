#include "engine.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace archdesign {

Engine::Engine(MemFs& fs, EngineConfig config)
    : fs_(fs),
      config_(std::move(config)),
      store_(config_.page_count),
      aof_(fs_, config_.aof_path, config_.aof) {}

void Engine::set(const std::string& key, const std::string& value, i64 now_ms, i64 ttl_ms) {
    const i64 deadline = ttl_ms < 0 ? -1 : now_ms + ttl_ms;
    store_.set(key, value, deadline);
    Command command{"SET", key, value};
    if (deadline != -1) {
        command.args.push_back("PXAT");
        command.args.push_back(std::to_string(deadline));
    }
    append_to_aof(command, now_ms);
}

bool Engine::del(const std::string& key, i64 now_ms) {
    if (!store_.del(key)) return false;
    append_to_aof(Command{"DEL", key}, now_ms);
    return true;
}

bool Engine::expire(const std::string& key, i64 now_ms, i64 ttl_ms) {
    if (ttl_ms <= 0) {
        if (!store_.del(key)) return false;
        append_to_aof(Command{"DEL", key}, now_ms);
        return true;
    }
    if (!store_.expire_at(key, now_ms + ttl_ms)) return false;
    append_to_aof(Command{"PEXPIREAT", key, std::to_string(now_ms + ttl_ms)}, now_ms);
    return true;
}

bool Engine::persist(const std::string& key, i64 now_ms) {
    if (!store_.persist(key)) return false;
    append_to_aof(Command{"PERSIST", key}, now_ms);
    return true;
}

std::optional<Entry> Engine::get(const std::string& key, i64 now_ms) const {
    return store_.get_live(key, now_ms);
}

std::size_t Engine::size(i64 now_ms) const { return store_.live_key_count(now_ms); }

void Engine::save_rdb(i64 now_ms) {
    (void)now_ms;
    const Snapshot snapshot = store_.fork();
    write_rdb_snapshot(snapshot);
}

void Engine::begin_bgsave(i64 now_ms) {
    (void)now_ms;
    if (bgsave_active_) throw std::runtime_error("BGSAVE already in progress");
    bgsave_snapshot_ = store_.fork();
    bgsave_active_ = true;
}

void Engine::finish_bgsave(i64 now_ms) {
    (void)now_ms;
    if (!bgsave_active_) throw std::runtime_error("no BGSAVE in progress");
    write_rdb_snapshot(*bgsave_snapshot_);
    bgsave_active_ = false;
    bgsave_snapshot_.reset();
    ++stats_.bgsaves;
}

bool Engine::bgsave_active() const { return bgsave_active_; }

void Engine::begin_rewrite(i64 now_ms) {
    (void)now_ms;
    if (!config_.aof_enabled) throw std::runtime_error("AOF is disabled");
    if (rewrite_active_) throw std::runtime_error("AOF rewrite already in progress");
    rewrite_snapshot_ = store_.fork();
    rewrite_base_ = config_.aof.use_rdb_preamble
                        ? rdb_encode(*rewrite_snapshot_, config_.rdb)
                        : encode_snapshot_as_aof(*rewrite_snapshot_);
    rewrite_tail_.clear();
    rewrite_active_ = true;
}

void Engine::finish_rewrite(i64 now_ms) {
    if (!rewrite_active_) throw std::runtime_error("no AOF rewrite in progress");
    aof_.flush(now_ms, true);  // 确保旧 AOF 的尾部已经 write；新文件不依赖它

    const std::string new_aof = rewrite_base_ + rewrite_tail_;
    const std::string temp = "temp-rewrite.aof";
    try {
        fs_.write_file(temp, new_aof);
        fs_.fsync(temp);
        fs_.rename(temp, config_.aof_path);
    } catch (...) {
        if (fs_.exists(temp)) fs_.remove(temp);
        throw;
    }

    aof_.reset_after_rewrite(now_ms);
    rewrite_active_ = false;
    rewrite_base_.clear();
    rewrite_tail_.clear();
    rewrite_snapshot_.reset();
    ++stats_.rewrites;
}

void Engine::abort_rewrite() {
    rewrite_active_ = false;
    rewrite_base_.clear();
    rewrite_tail_.clear();
    rewrite_snapshot_.reset();
}

bool Engine::rewrite_active() const { return rewrite_active_; }

std::size_t Engine::rewrite_tail_bytes() const { return rewrite_tail_.size(); }

std::size_t Engine::rewrite_tail_commands() const { return stats_.rewrite_tail_commands; }

bool Engine::maybe_auto_rewrite(i64 now_ms) {
    if (!config_.aof_enabled || rewrite_active_) return false;
    const std::size_t current = aof_.size();
    if (current < config_.auto_rewrite_min_size) return false;
    const std::size_t base = std::max<std::size_t>(aof_.base_size(), 1);
    const u64 growth = current > base ? static_cast<u64>((current - base) * 100 / base) : 0;
    if (growth < static_cast<u64>(config_.auto_rewrite_percentage)) return false;
    begin_rewrite(now_ms);
    return true;
}

void Engine::load(i64 now_ms) {
    store_.clear();
    aof_.mark_start(now_ms);
    stats_ = EngineStats{};

    const bool aof_exists = fs_.exists(config_.aof_path) && fs_.size(config_.aof_path) > 0;
    if (config_.aof_enabled && aof_exists) {
        const AofLoadResult result = aof_.load(store_, now_ms);
        stats_.loaded_from = result.used_rdb_preamble ? "aof (rdb preamble)" : "aof";
        stats_.loaded_keys = store_.live_key_count(now_ms);
        stats_.aof_truncated_bytes = result.truncated_bytes;
        return;
    }

    if (fs_.exists(config_.rdb_path)) {
        const std::string data = fs_.read(config_.rdb_path);
        RdbLoadOptions options;
        options.verify_checksum = config_.rdb.checksum;
        const RdbDecodeResult result = rdb_decode(data, store_, now_ms, options);
        if (result.consumed != data.size()) {
            throw std::runtime_error("RDB: trailing bytes after checksum");
        }
        stats_.loaded_from = "rdb";
        stats_.loaded_keys = result.loaded;
        stats_.skipped_expired = result.skipped_expired;
        return;
    }

    stats_.loaded_from = "empty";
}

const PageStore& Engine::store() const { return store_; }

const EngineStats& Engine::stats() const { return stats_; }

const EngineConfig& Engine::config() const { return config_; }

MemFs& Engine::fs() { return fs_; }

AofWriter& Engine::aof() { return aof_; }

void Engine::append_to_aof(const Command& command, i64 now_ms) {
    if (!config_.aof_enabled) return;
    aof_.append(command);
    aof_.flush(now_ms);
    if (rewrite_active_) {
        rewrite_tail_ += resp_encode(command);
        ++stats_.rewrite_tail_commands;
        stats_.rewrite_tail_bytes = rewrite_tail_.size();
    }
}

std::string Engine::encode_snapshot_as_aof(const Snapshot& snapshot) const {
    std::string out;
    snapshot.for_each([&](const std::string& key, const Entry& entry) {
        out += resp_encode(Command{"SET", key, entry.value});
        if (entry.expire_at_ms != -1) {
            out += resp_encode(Command{"PEXPIREAT", key, std::to_string(entry.expire_at_ms)});
        }
    });
    return out;
}

void Engine::write_rdb_snapshot(const Snapshot& snapshot) {
    const std::string data = rdb_encode(snapshot, config_.rdb);
    const std::string temp = "temp-dump.rdb";
    try {
        fs_.write_file(temp, data);
        fs_.fsync(temp);
        fs_.rename(temp, config_.rdb_path);
    } catch (...) {
        if (fs_.exists(temp)) fs_.remove(temp);
        throw;
    }
}

}  // namespace archdesign
