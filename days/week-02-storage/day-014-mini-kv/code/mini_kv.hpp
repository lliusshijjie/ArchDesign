// mini_kv.hpp
//
// Day 14 周复盘：把 W2 的组件拼成一个可运行的 Mini KV Engine。
//
// 组件边界：
//   WAL        —— 追加写 + 恢复 + 部分尾记录截断
//   MemTable   —— 内存有序表 + tombstone
//   SSTable    —— 不可变有序文件
//   Compaction —— 全量合并，保留最新版本
//   MiniKV     —— 组合以上组件，暴露 put/get/erase/flush/close
//
// 这是“复盘 + 集成”，不是生产引擎：
//   - 单线程；
//   - 没有 Manifest/VersionSet；
//   - 没有分层 compaction；
//   - 没有 fsync/原子 rename；
//   - 没有 block index/Bloom filter；
//   - 没有 MVCC/事务。

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace archdesign {

namespace fs = std::filesystem;

using MemTable = std::map<std::string, std::optional<std::string>>;

struct KVEntry {
    std::string key;
    std::string value;
    bool tombstone = false;
};

struct RecoveryReport {
    std::uint64_t records = 0;
    std::uint64_t truncated_bytes = 0;
};

// ---------------------------------------------------------------- WAL

class Wal {
public:
    explicit Wal(fs::path path) : path_(std::move(path)) {}

    Wal(const Wal&) = delete;
    Wal& operator=(const Wal&) = delete;

    ~Wal() {
        close();
    }

    // 读取 WAL，把记录应用到 memtable；损坏/不完整的尾部会被截断。
    RecoveryReport recover(MemTable& memtable) {
        RecoveryReport report;
        if (!fs::exists(path_)) {
            return report;
        }

        const auto file_size = fs::file_size(path_);
        std::ifstream in(path_, std::ios::binary);
        if (!in) {
            throw std::runtime_error("cannot open WAL for recovery");
        }

        std::uint64_t valid_bytes = 0;
        for (;;) {
            std::uint32_t key_size = 0;
            std::uint32_t value_size = 0;
            std::uint8_t tombstone = 0;
            std::uint32_t checksum = 0;

            if (!in.read(reinterpret_cast<char*>(&key_size), sizeof(key_size)) ||
                !in.read(reinterpret_cast<char*>(&value_size), sizeof(value_size)) ||
                !in.read(reinterpret_cast<char*>(&tombstone), sizeof(tombstone)) ||
                !in.read(reinterpret_cast<char*>(&checksum), sizeof(checksum))) {
                break;
            }

            if (key_size > (1u << 20) || value_size > (1u << 28)) {
                break;  // 防御异常长度
            }

            std::string key(key_size, '\0');
            std::string value(value_size, '\0');
            if (!in.read(key.data(), static_cast<std::streamsize>(key_size)) ||
                !in.read(value.data(), static_cast<std::streamsize>(value_size))) {
                break;
            }

            const std::uint32_t actual = fnv1a(tombstone != 0, key, value);
            if (actual != checksum) {
                break;  // 校验失败：尾部损坏，保守截断
            }

            memtable[key] = tombstone
                                ? std::nullopt
                                : std::optional<std::string>(std::move(value));
            ++report.records;
            valid_bytes += 4 + 4 + 1 + 4 + key_size + value_size;
        }

        in.close();
        if (valid_bytes < file_size) {
            report.truncated_bytes = file_size - valid_bytes;
            fs::resize_file(path_, valid_bytes);
        }
        return report;
    }

    void open_append() {
        out_.open(path_, std::ios::binary | std::ios::app);
        if (!out_) {
            throw std::runtime_error("cannot open WAL for append");
        }
    }

    void append(bool tombstone, std::string_view key, std::string_view value) {
        if (!out_.is_open()) {
            throw std::logic_error("WAL is not open");
        }
        if (key.size() > (1u << 20) || value.size() > (1u << 28)) {
            throw std::length_error("key/value too large");
        }

        const auto key_size = static_cast<std::uint32_t>(key.size());
        const auto value_size = static_cast<std::uint32_t>(value.size());
        const std::uint8_t flag = tombstone ? 1 : 0;
        const std::uint32_t checksum = fnv1a(tombstone, key, value);

        out_.write(reinterpret_cast<const char*>(&key_size), sizeof(key_size));
        out_.write(reinterpret_cast<const char*>(&value_size), sizeof(value_size));
        out_.write(reinterpret_cast<const char*>(&flag), sizeof(flag));
        out_.write(reinterpret_cast<const char*>(&checksum), sizeof(checksum));
        out_.write(key.data(), static_cast<std::streamsize>(key.size()));
        out_.write(value.data(), static_cast<std::streamsize>(value.size()));
        out_.flush();
        if (!out_) {
            throw std::runtime_error("WAL append failed");
        }
    }

    void reset() {
        close();
        std::ofstream truncate(path_, std::ios::binary | std::ios::trunc);
        truncate.close();
        open_append();
    }

    void close() {
        if (out_.is_open()) {
            out_.flush();
            out_.close();
        }
    }

private:
    static std::uint32_t fnv1a(bool tombstone, std::string_view key,
                               std::string_view value) {
        std::uint32_t hash = 2166136261u;
        const auto mix = [&](std::uint8_t byte) {
            hash ^= byte;
            hash *= 16777619u;
        };
        mix(tombstone ? 1u : 0u);
        for (const unsigned char byte : key) {
            mix(byte);
        }
        for (const unsigned char byte : value) {
            mix(byte);
        }
        return hash;
    }

    fs::path path_;
    std::ofstream out_;
};

// ---------------------------------------------------------------- SSTable

struct SSTable {
    std::uint64_t id = 0;
    fs::path path;
    std::vector<KVEntry> entries;
};

// ---------------------------------------------------------------- MiniKV

class MiniKV {
public:
    struct Options {
        std::size_t memtable_limit = 4096;
        std::size_t compaction_trigger = 3;
    };

    struct Stats {
        std::uint64_t puts = 0;
        std::uint64_t removes = 0;
        std::uint64_t flushes = 0;
        std::uint64_t compactions = 0;
        std::uint64_t recovered_wal_records = 0;
        std::uint64_t truncated_wal_bytes = 0;
        std::size_t memtable_bytes = 0;
        std::size_t sstables = 0;
    };

    explicit MiniKV(fs::path directory)
        : MiniKV(std::move(directory), Options{}) {}

    MiniKV(fs::path directory, Options options)
        : directory_(std::move(directory)),
          wal_(directory_ / "wal.log"),
          options_(options) {
        if (options_.memtable_limit == 0) {
            throw std::invalid_argument("memtable_limit must be > 0");
        }
        if (options_.compaction_trigger < 2) {
            throw std::invalid_argument("compaction_trigger must be >= 2");
        }

        std::error_code ec;
        fs::create_directories(directory_, ec);
        if (ec) {
            throw std::runtime_error("cannot create directory: " + ec.message());
        }

        load_sstables();
        const auto report = wal_.recover(memtable_);
        stats_.recovered_wal_records = report.records;
        stats_.truncated_wal_bytes = report.truncated_bytes;
        wal_.open_append();
        rebuild_memtable_bytes();
    }

    ~MiniKV() {
        // 只关闭 WAL，不强制 flush：模拟进程退出，靠 WAL 恢复。
        wal_.close();
    }

    MiniKV(const MiniKV&) = delete;
    MiniKV& operator=(const MiniKV&) = delete;

    void put(const std::string& key, const std::string& value) {
        ensure_open();
        wal_.append(false, key, value);
        set_memtable(key, value);
        ++stats_.puts;
        maybe_flush();
    }

    void erase(const std::string& key) {
        ensure_open();
        wal_.append(true, key, "");
        set_memtable(key, std::nullopt);
        ++stats_.removes;
        maybe_flush();
    }

    [[nodiscard]] std::optional<std::string> get(const std::string& key) const {
        const auto mem = memtable_.find(key);
        if (mem != memtable_.end()) {
            return mem->second;
        }
        for (auto it = sstables_.rbegin(); it != sstables_.rend(); ++it) {
            const auto found = std::lower_bound(
                it->entries.begin(), it->entries.end(), key,
                [](const KVEntry& entry, const std::string& value) {
                    return entry.key < value;
                });
            if (found != it->entries.end() && found->key == key) {
                return found->tombstone
                           ? std::nullopt
                           : std::optional<std::string>(found->value);
            }
        }
        return std::nullopt;
    }

    void flush() {
        ensure_open();
        if (memtable_.empty()) {
            return;
        }
        auto sst = write_sst(memtable_);
        sstables_.push_back(std::move(sst));
        memtable_.clear();
        memtable_bytes_ = 0;
        wal_.reset();
        ++stats_.flushes;
        if (sstables_.size() >= options_.compaction_trigger) {
            compact();
        }
    }

    void close() {
        if (closed_) {
            return;
        }
        flush();
        wal_.close();
        closed_ = true;
    }

    [[nodiscard]] Stats stats() const {
        Stats result = stats_;
        result.memtable_bytes = memtable_bytes_;
        result.sstables = sstables_.size();
        return result;
    }

private:
    static constexpr std::uint32_t kSstMagic = 0x3153564bu;  // "KVS1"

    void ensure_open() const {
        if (closed_) {
            throw std::logic_error("MiniKV is closed");
        }
    }

    static std::size_t estimate(const std::string& key,
                                const std::optional<std::string>& value) {
        return key.size() + (value.has_value() ? value->size() : 0) + 32;
    }

    void set_memtable(const std::string& key, std::optional<std::string> value) {
        const auto it = memtable_.find(key);
        if (it != memtable_.end()) {
            memtable_bytes_ -= estimate(it->first, it->second);
        }
        memtable_[key] = std::move(value);
        memtable_bytes_ += estimate(key, memtable_[key]);
    }

    void rebuild_memtable_bytes() {
        memtable_bytes_ = 0;
        for (const auto& [key, value] : memtable_) {
            memtable_bytes_ += estimate(key, value);
        }
    }

    void maybe_flush() {
        if (memtable_bytes_ >= options_.memtable_limit) {
            flush();
        }
    }

    fs::path sst_path(std::uint64_t id) const {
        char name[32];
        std::snprintf(name, sizeof(name), "sst-%06llu.dat",
                      static_cast<unsigned long long>(id));
        return directory_ / name;
    }

    SSTable write_sst(const MemTable& memtable) {
        SSTable sst;
        sst.id = next_sst_id_++;
        sst.path = sst_path(sst.id);

        for (const auto& [key, value] : memtable) {
            sst.entries.push_back(
                KVEntry{key, value.value_or(""), !value.has_value()});
        }

        std::ofstream out(sst.path, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("cannot create SST");
        }
        const auto count = static_cast<std::uint32_t>(sst.entries.size());
        out.write(reinterpret_cast<const char*>(&kSstMagic), sizeof(kSstMagic));
        out.write(reinterpret_cast<const char*>(&count), sizeof(count));
        for (const auto& entry : sst.entries) {
            const auto key_size = static_cast<std::uint32_t>(entry.key.size());
            const auto value_size = static_cast<std::uint32_t>(entry.value.size());
            const std::uint8_t tombstone = entry.tombstone ? 1 : 0;
            out.write(reinterpret_cast<const char*>(&key_size), sizeof(key_size));
            out.write(reinterpret_cast<const char*>(&value_size), sizeof(value_size));
            out.write(reinterpret_cast<const char*>(&tombstone), sizeof(tombstone));
            out.write(entry.key.data(), static_cast<std::streamsize>(entry.key.size()));
            out.write(entry.value.data(),
                      static_cast<std::streamsize>(entry.value.size()));
        }
        out.flush();
        if (!out) {
            std::error_code ec;
            fs::remove(sst.path, ec);
            throw std::runtime_error("failed to write SST");
        }
        return sst;
    }

    SSTable load_sst(const fs::path& path) const {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            throw std::runtime_error("cannot open SST: " + path.string());
        }
        std::uint32_t magic = 0;
        std::uint32_t count = 0;
        in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        in.read(reinterpret_cast<char*>(&count), sizeof(count));
        if (magic != kSstMagic) {
            throw std::runtime_error("bad SST magic");
        }

        SSTable sst;
        sst.path = path;
        sst.entries.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t key_size = 0;
            std::uint32_t value_size = 0;
            std::uint8_t tombstone = 0;
            in.read(reinterpret_cast<char*>(&key_size), sizeof(key_size));
            in.read(reinterpret_cast<char*>(&value_size), sizeof(value_size));
            in.read(reinterpret_cast<char*>(&tombstone), sizeof(tombstone));
            KVEntry entry;
            entry.key.resize(key_size);
            entry.value.resize(value_size);
            entry.tombstone = tombstone != 0;
            in.read(entry.key.data(), static_cast<std::streamsize>(key_size));
            in.read(entry.value.data(), static_cast<std::streamsize>(value_size));
            if (!in) {
                throw std::runtime_error("truncated SST");
            }
            sst.entries.push_back(std::move(entry));
        }
        return sst;
    }

    void load_sstables() {
        std::vector<std::pair<std::uint64_t, fs::path>> files;
        for (const auto& item : fs::directory_iterator(directory_)) {
            if (!item.is_regular_file()) {
                continue;
            }
            const auto name = item.path().filename().string();
            if (name.rfind("sst-", 0) != 0 ||
                item.path().extension() != ".dat") {
                continue;
            }
            const auto id_text = name.substr(4, name.find('.') - 4);
            try {
                files.emplace_back(std::stoull(id_text), item.path());
            } catch (...) {
                // 忽略非本实现命名的文件
            }
        }
        std::sort(files.begin(), files.end());
        for (const auto& [id, path] : files) {
            auto sst = load_sst(path);
            sst.id = id;
            next_sst_id_ = std::max(next_sst_id_, id + 1);
            sstables_.push_back(std::move(sst));
        }
    }

    void compact() {
        if (sstables_.size() < 2) {
            return;
        }

        MemTable merged;
        for (auto it = sstables_.rbegin(); it != sstables_.rend(); ++it) {
            for (const auto& entry : it->entries) {
                if (merged.find(entry.key) != merged.end()) {
                    continue;
                }
                merged.emplace(entry.key,
                               entry.tombstone
                                   ? std::nullopt
                                   : std::optional<std::string>(entry.value));
            }
        }

        const auto old_ssts = sstables_;
        auto compacted = write_sst(merged);
        sstables_.clear();
        if (!compacted.entries.empty()) {
            sstables_.push_back(std::move(compacted));
        }
        for (const auto& old : old_ssts) {
            std::error_code ec;
            fs::remove(old.path, ec);
        }
        ++stats_.compactions;
    }

    fs::path directory_;
    Wal wal_;
    Options options_;
    MemTable memtable_;
    std::vector<SSTable> sstables_;
    std::size_t memtable_bytes_ = 0;
    std::uint64_t next_sst_id_ = 1;
    mutable Stats stats_;
    bool closed_ = false;
};

} // namespace archdesign
