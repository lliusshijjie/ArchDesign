#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using Lsn = std::uint64_t;

struct Put {
    std::uint32_t key;
    std::uint32_t value;
};

// Little-endian frame: LSN (8), key (4), value (4), CRC32 (4).
constexpr std::size_t payload_size = 16;
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

std::uint32_t crc32(std::span<const unsigned char> bytes) {
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

Frame encode(Lsn lsn, Put put) {
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

File open_file(const fs::path& path, bool append) {
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

void sync_file(std::FILE* file) {
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

struct Recovery {
    std::map<std::uint32_t, std::uint32_t> values;
    Lsn last_lsn = 0;
    std::uintmax_t valid_bytes = 0;
    bool short_tail = false;
};

Recovery recover(const fs::path& path) {
    auto file = open_file(path, false);
    Recovery state;
    Frame frame{};
    for (;;) {
        const auto bytes = std::fread(frame.data(), 1, frame.size(), file.get());
        if (bytes != frame.size()) {
            if (std::ferror(file.get()) != 0) {
                throw std::system_error(errno, std::generic_category(), "read WAL");
            }
            state.short_tail = bytes != 0;
            return state;
        }

        const auto expected = read_le<std::uint32_t>(frame.data() + payload_size);
        if (crc32(std::span{frame}.first(payload_size)) != expected) {
            throw std::runtime_error("WAL checksum mismatch at byte " +
                                     std::to_string(state.valid_bytes));
        }
        const auto lsn = read_le<Lsn>(frame.data());
        if (lsn != state.last_lsn + 1) {
            throw std::runtime_error("non-consecutive WAL LSN");
        }

        const auto key = read_le<std::uint32_t>(frame.data() + 8);
        const auto value = read_le<std::uint32_t>(frame.data() + 12);
        state.values[key] = value;
        state.last_lsn = lsn;
        state.valid_bytes += frame.size();
    }
}

class GroupCommit {
public:
    using Sync = std::function<void(std::FILE*)>;

    explicit GroupCommit(const fs::path& path, Sync sync = sync_file)
        : sync_(std::move(sync)) {
        const auto state = recover(path);
        if (state.short_tail) {
            throw std::runtime_error("truncate short WAL tail before appending");
        }
        // Also cover any complete frames left behind by an earlier writer error.
        file_ = open_file(path, true);
        if (state.last_lsn != 0) {
            sync_file(file_.get());
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
            const auto frame = encode(next++, request.put);
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
    File file_;
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

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <class Error, class Action>
void require_error(Action action, std::string_view message) {
    try {
        action();
    } catch (const Error& error) {
        require(std::string_view{error.what()}.find(message) != std::string_view::npos,
                error.what());
        return;
    }
    throw std::runtime_error("expected error: " + std::string(message));
}

class TempWal {
public:
    TempWal() {
        const auto base = fs::temp_directory_path();
        std::random_device random;
        for (int attempt = 0; attempt < 16; ++attempt) {
            auto directory = base / ("archdesign-day009-" + std::to_string(random()));
            auto path = directory / "demo.wal";
            if (fs::create_directory(directory)) {
                directory_ = std::move(directory);
                path_ = std::move(path);
                std::ofstream file(path_, std::ios::binary);
                file.close();
                if (!file) {
                    cleanup();
                    throw std::runtime_error("could not create WAL fixture");
                }
                return;
            }
        }
        throw std::runtime_error("could not reserve a temporary WAL directory");
    }

    TempWal(const TempWal&) = delete;
    TempWal& operator=(const TempWal&) = delete;
    ~TempWal() { cleanup(); }

    const fs::path& path() const { return path_; }

private:
    void cleanup() const {
        std::error_code ignored;
        fs::remove(path_, ignored);
        fs::remove(directory_, ignored);
    }

    fs::path directory_;
    fs::path path_;
};

// Hold the writer between fflush and file sync while the test queues more work.
struct SyncGate {
    std::promise<void> entered;
    std::promise<void> release;
    std::future<void> released = release.get_future();

    void pause() {
        entered.set_value();
        require(released.wait_for(5s) == std::future_status::ready, "sync gate timed out");
    }

    void wait_until_entered() {
        require(entered.get_future().wait_for(5s) == std::future_status::ready,
                "writer did not reach sync");
    }
};

void batching() {
    const TempWal serial;
    {
        GroupCommit wal(serial.path());
        for (std::uint32_t value = 1; value <= 5; ++value) {
            require(wal.submit({1, value}).get() == value, "unexpected serial LSN");
        }
        require(wal.sync_count() == 5, "serial commits must use five syncs");
    }

    const TempWal grouped;
    SyncGate gate;
    bool first_sync = true;
    {
        GroupCommit wal(grouped.path(), [&](std::FILE* file) {
            if (std::exchange(first_sync, false)) {
                gate.pause();
            }
            sync_file(file);
        });
        auto first = wal.submit({1, 1});
        gate.wait_until_entered();
        std::array<std::future<Lsn>, 4> next;
        for (std::size_t i = 0; i < next.size(); ++i) {
            next[i] = wal.submit({1, static_cast<std::uint32_t>(i + 2)});
        }
        const bool waiting_for_sync = first.wait_for(0s) == std::future_status::timeout;
        gate.release.set_value();

        require(waiting_for_sync, "request acknowledged before file sync");
        require(first.get() == 1, "unexpected first LSN");
        for (std::size_t i = 0; i < next.size(); ++i) {
            require(next[i].get() == i + 2, "batch acknowledgements must follow LSN order");
        }
        require(wal.sync_count() == 2, "expected a batch of one then a batch of four");
    }

    const auto restored = recover(grouped.path());
    require(!restored.short_tail && restored.last_lsn == 5 && restored.values.at(1) == 5 &&
                restored.values == recover(serial.path()).values,
            "grouped and serial replay must agree");
    std::cout << "[  OK  ] batching (5 serial syncs, 2 grouped syncs)\n";
}

void short_tail() {
    const TempWal log;
    {
        GroupCommit wal(log.path());
        wal.submit({1, 10}).get();
        wal.submit({1, 20}).get();
        wal.submit({1, 30}).get();
    }

    fs::resize_file(log.path(), fs::file_size(log.path()) - 3);
    const auto state = recover(log.path());
    require(state.short_tail && state.last_lsn == 2 && state.valid_bytes == 2 * Frame{}.size() &&
                state.values.at(1) == 20,
            "recovery must stop at the last full frame");
    require_error<std::runtime_error>([&] { GroupCommit wal(log.path()); }, "truncate short");

    fs::resize_file(log.path(), state.valid_bytes);
    {
        GroupCommit wal(log.path());
        require(wal.submit({1, 31}).get() == 3, "reopen must resume at LSN 3");
    }
    const auto repaired = recover(log.path());
    require(!repaired.short_tail && repaired.last_lsn == 3 && repaired.values.at(1) == 31,
            "replay after repair must include the new record");
    std::cout << "[  OK  ] short-tail (kept LSN 1-2, resumed at 3)\n";
}

void corrupt_record() {
    const TempWal log;
    {
        GroupCommit wal(log.path());
        wal.submit({1, 10}).get();
        wal.submit({1, 20}).get();
        wal.submit({1, 30}).get();
    }
    {
        std::fstream file(log.path(), std::ios::binary | std::ios::in | std::ios::out);
        const auto offset = static_cast<std::streamoff>(Frame{}.size() + 12);
        file.seekg(offset);
        const int byte = file.get();
        require(byte != std::char_traits<char>::eof(), "could not read the frame to corrupt");
        file.seekp(offset);
        file.put(static_cast<char>(byte ^ 1));
        file.close();
        require(static_cast<bool>(file), "could not inject a bit flip");
    }
    require_error<std::runtime_error>([&] { recover(log.path()); }, "checksum mismatch at byte 20");
    require_error<std::runtime_error>([&] { GroupCommit wal(log.path()); }, "checksum mismatch");
    std::cout << "[  OK  ] corrupt-record (CRC rejected the second frame)\n";
}

void sync_failure() {
    const TempWal log;
    SyncGate gate;
    {
        GroupCommit wal(log.path(), [&](std::FILE*) {
            gate.pause();
            throw std::runtime_error("injected sync failure");
        });
        auto active = wal.submit({1, 10});
        gate.wait_until_entered();
        auto queued = wal.submit({2, 20});
        gate.release.set_value();

        require_error<std::runtime_error>([&] { active.get(); }, "injected sync failure");
        require_error<std::runtime_error>([&] { queued.get(); }, "injected sync failure");
        require(wal.sync_count() == 0, "failed sync counted as success");
        require_error<std::logic_error>([&] { wal.submit({3, 30}); }, "writer stopped");
    }
    const auto visible = recover(log.path());
    require(visible.last_lsn == 1 && visible.values.at(1) == 10 && !visible.values.contains(2),
            "the active frame reached the OS but the queued frame was never written");
    std::cout << "[  OK  ] sync-failure (active and queued requests both received errors)\n";
}

struct Scenario {
    std::string_view name;
    void (*run)();
};

constexpr std::array scenarios{
    Scenario{"batching", batching},
    Scenario{"short-tail", short_tail},
    Scenario{"corrupt-record", corrupt_record},
    Scenario{"sync-failure", sync_failure},
};

}  // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc == 1) {
            for (const auto& scenario : scenarios) {
                scenario.run();
            }
            std::cout << "\nAll " << scenarios.size() << " tests passed.\n";
            return 0;
        }
        if (argc == 2) {
            for (const auto& scenario : scenarios) {
                if (scenario.name == argv[1]) {
                    scenario.run();
                    return 0;
                }
            }
        }
        std::cerr << "usage: mini_wal_demo [batching|short-tail|corrupt-record|sync-failure]\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
