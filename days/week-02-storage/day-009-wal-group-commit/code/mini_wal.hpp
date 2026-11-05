// Day 09: single-writer WAL with group commit and prefix recovery.

#pragma once

#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace mini_wal {

using Lsn = std::uint64_t;

struct Put {
    std::uint32_t key;
    std::uint32_t value;
};

namespace detail {

// Little-endian frame: LSN (8), key (4), value (4), CRC32 (4).
inline constexpr std::size_t payload_size = 16;
using Frame = std::array<unsigned char, 20>;

template <class UInt>
void write_le(unsigned char* output, UInt value) {
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        output[i] = static_cast<unsigned char>(value & 0xffU);
        value >>= 8;
    }
}

template <class UInt>
UInt read_le(const unsigned char* input) {
    UInt value = 0;
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        value |= static_cast<UInt>(input[i]) << (8 * i);
    }
    return value;
}

inline std::uint32_t crc32(std::span<const unsigned char> bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const auto polynomial = (crc & 1U) != 0 ? 0xedb88320U : 0U;
            crc = (crc >> 1) ^ polynomial;
        }
    }
    return ~crc;
}

inline Frame encode(Lsn lsn, Put put) {
    Frame frame{};
    write_le(frame.data(), lsn);
    write_le(frame.data() + 8, put.key);
    write_le(frame.data() + 12, put.value);
    write_le(frame.data() + payload_size, crc32(std::span{frame}.first(payload_size)));
    return frame;
}

struct CloseFile {
    void operator()(std::FILE* file) const noexcept { std::fclose(file); }
};

using File = std::unique_ptr<std::FILE, CloseFile>;

inline File open_file(const std::filesystem::path& path, bool append) {
#ifdef _WIN32
    auto* file = ::_wfopen(path.c_str(), append ? L"ab" : L"rb");
#else
    auto* file = std::fopen(path.c_str(), append ? "ab" : "rb");
#endif
    if (file == nullptr) {
        throw std::system_error(errno, std::generic_category(), "open WAL");
    }
    return File{file};
}

inline void sync_file(std::FILE* file) {
#ifdef _WIN32
    const int result = ::_commit(::_fileno(file));
#else
    int result;
    do {
        result = ::fsync(::fileno(file));
    } while (result == -1 && errno == EINTR);
#endif
    if (result != 0) {
        throw std::system_error(errno, std::generic_category(), "sync WAL");
    }
}

}  // namespace detail

struct Recovery {
    std::map<std::uint32_t, std::uint32_t> values;
    Lsn last_lsn = 0;
    std::uintmax_t valid_bytes = 0;
    bool short_tail = false;
};

inline Recovery recover(const std::filesystem::path& path) {
    auto file = detail::open_file(path, false);
    Recovery state;
    detail::Frame frame{};
    for (;;) {
        const auto bytes = std::fread(frame.data(), 1, frame.size(), file.get());
        if (bytes != frame.size()) {
            if (std::ferror(file.get()) != 0) {
                throw std::system_error(errno, std::generic_category(), "read WAL");
            }
            state.short_tail = bytes != 0;
            return state;
        }

        const auto expected = detail::read_le<std::uint32_t>(frame.data() + detail::payload_size);
        if (detail::crc32(std::span{frame}.first(detail::payload_size)) != expected) {
            throw std::runtime_error("WAL checksum mismatch at byte " +
                                     std::to_string(state.valid_bytes));
        }
        const auto lsn = detail::read_le<Lsn>(frame.data());
        if (lsn != state.last_lsn + 1) {
            throw std::runtime_error("non-consecutive WAL LSN");
        }

        const auto key = detail::read_le<std::uint32_t>(frame.data() + 8);
        const auto value = detail::read_le<std::uint32_t>(frame.data() + 12);
        state.values[key] = value;
        state.last_lsn = lsn;
        state.valid_bytes += frame.size();
    }
}

class GroupCommit {
public:
    using Sync = std::function<void(std::FILE*)>;

    explicit GroupCommit(const std::filesystem::path& path, Sync sync = detail::sync_file)
        : sync_(std::move(sync)) {
        const auto state = recover(path);
        if (state.short_tail) {
            throw std::runtime_error("truncate short WAL tail before appending");
        }
        // Also cover any complete frames left behind by an earlier writer error.
        file_ = detail::open_file(path, true);
        if (state.last_lsn != 0) {
            detail::sync_file(file_.get());
        }
        flush_lsn_ = state.last_lsn;
        worker_ = std::thread([this] { run(); });
    }

    ~GroupCommit() {
        {
            const std::lock_guard lock(mutex_);
            closing_ = true;
        }
        ready_.notify_one();
        worker_.join();
    }

    std::future<Lsn> submit(Put put) {
        Request request{put, {}};
        auto result = request.completion.get_future();
        {
            const std::lock_guard lock(mutex_);
            if (failed_) {
                throw std::logic_error("WAL writer stopped after I/O error");
            }
            if (pending_.size() == max_pending) {
                throw std::runtime_error("WAL queue full");
            }
            pending_.push_back(std::move(request));
        }
        ready_.notify_one();
        return result;
    }

    std::size_t sync_count() const { return sync_count_.load(); }

private:
    struct Request {
        Put put;
        std::promise<Lsn> completion;
    };

    void run() {
        std::vector<Request> batch;
        for (;;) {
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [&] { return closing_ || !pending_.empty(); });
                if (pending_.empty()) {
                    return;
                }
                batch.swap(pending_);
            }
            try {
                flush_batch(batch);
            } catch (...) {
                fail_batch(batch, std::current_exception());
                return;
            }
            batch.clear();
        }
    }

    void flush_batch(std::vector<Request>& batch) {
        const Lsn first = flush_lsn_ + 1;
        Lsn next = first;
        for (const auto& request : batch) {
            const auto frame = detail::encode(next++, request.put);
            if (std::fwrite(frame.data(), 1, frame.size(), file_.get()) != frame.size()) {
                throw std::system_error(errno, std::generic_category(), "append WAL");
            }
        }
        if (std::fflush(file_.get()) != 0) {
            throw std::system_error(errno, std::generic_category(), "flush WAL buffer");
        }
        sync_(file_.get());

        flush_lsn_ = next - 1;
        ++sync_count_;
        Lsn committed = first;
        for (auto& request : batch) {
            request.completion.set_value(committed++);
        }
    }

    void fail_batch(std::vector<Request>& batch, std::exception_ptr error) {
        {
            const std::lock_guard lock(mutex_);
            failed_ = true;
            for (auto& request : pending_) {
                request.completion.set_exception(error);
            }
            pending_.clear();
        }
        for (auto& request : batch) {
            request.completion.set_exception(error);
        }
    }

    static constexpr std::size_t max_pending = 64;
    detail::File file_;
    Sync sync_;
    Lsn flush_lsn_ = 0;  // Only the writer thread changes it after startup.
    std::atomic<std::size_t> sync_count_{0};
    std::mutex mutex_;
    std::condition_variable ready_;
    std::vector<Request> pending_;
    bool closing_ = false;
    bool failed_ = false;
    std::thread worker_;
};

}  // namespace mini_wal
