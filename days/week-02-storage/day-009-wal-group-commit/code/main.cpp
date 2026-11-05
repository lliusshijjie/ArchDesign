#include "mini_wal.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

namespace fs = std::filesystem;
using mini_wal::Put;
using mini_wal::Writer;
using mini_wal::recover;

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
            auto dir = base / ("archdesign-day009-" + std::to_string(random()));
            if (fs::create_directory(dir)) {
                directory_ = std::move(dir);
                return;
            }
        }
        throw std::runtime_error("could not reserve a temporary WAL directory");
    }

    TempWal(const TempWal&) = delete;
    TempWal& operator=(const TempWal&) = delete;

    ~TempWal() {
        std::error_code ignored;
        fs::remove(path(), ignored);
        fs::remove(directory_, ignored);
    }

    [[nodiscard]] fs::path path() const { return directory_ / "demo.wal"; }

    void create_empty() const {
        std::ofstream file(path(), std::ios::binary | std::ios::trunc);
        file.close();
        require(static_cast<bool>(file), "could not create an empty WAL fixture");
    }

private:
    fs::path directory_;
};

void batching() {
    const std::array requests{
        Put{"a", "one"}, Put{"b", "two"}, Put{"a", "three"}, Put{"c", "four"},
        Put{"d", "five"}, Put{"c", "six"}, Put{"e", "seven"}, Put{"f", "eight"},
        Put{"a", "nine"}, Put{"g", "ten"}, Put{"b", ""}, Put{"h", std::string("x\0y", 3)},
    };

    TempWal single;
    TempWal grouped;
    single.create_empty();
    grouped.create_empty();
    const auto run = [&](const fs::path& path, std::size_t group_size) {
        Writer writer(path);
        for (std::size_t begin = 0; begin < requests.size(); begin += group_size) {
            const auto size = std::min(group_size, requests.size() - begin);
            const auto lsn = writer.commit(std::span{requests}.subspan(begin, size));
            require(lsn == begin + size, "unexpected acknowledged LSN");
        }
        return writer.sync_count();
    };

    constexpr std::size_t group_size = 4;
    const auto single_syncs = run(single.path(), 1);
    const auto group_syncs = run(grouped.path(), group_size);
    const auto one = recover(single.path());
    const auto four = recover(grouped.path());
    require(single_syncs == 12 && group_syncs == 3, "unexpected sync counts");
    require(!one.incomplete_tail && !four.incomplete_tail && one.last_lsn == 12 &&
                four.last_lsn == 12 && one.values == four.values && four.values.size() == 8 &&
                four.values.at("a") == "nine" && four.values.at("b").empty() &&
                four.values.at("c") == "six" && four.values.at("h") == requests.back().value,
            "replay produced unexpected state");
    std::cout << "12 puts: 1 per batch -> " << single_syncs << " syncs; " << group_size
              << " per batch -> " << group_syncs << " syncs; same recovered state\n";
}

void short_tail() {
    TempWal wal;
    wal.create_empty();
    const auto path = wal.path();
    const std::array prefix{Put{"a", "old"}, Put{"b", "kept"}};
    {
        Writer writer(path);
        require(writer.commit(prefix) == 2, "expected LSN 2");
    }
    const auto good_end = fs::file_size(path);
    {
        Writer writer(path);
        const std::array suffix{Put{"a", "lost-tail"}};
        require(writer.commit(suffix) == 3, "expected LSN 3 before fault injection");
    }

    // First cut inside the body, then cut inside the header of the same record.
    const std::array cuts{fs::file_size(path) - 6, good_end + 3};
    for (const auto size : cuts) {
        fs::resize_file(path, size);
        const auto state = recover(path);
        require(state.incomplete_tail && state.valid_bytes == good_end && state.last_lsn == 2 &&
                    state.values.at("a") == "old" && state.values.at("b") == "kept",
                "incomplete tail changed the valid prefix");
        require_error<std::runtime_error>([&] { Writer writer(path); }, "repair incomplete WAL tail");
    }

    fs::resize_file(path, good_end);
    {
        Writer writer(path);
        const std::array retry{Put{"a", "retried"}};
        require(writer.commit(retry) == 3, "append after repair did not continue from LSN 2");
    }
    const auto state = recover(path);
    require(!state.incomplete_tail && state.last_lsn == 3 && state.values.at("a") == "retried",
            "replay failed after tail repair");
    std::cout << "short header/body: kept LSN 1..2; offline repair and append reached LSN 3\n";
}

void flip_byte(const fs::path& path, std::uintmax_t offset) {
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    const auto position = static_cast<std::streamoff>(offset);
    file.seekg(position);
    const int byte = file.get();
    require(byte != std::char_traits<char>::eof(), "could not read the byte to corrupt");
    file.seekp(position);
    file.put(static_cast<char>(byte ^ 1));
    file.close();
    require(static_cast<bool>(file), "could not inject a bit flip");
}

void corrupt_record() {
    TempWal wal;
    wal.create_empty();
    const auto path = wal.path();
    std::uintmax_t second_start;
    std::uintmax_t second_end;
    {
        Writer writer(path);
        const std::array requests{Put{"a", "one"}, Put{"b", "two"}, Put{"c", "three"}};
        writer.commit(std::span{requests}.first(1));
        second_start = fs::file_size(path);
        writer.commit(std::span{requests}.subspan(1, 1));
        second_end = fs::file_size(path);
        writer.commit(std::span{requests}.last(1));
    }
    flip_byte(path, second_start);
    require_error<std::runtime_error>([&] { recover(path); }, "header checksum mismatch");
    flip_byte(path, second_start);
    flip_byte(path, second_end - 1);
    require_error<std::runtime_error>([&] { recover(path); }, "payload checksum mismatch");
    require_error<std::runtime_error>([&] { Writer writer(path); }, "payload checksum mismatch");
    std::cout << "corrupt header/payload checksum: stopped at the second record\n";
}

void fail_sync(std::FILE*) {
    throw std::runtime_error("injected sync failure");
}

void sync_failure() {
    TempWal wal;
    wal.create_empty();
    Writer writer(wal.path(), fail_sync);
    const std::array requests{Put{"a", "one"}, Put{"b", "two"}};
    require_error<std::runtime_error>([&] { writer.commit(requests); }, "injected sync failure");
    require(writer.sync_count() == 0, "failed sync counted as success");
    require_error<std::logic_error>([&] { writer.commit(requests); }, "recover before retrying");
    std::cout << "sync failure: no acknowledgement; writer fenced until recovery\n";
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
