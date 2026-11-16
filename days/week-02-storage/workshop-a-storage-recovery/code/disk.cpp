// disk.cpp

#include "disk.hpp"

#include <algorithm>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace arch {

namespace {

std::FILE* open_or_throw(const fs::path& path, const char* mode) {
    std::FILE* file = std::fopen(path.string().c_str(), mode);
    if (file == nullptr) {
        throw std::runtime_error("cannot open " + path.string());
    }
    return file;
}

} // namespace

Disk::Disk(fs::path directory) : directory_(std::move(directory)) {}

fs::path Disk::path_of(const std::string& name) const {
    return directory_ / name;
}

Disk::FileState& Disk::state_for(const std::string& name) {
    const auto it = state_.find(name);
    if (it != state_.end()) {
        return it->second;
    }
    std::error_code error;
    const auto bytes = fs::file_size(path_of(name), error);
    FileState state;
    if (!error) {
        state.durable = bytes;
        state.logical = bytes;
    }
    return state_.emplace(name, state).first->second;
}

void Disk::sync_handle(std::FILE* file) {
    std::fflush(file);
#ifdef _WIN32
    if (_commit(_fileno(file)) != 0) {
        throw std::runtime_error("fsync failed");
    }
#else
    if (::fsync(fileno(file)) != 0) {
        throw std::runtime_error("fsync failed");
    }
#endif
    ++fsyncs_;
}

void Disk::write_file(const std::string& name, std::string_view bytes, bool sync) {
    auto& state = state_for(name);
    std::FILE* file = open_or_throw(path_of(name), "wb");
    const auto written = std::fwrite(bytes.data(), 1, bytes.size(), file);
    std::fflush(file);
    if (sync) {
        sync_handle(file);
    }
    std::fclose(file);
    if (written != bytes.size()) {
        throw std::runtime_error("short write: " + name);
    }
    // "wb" 先把旧内容截掉了；没有 fsync 的新内容在断电后不能算数。
    state.logical = bytes.size();
    state.durable = sync ? bytes.size() : 0;
}

void Disk::append(const std::string& name, std::string_view bytes, bool sync) {
    auto& state = state_for(name);
    std::FILE* file = open_or_throw(path_of(name), "ab");
    const auto written = std::fwrite(bytes.data(), 1, bytes.size(), file);
    std::fflush(file);
    if (sync) {
        sync_handle(file);
    }
    std::fclose(file);
    if (written != bytes.size()) {
        throw std::runtime_error("short append: " + name);
    }
    state.logical += bytes.size();
    if (sync) {
        state.durable = state.logical;
    }
}

void Disk::truncate(const std::string& name, std::uint64_t size, bool sync) {
    auto& state = state_for(name);
    const auto path = path_of(name);
#ifdef _WIN32
    const int fd = _open(path.string().c_str(), _O_RDWR | _O_CREAT | _O_BINARY,
                         _S_IREAD | _S_IWRITE);
    if (fd == -1) {
        throw std::runtime_error("cannot open for truncate: " + name);
    }
    if (_chsize_s(fd, static_cast<__int64>(size)) != 0) {
        _close(fd);
        throw std::runtime_error("truncate failed: " + name);
    }
    if (sync && _commit(fd) != 0) {
        _close(fd);
        throw std::runtime_error("fsync failed: " + name);
    }
    _close(fd);
#else
    const int fd = ::open(path.string().c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        throw std::runtime_error("cannot open for truncate: " + name);
    }
    if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
        ::close(fd);
        throw std::runtime_error("truncate failed: " + name);
    }
    if (sync && ::fsync(fd) != 0) {
        ::close(fd);
        throw std::runtime_error("fsync failed: " + name);
    }
    ::close(fd);
#endif
    if (sync) {
        ++fsyncs_;
    }
    // 截断立即生效：留下的前缀本来就是持久数据，丢掉的字节也不会回来。
    state.logical = size;
    state.durable = size;
}

void Disk::sync_file(const std::string& name) {
    auto& state = state_for(name);
    if (!exists(name)) {
        throw std::runtime_error("cannot fsync missing file: " + name);
    }
    std::FILE* file = open_or_throw(path_of(name), "r+b");
    sync_handle(file);
    std::fclose(file);
    state.durable = state.logical;
}

void Disk::atomic_replace(const std::string& from, const std::string& to) {
    if (!exists(from)) {
        throw std::runtime_error("atomic_replace: missing " + from);
    }
#ifdef _WIN32
    const bool ok =
        ::MoveFileExW(path_of(from).c_str(), path_of(to).c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) {
        throw std::runtime_error("atomic_replace failed: " + from + " -> " + to);
    }
#else
    if (::rename(path_of(from).string().c_str(), path_of(to).string().c_str()) != 0) {
        throw std::runtime_error("atomic_replace failed: " + from + " -> " + to);
    }
#endif
    const auto it = state_.find(from);
    if (it != state_.end()) {
        state_[to] = it->second;
        state_.erase(it);
    } else {
        (void)state_for(to);
    }
}

void Disk::remove(const std::string& name) {
    std::error_code error;
    fs::remove(path_of(name), error);
    if (error) {
        throw std::runtime_error("remove failed: " + name + ": " + error.message());
    }
    state_.erase(name);
}

bool Disk::exists(const std::string& name) const {
    return fs::exists(path_of(name));
}

std::uint64_t Disk::size(const std::string& name) const {
    std::error_code error;
    const auto bytes = fs::file_size(path_of(name), error);
    if (error) {
        throw std::runtime_error("cannot stat " + name);
    }
    return bytes;
}

std::string Disk::read_file(const std::string& name) const {
    std::FILE* file = open_or_throw(path_of(name), "rb");
    std::string out;
    char buffer[8192];
    for (;;) {
        const auto read = std::fread(buffer, 1, sizeof(buffer), file);
        out.append(buffer, read);
        if (read < sizeof(buffer)) {
            break;
        }
    }
    const bool failed = std::ferror(file) != 0;
    std::fclose(file);
    if (failed) {
        throw std::runtime_error("read failed: " + name);
    }
    return out;
}

std::vector<std::string> Disk::list() const {
    std::vector<std::string> names;
    std::error_code error;
    fs::directory_iterator iterator(directory_, error);
    if (error) {
        throw std::runtime_error("cannot list " + directory_.string() + ": " + error.message());
    }
    for (const auto& entry : iterator) {
        if (entry.is_regular_file()) {
            names.push_back(entry.path().filename().string());
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

void Disk::power_loss() {
    for (auto& [name, state] : state_) {
        if (state.logical <= state.durable) {
            continue;
        }
        const auto target = state.durable;
        const auto path = path_of(name);
        if (fs::exists(path)) {
            const auto actual = fs::file_size(path);
            if (actual > target) {
#ifdef _WIN32
                const int fd = _open(path.string().c_str(), _O_RDWR | _O_BINARY);
                if (fd != -1) {
                    (void)_chsize_s(fd, static_cast<__int64>(target));
                    _close(fd);
                }
#else
                (void)::truncate(path.string().c_str(), static_cast<off_t>(target));
#endif
            }
        }
        state.logical = target;
    }
}

} // namespace arch
