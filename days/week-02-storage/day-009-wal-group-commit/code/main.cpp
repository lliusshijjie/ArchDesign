// Day 09: batching, short-tail recovery, corruption and sync failure experiments.

#include "mini_wal.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using mini_wal::GroupCommit;
using mini_wal::Lsn;
using mini_wal::recover;
using mini_wal::detail::Frame;

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
            mini_wal::detail::sync_file(file);
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
