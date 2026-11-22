// persistence.hpp
//
// Day 17 的最小实现：Redis 风格的 RDB 快照 + AOF 日志 + fork/COW 模型 + AOF rewrite。
//
// 目标不是复刻 redis/src/rdb.c 与 aof.c 的每一行，而是把四条真正可迁移的机制
// 做成能跑、能测、能破坏的结构：
//
//   1. RDB：把某一时刻的键空间编码成「魔数 + 元数据 + 键值 + EOF + CRC64」；
//      先写临时文件、fsync、再 rename，避免用半个文件覆盖好文件。
//   2. fork/COW：快照只复制页表（本实现复制 shared_ptr 数组）；父进程之后写哪一页，
//      才复制哪一页。子进程看到的是 fork 那一刻的稳定视图。
//   3. AOF：把写命令按 RESP 追加到日志；appendfsync 决定 write 之后何时 fsync。
//   4. AOF rewrite：子进程从快照生成「最小命令集」，父进程把 rewrite 期间的新命令
//      记到 tail；完成后把 base + tail 原子替换旧 AOF。本文实现的是 Redis < 7.0 的
//      rewrite buffer 模型；Redis 7.x 的 multi-part AOF 在文章第 3.7 节对照。
//
// 刻意的简化（文章 6.9 节会逐条列出）：
//   - 用 MemFs 模拟文件系统与 page cache；crash() 会丢掉没有 fsync 的尾部。
//   - 单线程、无事件循环、无 bio 线程；AOF everysec 的 fsync 在 tick() 里同步执行。
//   - 用固定数量的页 + shared_ptr 模拟 COW，不模拟真实页表、THP 与 RSS。
//   - 只支持 string 类型与 SET/DEL/PEXPIREAT/PERSIST，足够覆盖持久化不变量。
//   - AOF rewrite 的 tail 在内存里累积，正是 Redis 6.x 的 rewrite buffer；
//     Redis 7.x 改为新的 incr AOF 文件，见文章。

#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace archdesign {

using i64 = std::int64_t;
using u64 = std::uint64_t;
using u32 = std::uint32_t;
using u8 = std::uint8_t;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

inline void put_u32_be(std::string& out, u32 v) {
    out.push_back(static_cast<char>((v >> 24) & 0xff));
    out.push_back(static_cast<char>((v >> 16) & 0xff));
    out.push_back(static_cast<char>((v >> 8) & 0xff));
    out.push_back(static_cast<char>(v & 0xff));
}

inline void put_u64_le(std::string& out, u64 v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

inline void put_u64_be(std::string& out, u64 v) {
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

inline u32 read_u32_be(std::string_view data, std::size_t pos) {
    if (pos + 4 > data.size()) throw std::runtime_error("read_u32_be: out of bounds");
    return (static_cast<u32>(static_cast<u8>(data[pos])) << 24) |
           (static_cast<u32>(static_cast<u8>(data[pos + 1])) << 16) |
           (static_cast<u32>(static_cast<u8>(data[pos + 2])) << 8) |
           static_cast<u32>(static_cast<u8>(data[pos + 3]));
}

inline u64 read_u64_le(std::string_view data, std::size_t pos) {
    if (pos + 8 > data.size()) throw std::runtime_error("read_u64_le: out of bounds");
    u64 v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<u64>(static_cast<u8>(data[pos + static_cast<std::size_t>(i)])) << (8 * i);
    }
    return v;
}

inline u64 read_u64_be(std::string_view data, std::size_t pos) {
    if (pos + 8 > data.size()) throw std::runtime_error("read_u64_be: out of bounds");
    u64 v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | static_cast<u64>(static_cast<u8>(data[pos + static_cast<std::size_t>(i)]));
    }
    return v;
}

inline std::string upper_ascii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

inline i64 parse_i64(std::string_view text, const char* what) {
    i64 value = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        throw std::runtime_error(std::string("invalid integer for ") + what + ": '" + std::string(text) + "'");
    }
    return value;
}

// Redis 使用的 CRC64（Jones 多项式，RefIn/RefOut，XorIn=~0）。
// 已知测试向量：crc64("123456789") == 0xe9c6d914c4b8d9ca。
inline u64 crc64(std::string_view data) {
    constexpr u64 kPoly = 0xad93d23594c935a9ULL;
    u64 crc = 0;
    for (unsigned char c : data) {
        for (unsigned mask = 0x01; mask & 0xff; mask <<= 1) {
            u64 bit = crc & 0x8000000000000000ULL;
            if ((c & mask) != 0) bit = !bit;
            crc <<= 1;
            if (bit != 0) crc ^= kPoly;
        }
    }
    // RefOut=true：把 64 位结果反转。
    u64 reflected = 0;
    for (int i = 0; i < 64; ++i) {
        reflected = (reflected << 1) | (crc & 1ULL);
        crc >>= 1;
    }
    return reflected;
}

// ---------------------------------------------------------------------------
// 页式键空间：fork/COW 的模型
//
// 每个 key 通过 FNV-1a 映射到固定数量的页。写一个 key 时，如果这一页正被某个
// 快照共享（use_count > 1），就先复制整页，再改副本。快照持有旧的 shared_ptr，
// 所以它永远看到 fork 那一刻的内容。
// ---------------------------------------------------------------------------

struct Entry {
    std::string value;
    i64 expire_at_ms = -1;  // -1 = 无 TTL；否则是绝对毫秒时间戳
};

struct Page {
    std::map<std::string, Entry> entries;
};

inline std::size_t page_bytes(const Page& page) {
    std::size_t total = 0;
    for (const auto& [key, entry] : page.entries) {
        // 近似账：key/value 字节 + map 节点/对象头的固定开销。
        total += key.size() + entry.value.size() + 96;
    }
    return total;
}

struct CowStats {
    u64 snapshots = 0;
    u64 pages_copied = 0;
    u64 bytes_copied = 0;
    u64 writes = 0;
};

class Snapshot {
public:
    Snapshot() = default;

    Snapshot(std::vector<std::shared_ptr<const Page>> pages, u64 version, std::size_t page_count)
        : pages_(std::move(pages)), version_(version), page_count_(page_count) {}

    std::size_t page_count() const { return page_count_; }
    u64 version() const { return version_; }

    std::size_t key_count() const {
        std::size_t total = 0;
        for (const auto& page : pages_) total += page->entries.size();
        return total;
    }

    std::size_t expires_count() const {
        std::size_t total = 0;
        for (const auto& page : pages_) {
            for (const auto& [key, entry] : page->entries) {
                (void)key;
                if (entry.expire_at_ms != -1) ++total;
            }
        }
        return total;
    }

    template <class Fn>
    void for_each(Fn&& fn) const {
        for (const auto& page : pages_) {
            for (const auto& [key, entry] : page->entries) fn(key, entry);
        }
    }

private:
    std::vector<std::shared_ptr<const Page>> pages_;
    u64 version_ = 0;
    std::size_t page_count_ = 0;
};

class PageStore {
public:
    explicit PageStore(std::size_t page_count = 16) : page_count_(page_count == 0 ? 1 : page_count) {
        pages_.reserve(page_count_);
        for (std::size_t i = 0; i < page_count_; ++i) pages_.push_back(std::make_shared<Page>());
    }

    std::optional<Entry> get(std::string_view key) const {
        const Page& page = *pages_[page_of(key)];
        auto it = page.entries.find(std::string(key));
        if (it == page.entries.end()) return std::nullopt;
        return it->second;
    }

    // 读路径的「逻辑过期」：过期键返回 nullopt，但物理上仍然占着页。
    std::optional<Entry> get_live(std::string_view key, i64 now_ms) const {
        auto entry = get(key);
        if (!entry) return std::nullopt;
        if (entry->expire_at_ms != -1 && entry->expire_at_ms <= now_ms) return std::nullopt;
        return entry;
    }

    void set(std::string key, std::string value, i64 expire_at_ms = -1) {
        Page& page = writable_page(page_of(key));
        auto [it, inserted] = page.entries.try_emplace(std::move(key), Entry{});
        (void)inserted;
        it->second.value = std::move(value);
        it->second.expire_at_ms = expire_at_ms;
        ++stats_.writes;
    }

    bool del(std::string_view key) {
        Page& page = writable_page(page_of(key));
        return page.entries.erase(std::string(key)) != 0;
    }

    bool expire_at(std::string_view key, i64 when_ms) {
        Page& page = writable_page(page_of(key));
        auto it = page.entries.find(std::string(key));
        if (it == page.entries.end()) return false;
        it->second.expire_at_ms = when_ms;
        ++stats_.writes;
        return true;
    }

    bool persist(std::string_view key) {
        Page& page = writable_page(page_of(key));
        auto it = page.entries.find(std::string(key));
        if (it == page.entries.end() || it->second.expire_at_ms == -1) return false;
        it->second.expire_at_ms = -1;
        ++stats_.writes;
        return true;
    }

    Snapshot fork() const {
        ++stats_.snapshots;
        std::vector<std::shared_ptr<const Page>> pages;
        pages.reserve(pages_.size());
        for (const auto& page : pages_) pages.push_back(page);
        return Snapshot(std::move(pages), version_++, pages_.size());
    }

    std::size_t key_count() const {
        std::size_t total = 0;
        for (const auto& page : pages_) total += page->entries.size();
        return total;
    }

    std::size_t live_key_count(i64 now_ms) const {
        std::size_t total = 0;
        for (const auto& page : pages_) {
            for (const auto& [key, entry] : page->entries) {
                (void)key;
                if (entry.expire_at_ms == -1 || entry.expire_at_ms > now_ms) ++total;
            }
        }
        return total;
    }

    std::size_t page_of(std::string_view key) const {
        u64 hash = 1469598103934665603ULL;  // FNV offset basis
        for (unsigned char c : key) {
            hash ^= c;
            hash *= 1099511628211ULL;  // FNV prime
        }
        return static_cast<std::size_t>(hash % page_count_);
    }

    std::vector<std::string> keys() const {
        std::vector<std::string> out;
        out.reserve(key_count());
        for (const auto& page : pages_) {
            for (const auto& [key, entry] : page->entries) {
                (void)entry;
                out.push_back(key);
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    void clear() {
        for (auto& page : pages_) page = std::make_shared<Page>();
        version_ = 0;
    }

    const CowStats& stats() const { return stats_; }
    void reset_stats() { stats_ = CowStats{}; }
    std::size_t page_count() const { return page_count_; }

private:
    Page& writable_page(std::size_t index) {
        auto& slot = pages_[index];
        if (slot.use_count() > 1) {
            auto copy = std::make_shared<Page>(*slot);
            stats_.pages_copied++;
            stats_.bytes_copied += page_bytes(*copy);
            slot = std::move(copy);
        }
        return *slot;
    }

    std::vector<std::shared_ptr<Page>> pages_;
    std::size_t page_count_;
    mutable u64 version_ = 0;
    mutable CowStats stats_;
};

// ---------------------------------------------------------------------------
// RDB 编解码
//
// 文件布局（与真实 RDB 同构，但只支持 string）：
//   "REDIS0011"
//   AUX <redis-ver> <7.2.0>          (0xFA)
//   AUX <archdesign> <day17>
//   SELECTDB 0                        (0xFE)
//   RESIZEDB <keys> <expires>         (0xFB)
//   [EXPIRETIME_MS <ms>] TYPE_STRING KEY VALUE  (0xFC, 0x00)
//   ...
//   EOF                               (0xFF)
//   CRC64                             (8 bytes, little endian；关闭校验时写 0)
// ---------------------------------------------------------------------------

struct RdbOptions {
    bool checksum = true;
    std::string redis_version = "7.2.0";
};

struct RdbLoadOptions {
    bool verify_checksum = true;
    // 独立 RDB 启动时跳过已过期键；AOF 的 RDB preamble 不跳过，
    // 因为后面的 AOF tail 仍可能引用/删除这些键。
    bool skip_expired = true;
    int max_version = 11;
};

struct RdbDecodeResult {
    std::size_t consumed = 0;
    std::size_t loaded = 0;
    std::size_t skipped_expired = 0;
    u64 checksum = 0;
};

inline void rdb_write_len(std::string& out, u64 len) {
    if (len < 64) {
        out.push_back(static_cast<char>(len));
    } else if (len < 16384) {
        out.push_back(static_cast<char>(0x40 | ((len >> 8) & 0x3f)));
        out.push_back(static_cast<char>(len & 0xff));
    } else if (len <= 0xffffffffULL) {
        out.push_back(static_cast<char>(0x80));
        put_u32_be(out, static_cast<u32>(len));
    } else {
        out.push_back(static_cast<char>(0x81));
        put_u64_be(out, len);  // RDB 的 64 位长度是 big-endian
    }
}

inline void rdb_write_string(std::string& out, std::string_view value) {
    rdb_write_len(out, value.size());
    out.append(value.data(), value.size());
}

inline std::string rdb_encode(const Snapshot& snapshot, const RdbOptions& options = {}) {
    std::string out;
    out.append("REDIS0011", 9);

    // AUX 字段：真实 RDB 里还有 ctime/used-mem 等；这里保留最少的可观测元数据。
    out.push_back(static_cast<char>(0xFA));
    rdb_write_string(out, "redis-ver");
    rdb_write_string(out, options.redis_version);
    out.push_back(static_cast<char>(0xFA));
    rdb_write_string(out, "archdesign");
    rdb_write_string(out, "day17");

    out.push_back(static_cast<char>(0xFE));  // SELECTDB
    rdb_write_len(out, 0);

    out.push_back(static_cast<char>(0xFB));  // RESIZEDB
    rdb_write_len(out, snapshot.key_count());
    rdb_write_len(out, snapshot.expires_count());

    snapshot.for_each([&](const std::string& key, const Entry& entry) {
        if (entry.expire_at_ms != -1) {
            out.push_back(static_cast<char>(0xFC));  // EXPIRETIME_MS
            put_u64_le(out, static_cast<u64>(entry.expire_at_ms));
        }
        out.push_back(static_cast<char>(0x00));  // RDB_TYPE_STRING
        rdb_write_string(out, key);
        rdb_write_string(out, entry.value);
    });

    out.push_back(static_cast<char>(0xFF));  // EOF
    const u64 checksum = options.checksum ? crc64(out) : 0;
    put_u64_le(out, checksum);
    return out;
}

class RdbReader {
public:
    explicit RdbReader(std::string_view data) : data_(data) {}

    u8 byte() {
        if (pos_ >= data_.size()) fail("unexpected end of RDB");
        return static_cast<u8>(data_[pos_++]);
    }

    std::string_view bytes(std::size_t n) {
        if (n > data_.size() - pos_) fail("unexpected end of RDB");
        std::string_view out = data_.substr(pos_, n);
        pos_ += n;
        return out;
    }

    u64 len() {
        const u8 first = byte();
        const u8 type = static_cast<u8>(first >> 6);
        if (type == 0) return first & 0x3f;
        if (type == 1) {
            const u8 second = byte();
            return (static_cast<u64>(first & 0x3f) << 8) | second;
        }
        if (first == 0x80) {
            const auto raw = bytes(4);
            return read_u32_be(raw, 0);
        }
        if (first == 0x81) {
            const auto raw = bytes(8);
            return read_u64_be(raw, 0);
        }
        fail("unsupported RDB length encoding");
        return 0;
    }

    std::string str() {
        const u64 n = len();
        if (n > static_cast<u64>(std::numeric_limits<std::size_t>::max())) fail("RDB string too large");
        const auto raw = bytes(static_cast<std::size_t>(n));
        return std::string(raw);
    }

    std::size_t position() const { return pos_; }

private:
    [[noreturn]] void fail(const char* message) const {
        throw std::runtime_error(std::string(message) + " at offset " + std::to_string(pos_));
    }

    std::string_view data_;
    std::size_t pos_ = 0;
};

inline RdbDecodeResult rdb_decode(std::string_view data, PageStore& store, i64 now_ms,
                                  const RdbLoadOptions& options = {}) {
    RdbReader reader(data);
    const std::string_view magic = reader.bytes(9);
    if (magic.substr(0, 5) != "REDIS") throw std::runtime_error("RDB: bad magic, expected REDIS");
    const int version = static_cast<int>(parse_i64(magic.substr(5), "RDB version"));
    if (version < 1 || version > options.max_version) {
        throw std::runtime_error("RDB: unsupported format version " + std::to_string(version));
    }

    RdbDecodeResult result;
    i64 expire_at_ms = -1;
    bool saw_eof = false;

    while (!saw_eof) {
        const u8 opcode = reader.byte();
        switch (opcode) {
            case 0xFA: {  // AUX
                (void)reader.str();
                (void)reader.str();
                break;
            }
            case 0xFE: {  // SELECTDB
                const u64 dbid = reader.len();
                if (dbid != 0) throw std::runtime_error("RDB: only DB 0 is supported");
                break;
            }
            case 0xFB: {  // RESIZEDB
                (void)reader.len();
                (void)reader.len();
                break;
            }
            case 0xFC: {  // EXPIRETIME_MS
                expire_at_ms = static_cast<i64>(read_u64_le(reader.bytes(8), 0));
                break;
            }
            case 0xF8: {  // IDLE（真实 RDB 在 LRU 策略下会写；跳过）
                (void)reader.len();
                break;
            }
            case 0xF9: {  // FREQ（LFU 策略）
                (void)reader.byte();
                break;
            }
            case 0x00: {  // RDB_TYPE_STRING
                const std::string key = reader.str();
                std::string value = reader.str();
                if (options.skip_expired && expire_at_ms != -1 && expire_at_ms <= now_ms) {
                    ++result.skipped_expired;
                } else {
                    store.set(std::move(key), std::move(value), expire_at_ms);
                    ++result.loaded;
                }
                expire_at_ms = -1;
                break;
            }
            case 0xFF: {  // EOF
                saw_eof = true;
                break;
            }
            default:
                throw std::runtime_error("RDB: unsupported opcode " + std::to_string(opcode) +
                                         " at offset " + std::to_string(reader.position() - 1));
        }
    }

    const std::size_t eof_end = reader.position();
    const std::string_view checksum_bytes = reader.bytes(8);
    result.checksum = read_u64_le(checksum_bytes, 0);
    if (options.verify_checksum) {
        if (result.checksum == 0) {
            // 与 Redis 一致：0 表示「写文件时关闭了校验」，加载时跳过比较。
        } else {
            const u64 actual = crc64(data.substr(0, eof_end));
            if (actual != result.checksum) {
                throw std::runtime_error("RDB: CRC64 mismatch (stored " + std::to_string(result.checksum) +
                                         ", actual " + std::to_string(actual) + ")");
            }
        }
    }
    result.consumed = eof_end + 8;
    return result;
}

// ---------------------------------------------------------------------------
// MemFs：把「OS page cache / 磁盘」拆成两层
//
// data  = 当前文件内容（包括已 write、未 fsync 的尾部）
// durable = 已经 fsync 的前缀长度
// crash() 只保留 durable 前缀，模拟掉电时 page cache 里未落盘的数据。
// ---------------------------------------------------------------------------

struct MemFile {
    std::string data;
    std::size_t durable = 0;
};

class MemFs {
public:
    void append(const std::string& path, std::string_view bytes) {
        if (fail_writes_) throw std::runtime_error("MemFs: injected write failure");
        auto& file = files_[path];
        file.data.append(bytes.data(), bytes.size());
        ++writes_;
    }

    void write_file(const std::string& path, std::string_view bytes) {
        if (fail_writes_) throw std::runtime_error("MemFs: injected write failure");
        auto& file = files_[path];
        file.data.assign(bytes.data(), bytes.size());
        file.durable = 0;
        ++writes_;
    }

    void fsync(const std::string& path) {
        auto it = files_.find(path);
        if (it == files_.end()) throw std::runtime_error("MemFs: fsync of missing file " + path);
        it->second.durable = it->second.data.size();
        ++fsyncs_;
    }

    void rename(const std::string& from, const std::string& to) {
        auto it = files_.find(from);
        if (it == files_.end()) throw std::runtime_error("MemFs: rename of missing file " + from);
        MemFile moved = std::move(it->second);
        files_.erase(it);
        files_[to] = std::move(moved);
    }

    void remove(const std::string& path) { files_.erase(path); }

    void truncate(const std::string& path, std::size_t size) {
        auto it = files_.find(path);
        if (it == files_.end()) throw std::runtime_error("MemFs: truncate of missing file " + path);
        if (size > it->second.data.size()) throw std::runtime_error("MemFs: truncate beyond EOF");
        it->second.data.resize(size);
        it->second.durable = std::min(it->second.durable, size);
    }

    bool exists(const std::string& path) const { return files_.find(path) != files_.end(); }

    std::string read(const std::string& path) const {
        auto it = files_.find(path);
        if (it == files_.end()) throw std::runtime_error("MemFs: read of missing file " + path);
        return it->second.data;
    }

    std::size_t size(const std::string& path) const {
        auto it = files_.find(path);
        return it == files_.end() ? 0 : it->second.data.size();
    }

    // 模拟掉电：所有文件只剩 fsync 过的前缀。
    void crash() {
        for (auto& [path, file] : files_) {
            (void)path;
            if (file.data.size() > file.durable) file.data.resize(file.durable);
        }
        ++crashes_;
    }

    void set_fail_writes(bool value) { fail_writes_ = value; }
    u64 fsyncs() const { return fsyncs_; }
    u64 writes() const { return writes_; }
    u64 crashes() const { return crashes_; }

    std::size_t durable_size(const std::string& path) const {
        auto it = files_.find(path);
        return it == files_.end() ? 0 : it->second.durable;
    }

private:
    std::map<std::string, MemFile> files_;
    bool fail_writes_ = false;
    u64 fsyncs_ = 0;
    u64 writes_ = 0;
    u64 crashes_ = 0;
};

// ---------------------------------------------------------------------------
// RESP 编解码：AOF 用的就是 Redis 协议
// ---------------------------------------------------------------------------

struct Command {
    std::vector<std::string> args;

    Command() = default;
    Command(std::initializer_list<std::string> values) : args(values) {}
};

inline std::string resp_encode(const Command& command) {
    std::string out = "*" + std::to_string(command.args.size()) + "\r\n";
    for (const std::string& arg : command.args) {
        out += "$" + std::to_string(arg.size()) + "\r\n";
        out += arg;
        out += "\r\n";
    }
    return out;
}

struct RespParseResult {
    std::vector<Command> commands;
    std::size_t consumed = 0;
    bool truncated = false;
};

inline bool resp_read_line(std::string_view data, std::size_t& pos, std::string& line) {
    const std::size_t start = pos;
    while (pos + 1 < data.size()) {
        if (data[pos] == '\r' && data[pos + 1] == '\n') {
            line.assign(data.substr(start, pos - start));
            pos += 2;
            return true;
        }
        ++pos;
    }
    return false;
}

inline RespParseResult resp_parse(std::string_view data) {
    RespParseResult result;
    std::size_t pos = 0;
    std::size_t last_complete = 0;

    auto truncated = [&]() {
        result.truncated = true;
        result.consumed = last_complete;
        return result;
    };

    while (pos < data.size()) {
        // Redis 7 的 AOF 可以带 "#TS:<unix>\r\n" 时间戳注释。
        if (data[pos] == '#') {
            std::string ignored;
            if (!resp_read_line(data, pos, ignored)) return truncated();
            last_complete = pos;
            continue;
        }
        if (data[pos] != '*') {
            throw std::runtime_error("AOF: expected '*', got byte " +
                                     std::to_string(static_cast<unsigned>(static_cast<u8>(data[pos]))) +
                                     " at offset " + std::to_string(pos));
        }
        ++pos;

        std::string count_line;
        if (!resp_read_line(data, pos, count_line)) return truncated();
        const i64 count = parse_i64(count_line, "RESP array length");
        if (count < 0 || count > 1'000'000) throw std::runtime_error("AOF: invalid array length");

        Command command;
        command.args.reserve(static_cast<std::size_t>(count));
        for (i64 i = 0; i < count; ++i) {
            if (pos >= data.size()) return truncated();
            if (data[pos] != '$') {
                throw std::runtime_error("AOF: expected '$' at offset " + std::to_string(pos));
            }
            ++pos;
            std::string length_line;
            if (!resp_read_line(data, pos, length_line)) return truncated();
            const i64 length = parse_i64(length_line, "RESP bulk length");
            if (length < 0) throw std::runtime_error("AOF: negative bulk length");
            if (static_cast<u64>(length) > data.size() - pos) return truncated();
            command.args.emplace_back(data.substr(pos, static_cast<std::size_t>(length)));
            pos += static_cast<std::size_t>(length);
            if (pos + 1 >= data.size()) return truncated();
            if (data[pos] != '\r' || data[pos + 1] != '\n') {
                throw std::runtime_error("AOF: bulk string not terminated by CRLF");
            }
            pos += 2;
        }
        result.commands.push_back(std::move(command));
        last_complete = pos;
    }

    result.consumed = last_complete;
    return result;
}

// ---------------------------------------------------------------------------
// AOF 写入：append + appendfsync
// ---------------------------------------------------------------------------

enum class FsyncPolicy { Always, EverySec, No };

inline const char* fsync_policy_name(FsyncPolicy policy) {
    switch (policy) {
        case FsyncPolicy::Always: return "always";
        case FsyncPolicy::EverySec: return "everysec";
        case FsyncPolicy::No: return "no";
    }
    return "unknown";
}

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

inline void apply_command(PageStore& store, const Command& command, i64 now_ms);

class AofWriter {
public:
    AofWriter(MemFs& fs, std::string path, AofConfig config)
        : fs_(fs), path_(std::move(path)), config_(std::move(config)) {}

    void mark_start(i64 now_ms) { last_fsync_ms_ = now_ms; }

    void append(const Command& command) {
        buffer_ += resp_encode(command);
        ++appended_commands_;
    }

    // 对应 Redis 的 beforeSleep -> flushAppendOnlyFile：
    // 先 write(2)，再根据 appendfsync 决定是否 fsync。
    void flush(i64 now_ms, bool force = false) {
        if (buffer_.empty()) {
            maybe_fsync(now_ms, force);
            return;
        }
        fs_.append(path_, buffer_);
        buffer_.clear();
        ++flushes_;
        maybe_fsync(now_ms, force);
    }

    void tick(i64 now_ms) { flush(now_ms, false); }

    void fsync_now(i64 now_ms) {
        fs_.fsync(path_);
        ++fsyncs_;
        last_fsync_ms_ = now_ms;
    }

    AofLoadResult load(PageStore& store, i64 now_ms) {
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
            fs_.fsync(path_);             // 把截断结果落盘，避免下次启动重复报警
        }

        auto_size_ = fs_.size(path_);
        base_size_ = auto_size_;
        return result;
    }

    void reset_after_rewrite(i64 now_ms) {
        auto_size_ = fs_.size(path_);
        base_size_ = auto_size_;
        last_fsync_ms_ = now_ms;
        buffer_.clear();
    }

    std::size_t size() const { return fs_.size(path_); }
    std::size_t base_size() const { return base_size_; }
    std::size_t buffered_bytes() const { return buffer_.size(); }
    std::size_t appended_commands() const { return appended_commands_; }
    std::size_t flushes() const { return flushes_; }
    std::size_t fsyncs() const { return fsyncs_; }
    const std::string& path() const { return path_; }

private:
    void maybe_fsync(i64 now_ms, bool force) {
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

    MemFs& fs_;
    std::string path_;
    AofConfig config_;
    std::string buffer_;
    i64 last_fsync_ms_ = 0;
    std::size_t auto_size_ = 0;
    std::size_t base_size_ = 0;
    std::size_t appended_commands_ = 0;
    std::size_t flushes_ = 0;
    std::size_t fsyncs_ = 0;
};

inline void apply_command(PageStore& store, const Command& command, i64 now_ms) {
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

// ---------------------------------------------------------------------------
// 持久化引擎：把 PageStore、RDB、AOF、COW、rewrite 串起来
// ---------------------------------------------------------------------------

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
    Engine(MemFs& fs, EngineConfig config)
        : fs_(fs),
          config_(std::move(config)),
          store_(config_.page_count),
          aof_(fs_, config_.aof_path, config_.aof) {}

    void set(const std::string& key, const std::string& value, i64 now_ms, i64 ttl_ms = -1) {
        const i64 deadline = ttl_ms < 0 ? -1 : now_ms + ttl_ms;
        store_.set(key, value, deadline);
        Command command{"SET", key, value};
        if (deadline != -1) {
            command.args.push_back("PXAT");
            command.args.push_back(std::to_string(deadline));
        }
        append_to_aof(command, now_ms);
    }

    bool del(const std::string& key, i64 now_ms) {
        if (!store_.del(key)) return false;
        append_to_aof(Command{"DEL", key}, now_ms);
        return true;
    }

    bool expire(const std::string& key, i64 now_ms, i64 ttl_ms) {
        if (ttl_ms <= 0) {
            if (!store_.del(key)) return false;
            append_to_aof(Command{"DEL", key}, now_ms);
            return true;
        }
        if (!store_.expire_at(key, now_ms + ttl_ms)) return false;
        append_to_aof(Command{"PEXPIREAT", key, std::to_string(now_ms + ttl_ms)}, now_ms);
        return true;
    }

    bool persist(const std::string& key, i64 now_ms) {
        if (!store_.persist(key)) return false;
        append_to_aof(Command{"PERSIST", key}, now_ms);
        return true;
    }

    std::optional<Entry> get(const std::string& key, i64 now_ms) const {
        return store_.get_live(key, now_ms);
    }

    std::size_t size(i64 now_ms) const { return store_.live_key_count(now_ms); }

    // ---- RDB ----
    void save_rdb(i64 /*now_ms*/) {
        const Snapshot snapshot = store_.fork();
        write_rdb_snapshot(snapshot);
    }

    void begin_bgsave(i64 /*now_ms*/) {
        if (bgsave_active_) throw std::runtime_error("BGSAVE already in progress");
        bgsave_snapshot_ = store_.fork();
        bgsave_active_ = true;
    }

    void finish_bgsave(i64 /*now_ms*/) {
        if (!bgsave_active_) throw std::runtime_error("no BGSAVE in progress");
        write_rdb_snapshot(*bgsave_snapshot_);
        bgsave_active_ = false;
        bgsave_snapshot_.reset();
        ++stats_.bgsaves;
    }

    bool bgsave_active() const { return bgsave_active_; }

    // ---- AOF rewrite ----
    void begin_rewrite(i64 /*now_ms*/) {
        if (!config_.aof_enabled) throw std::runtime_error("AOF is disabled");
        if (rewrite_active_) throw std::runtime_error("AOF rewrite already in progress");
        rewrite_snapshot_ = store_.fork();
        rewrite_base_ = config_.aof.use_rdb_preamble
                            ? rdb_encode(*rewrite_snapshot_, config_.rdb)
                            : encode_snapshot_as_aof(*rewrite_snapshot_);
        rewrite_tail_.clear();
        rewrite_active_ = true;
    }

    void finish_rewrite(i64 now_ms) {
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

    void abort_rewrite() {
        rewrite_active_ = false;
        rewrite_base_.clear();
        rewrite_tail_.clear();
        rewrite_snapshot_.reset();
    }

    bool rewrite_active() const { return rewrite_active_; }
    std::size_t rewrite_tail_bytes() const { return rewrite_tail_.size(); }
    std::size_t rewrite_tail_commands() const { return stats_.rewrite_tail_commands; }

    bool maybe_auto_rewrite(i64 now_ms) {
        if (!config_.aof_enabled || rewrite_active_) return false;
        const std::size_t current = aof_.size();
        if (current < config_.auto_rewrite_min_size) return false;
        const std::size_t base = std::max<std::size_t>(aof_.base_size(), 1);
        const u64 growth = current > base ? static_cast<u64>((current - base) * 100 / base) : 0;
        if (growth < static_cast<u64>(config_.auto_rewrite_percentage)) return false;
        begin_rewrite(now_ms);
        return true;
    }

    // ---- 启动恢复 ----
    void load(i64 now_ms) {
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

    const PageStore& store() const { return store_; }
    const EngineStats& stats() const { return stats_; }
    const EngineConfig& config() const { return config_; }
    MemFs& fs() { return fs_; }
    AofWriter& aof() { return aof_; }

private:
    void append_to_aof(const Command& command, i64 now_ms) {
        if (!config_.aof_enabled) return;
        aof_.append(command);
        aof_.flush(now_ms);
        if (rewrite_active_) {
            rewrite_tail_ += resp_encode(command);
            ++stats_.rewrite_tail_commands;
            stats_.rewrite_tail_bytes = rewrite_tail_.size();
        }
    }

    std::string encode_snapshot_as_aof(const Snapshot& snapshot) const {
        std::string out;
        snapshot.for_each([&](const std::string& key, const Entry& entry) {
            out += resp_encode(Command{"SET", key, entry.value});
            if (entry.expire_at_ms != -1) {
                out += resp_encode(Command{"PEXPIREAT", key, std::to_string(entry.expire_at_ms)});
            }
        });
        return out;
    }

    void write_rdb_snapshot(const Snapshot& snapshot) {
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
