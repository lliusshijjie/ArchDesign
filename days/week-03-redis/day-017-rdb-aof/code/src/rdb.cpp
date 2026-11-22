#include "rdb.hpp"

#include <limits>
#include <stdexcept>

#include "bytes.hpp"

namespace archdesign {

namespace {

void write_len(std::string& out, u64 len) {
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

void write_string(std::string& out, std::string_view value) {
    write_len(out, value.size());
    out.append(value.data(), value.size());
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
        if (first == 0x80) return read_u32_be(bytes(4), 0);
        if (first == 0x81) return read_u64_be(bytes(8), 0);
        fail("unsupported RDB length encoding");
        return 0;
    }

    std::string str() {
        const u64 n = len();
        if (n > static_cast<u64>(std::numeric_limits<std::size_t>::max())) {
            fail("RDB string too large");
        }
        return std::string(bytes(static_cast<std::size_t>(n)));
    }

    std::size_t position() const { return pos_; }

private:
    [[noreturn]] void fail(const char* message) const {
        throw std::runtime_error(std::string(message) + " at offset " + std::to_string(pos_));
    }

    std::string_view data_;
    std::size_t pos_ = 0;
};

}  // namespace

std::string rdb_encode(const Snapshot& snapshot, const RdbOptions& options) {
    std::string out;
    out.append("REDIS0011", 9);

    // AUX 字段：真实 RDB 里还有 ctime/used-mem 等；这里保留最少的可观测元数据。
    out.push_back(static_cast<char>(0xFA));
    write_string(out, "redis-ver");
    write_string(out, options.redis_version);
    out.push_back(static_cast<char>(0xFA));
    write_string(out, "archdesign");
    write_string(out, "day17");

    out.push_back(static_cast<char>(0xFE));  // SELECTDB
    write_len(out, 0);

    out.push_back(static_cast<char>(0xFB));  // RESIZEDB
    write_len(out, snapshot.key_count());
    write_len(out, snapshot.expires_count());

    snapshot.for_each([&](const std::string& key, const Entry& entry) {
        if (entry.expire_at_ms != -1) {
            out.push_back(static_cast<char>(0xFC));  // EXPIRETIME_MS
            put_u64_le(out, static_cast<u64>(entry.expire_at_ms));
        }
        out.push_back(static_cast<char>(0x00));  // RDB_TYPE_STRING
        write_string(out, key);
        write_string(out, entry.value);
    });

    out.push_back(static_cast<char>(0xFF));  // EOF
    put_u64_le(out, options.checksum ? crc64(out) : 0);
    return out;
}

RdbDecodeResult rdb_decode(std::string_view data, PageStore& store, i64 now_ms,
                           const RdbLoadOptions& options) {
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
                std::string key = reader.str();
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
    result.checksum = read_u64_le(reader.bytes(8), 0);
    if (options.verify_checksum && result.checksum != 0) {
        const u64 actual = crc64(data.substr(0, eof_end));
        if (actual != result.checksum) {
            throw std::runtime_error("RDB: CRC64 mismatch (stored " +
                                     std::to_string(result.checksum) + ", actual " +
                                     std::to_string(actual) + ")");
        }
    }
    result.consumed = eof_end + 8;
    return result;
}

}  // namespace archdesign
