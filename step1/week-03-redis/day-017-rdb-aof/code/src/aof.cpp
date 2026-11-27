#include "aof.hpp"

#include <stdexcept>
#include <utility>

namespace archdesign {

const char* fsync_policy_name(FsyncPolicy policy) {
    switch (policy) {
        case FsyncPolicy::Always: return "always";
        case FsyncPolicy::EverySec: return "everysec";
        case FsyncPolicy::No: return "no";
    }
    return "unknown";
}

void apply_command(PageStore& store, const Command& command, i64 now_ms) {
    if (command.args.empty()) throw std::runtime_error("AOF: empty command");
    const std::string verb = upper_ascii(command.args[0]);

    if (verb == "SET") {
        if (command.args.size() != 3 && command.args.size() != 5) {
            throw std::runtime_error("AOF: SET expects 3 or 5 arguments");
        }
        i64 expire_at = -1;
        if (command.args.size() == 5) {
            const std::string option = upper_ascii(command.args[3]);
            const i64 value = parse_i64(command.args[4], "SET expire");
            if (option == "PXAT") {
                expire_at = value;
            } else if (option == "EXAT") {
                expire_at = value * 1000;
            } else if (option == "PX") {
                expire_at = now_ms + value;
            } else if (option == "EX") {
                expire_at = now_ms + value * 1000;
            } else {
                throw std::runtime_error("AOF: unsupported SET option " + option);
            }
        }
        if (expire_at != -1 && expire_at <= now_ms) {
            store.del(command.args[1]);
        } else {
            store.set(command.args[1], command.args[2], expire_at);
        }
        return;
    }

    if (verb == "DEL" || verb == "UNLINK") {
        for (std::size_t i = 1; i < command.args.size(); ++i) store.del(command.args[i]);
        return;
    }

    if (verb == "PEXPIREAT") {
        if (command.args.size() != 3) throw std::runtime_error("AOF: PEXPIREAT expects 2 arguments");
        const i64 deadline = parse_i64(command.args[2], "PEXPIREAT deadline");
        if (deadline <= now_ms) {
            store.del(command.args[1]);
        } else {
            store.expire_at(command.args[1], deadline);
        }
        return;
    }

    if (verb == "PEXPIRE") {
        if (command.args.size() != 3) throw std::runtime_error("AOF: PEXPIRE expects 2 arguments");
        const i64 ttl = parse_i64(command.args[2], "PEXPIRE ttl");
        if (ttl <= 0) {
            store.del(command.args[1]);
        } else {
            store.expire_at(command.args[1], now_ms + ttl);
        }
        return;
    }

    if (verb == "PERSIST") {
        if (command.args.size() != 2) throw std::runtime_error("AOF: PERSIST expects 1 argument");
        (void)store.persist(command.args[1]);
        return;
    }

    if (verb == "SELECT" || verb == "AOF-TIMESTAMP") {
        return;  // 单 DB 实现，忽略 SELECT；时间戳注释已由 parser 跳过
    }

    if (verb == "FLUSHALL" || verb == "FLUSHDB") {
        store.clear();
        return;
    }

    throw std::runtime_error("AOF: unsupported command " + verb);
}

AofWriter::AofWriter(MemFs& fs, std::string path, AofConfig config)
    : fs_(fs), path_(std::move(path)), config_(std::move(config)) {}

void AofWriter::mark_start(i64 now_ms) { last_fsync_ms_ = now_ms; }

void AofWriter::append(const Command& command) {
    buffer_ += resp_encode(command);
    ++appended_commands_;
}

void AofWriter::flush(i64 now_ms, bool force) {
    if (buffer_.empty()) {
        maybe_fsync(now_ms, force);
        return;
    }
    fs_.append(path_, buffer_);
    buffer_.clear();
    ++flushes_;
    maybe_fsync(now_ms, force);
}

void AofWriter::tick(i64 now_ms) { flush(now_ms, false); }

void AofWriter::fsync_now(i64 now_ms) {
    fs_.fsync(path_);
    ++fsyncs_;
    last_fsync_ms_ = now_ms;
}

AofLoadResult AofWriter::load(PageStore& store, i64 now_ms) {
    AofLoadResult result;
    const std::string data = fs_.read(path_);
    std::size_t offset = 0;

    if (data.size() >= 5 && std::string_view(data).substr(0, 5) == "REDIS") {
        RdbLoadOptions options;
        options.verify_checksum = config_.rdb.checksum;
        options.skip_expired = false;  // AOF preamble：由后面的命令决定这些键的命运
        auto rdb = rdb_decode(data, store, now_ms, options);
        offset = rdb.consumed;
        result.used_rdb_preamble = true;
    }

    const RespParseResult parsed = resp_parse(std::string_view(data).substr(offset));
    for (const Command& command : parsed.commands) apply_command(store, command, now_ms);
    result.commands = parsed.commands.size();

    if (parsed.truncated) {
        if (!config_.load_truncated) {
            throw std::runtime_error("AOF: truncated tail (aof-load-truncated is disabled)");
        }
        const std::size_t valid = offset + parsed.consumed;
        result.truncated_bytes = data.size() - valid;
        fs_.truncate(path_, valid);  // 只保留最后一个完整命令
        fs_.fsync(path_);            // 把截断结果落盘，避免下次启动重复报警
    }

    base_size_ = fs_.size(path_);
    return result;
}

void AofWriter::reset_after_rewrite(i64 now_ms) {
    base_size_ = fs_.size(path_);
    last_fsync_ms_ = now_ms;
    buffer_.clear();
}

std::size_t AofWriter::size() const { return fs_.size(path_); }

std::size_t AofWriter::base_size() const { return base_size_; }

std::size_t AofWriter::buffered_bytes() const { return buffer_.size(); }

std::size_t AofWriter::appended_commands() const { return appended_commands_; }

std::size_t AofWriter::flushes() const { return flushes_; }

std::size_t AofWriter::fsyncs() const { return fsyncs_; }

const std::string& AofWriter::path() const { return path_; }

void AofWriter::maybe_fsync(i64 now_ms, bool force) {
    if (config_.fsync == FsyncPolicy::Always) {
        fs_.fsync(path_);
        ++fsyncs_;
        last_fsync_ms_ = now_ms;
        return;
    }
    if (config_.fsync == FsyncPolicy::EverySec &&
        (force || now_ms - last_fsync_ms_ >= config_.everysec_ms)) {
        fs_.fsync(path_);
        ++fsyncs_;
        last_fsync_ms_ = now_ms;
    }
}

}  // namespace archdesign
