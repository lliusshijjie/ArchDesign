#pragma once

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace mini_wal {

using Lsn = std::uint64_t;

struct Put {
    std::string key;
    std::string value;
};

struct Recovery {
    std::map<std::string, std::string> values;
    Lsn last_lsn = 0;
    std::uintmax_t valid_bytes = 0;
    bool incomplete_tail = false;
};

namespace detail {

using Bytes = std::vector<unsigned char>;
constexpr std::size_t header_fields_size = sizeof(std::uint32_t) + sizeof(Lsn);
constexpr std::size_t header_size = header_fields_size + sizeof(std::uint32_t);
constexpr std::size_t max_payload_size = 64 * 1024;
constexpr std::size_t max_batch_records = 64;

template <class UInt>
void append_le(Bytes& output, UInt value) {
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        output.push_back(static_cast<unsigned char>(value >> (8 * i)));
    }
}

template <class UInt>
UInt read_le(std::span<const unsigned char> bytes) {
    UInt value = 0;
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        value |= static_cast<UInt>(bytes[i]) << (8 * i);
    }
    return value;
}

inline std::uint32_t crc32(std::span<const unsigned char> bytes) {
    std::uint32_t crc = 0xffffffffu;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const auto polynomial = (crc & 1u) != 0 ? 0xedb88320u : 0u;
            crc = (crc >> 1) ^ polynomial;
        }
    }
    return ~crc;
}

inline void encode(Bytes& output, Lsn lsn, const Put& put) {
    constexpr auto length_size = sizeof(std::uint32_t);
    if (put.key.empty() || put.key.size() > max_payload_size - length_size ||
        put.value.size() > max_payload_size - length_size - put.key.size()) {
        throw std::invalid_argument("invalid WAL key or payload size");
    }

    const auto start = output.size();
    const auto payload_size = length_size + put.key.size() + put.value.size();
    append_le(output, static_cast<std::uint32_t>(payload_size));
    append_le(output, lsn);
    append_le(output, crc32(std::span{output}.subspan(start, header_fields_size)));

    append_le(output, static_cast<std::uint32_t>(put.key.size()));
    output.insert(output.end(), put.key.begin(), put.key.end());
    output.insert(output.end(), put.value.begin(), put.value.end());
    append_le(output, crc32(std::span{output}.subspan(start + header_size, payload_size)));
}

inline std::size_t read_bytes(std::ifstream& file, std::span<unsigned char> bytes) {
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (file.bad() || (file.fail() && !file.eof())) {
        throw std::runtime_error("WAL read failed");
    }
    return static_cast<std::size_t>(file.gcount());
}

[[noreturn]] inline void corrupt(std::uintmax_t offset, const char* reason) {
    throw std::runtime_error("corrupt WAL at byte " + std::to_string(offset) + ": " + reason);
}

inline void sync_file(std::FILE* file) {
#ifdef _WIN32
    const int result = ::_commit(::_fileno(file));
#else
    int result;
    do {
        result = ::fsync(::fileno(file));
    } while (result != 0 && errno == EINTR);
#endif
    if (result != 0) {
        throw std::system_error(errno, std::generic_category(), "WAL sync failed");
    }
}

}  // namespace detail

inline Recovery recover(const std::filesystem::path& path) {
    using namespace detail;
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("could not open WAL for recovery");
    }

    Recovery result;
    std::array<unsigned char, header_size> header{};
    Bytes body;
    while (true) {
        const auto bytes_read = read_bytes(file, header);
        if (bytes_read != header.size()) {
            result.incomplete_tail = bytes_read != 0;
            return result;
        }

        const auto fields = std::span{header}.first(header_fields_size);
        if (crc32(fields) != read_le<std::uint32_t>(std::span{header}.last(sizeof(std::uint32_t)))) {
            corrupt(result.valid_bytes, "header checksum mismatch");
        }
        const auto payload_size = read_le<std::uint32_t>(fields);
        const auto lsn = read_le<Lsn>(fields.subspan(sizeof(std::uint32_t)));
        if (payload_size <= sizeof(std::uint32_t) || payload_size > max_payload_size) {
            corrupt(result.valid_bytes, "invalid payload length");
        }
        if (result.last_lsn == std::numeric_limits<Lsn>::max() || lsn != result.last_lsn + 1) {
            corrupt(result.valid_bytes, "non-contiguous LSN");
        }

        body.resize(payload_size + sizeof(std::uint32_t));
        if (read_bytes(file, body) != body.size()) {
            result.incomplete_tail = true;
            return result;
        }
        const auto payload = std::span{body}.first(payload_size);
        if (crc32(payload) != read_le<std::uint32_t>(std::span{body}.subspan(payload_size))) {
            corrupt(result.valid_bytes, "payload checksum mismatch");
        }
        const auto key_size = read_le<std::uint32_t>(payload);
        const auto data = payload.subspan(sizeof(std::uint32_t));
        if (key_size == 0 || key_size > data.size()) {
            corrupt(result.valid_bytes, "invalid key length");
        }
        const auto* chars = reinterpret_cast<const char*>(data.data());
        result.values.insert_or_assign(std::string(chars, key_size),
                                       std::string(chars + key_size, data.size() - key_size));
        result.last_lsn = lsn;
        result.valid_bytes += header.size() + body.size();
    }
}

// The caller owns exclusive access and supplies an existing WAL file.
class Writer {
public:
    using SyncFile = void(std::FILE*);

    explicit Writer(const std::filesystem::path& path, SyncFile& sync = detail::sync_file)
        : sync_(sync) {
        const auto state = recover(path);
        if (state.incomplete_tail) {
            throw std::runtime_error("repair incomplete WAL tail before appending");
        }
        last_lsn_ = state.last_lsn;
#ifdef _WIN32
        file_.reset(::_wfopen(path.c_str(), L"ab"));
#else
        file_.reset(std::fopen(path.c_str(), "ab"));
#endif
        if (!file_) {
            throw std::system_error(errno, std::generic_category(), "could not open WAL for append");
        }
    }

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    // Return the last LSN covered by this sync. Each record is an independent put.
    Lsn commit(std::span<const Put> batch) {
        if (failed_) {
            throw std::logic_error("WAL writer failed; recover before retrying");
        }
        if (batch.empty() || batch.size() > detail::max_batch_records) {
            throw std::invalid_argument("WAL batch must contain 1 to 64 records");
        }
        if (batch.size() > std::numeric_limits<Lsn>::max() - last_lsn_) {
            throw std::overflow_error("WAL LSN exhausted");
        }

        detail::Bytes bytes;
        auto batch_end = last_lsn_;
        for (const auto& put : batch) {
            detail::encode(bytes, ++batch_end, put);
        }

        // A write/sync error leaves an unknown on-disk outcome. Fence this writer.
        failed_ = true;
        if (std::fwrite(bytes.data(), 1, bytes.size(), file_.get()) != bytes.size()) {
            throw std::runtime_error("WAL append failed");
        }
        if (std::fflush(file_.get()) != 0) {
            throw std::system_error(errno, std::generic_category(), "WAL flush failed");
        }
        sync_(file_.get());
        last_lsn_ = batch_end;
        ++sync_count_;
        failed_ = false;
        return batch_end;
    }

    [[nodiscard]] std::size_t sync_count() const { return sync_count_; }

private:
    std::unique_ptr<std::FILE, decltype(&std::fclose)> file_{nullptr, std::fclose};
    SyncFile& sync_;
    Lsn last_lsn_ = 0;
    std::size_t sync_count_ = 0;
    bool failed_ = false;
};

}  // namespace mini_wal
