#include "mem_fs.hpp"

#include <algorithm>
#include <stdexcept>

namespace archdesign {

void MemFs::append(const std::string& path, std::string_view bytes) {
    if (fail_writes_) throw std::runtime_error("MemFs: injected write failure");
    auto& file = files_[path];
    file.data.append(bytes.data(), bytes.size());
    ++writes_;
}

void MemFs::write_file(const std::string& path, std::string_view bytes) {
    if (fail_writes_) throw std::runtime_error("MemFs: injected write failure");
    auto& file = files_[path];
    file.data.assign(bytes.data(), bytes.size());
    file.durable = 0;
    ++writes_;
}

void MemFs::fsync(const std::string& path) {
    auto it = files_.find(path);
    if (it == files_.end()) throw std::runtime_error("MemFs: fsync of missing file " + path);
    it->second.durable = it->second.data.size();
    ++fsyncs_;
}

void MemFs::rename(const std::string& from, const std::string& to) {
    auto it = files_.find(from);
    if (it == files_.end()) throw std::runtime_error("MemFs: rename of missing file " + from);
    MemFile moved = std::move(it->second);
    files_.erase(it);
    files_[to] = std::move(moved);
}

void MemFs::remove(const std::string& path) { files_.erase(path); }

void MemFs::truncate(const std::string& path, std::size_t size) {
    auto it = files_.find(path);
    if (it == files_.end()) throw std::runtime_error("MemFs: truncate of missing file " + path);
    if (size > it->second.data.size()) throw std::runtime_error("MemFs: truncate beyond EOF");
    it->second.data.resize(size);
    it->second.durable = std::min(it->second.durable, size);
}

bool MemFs::exists(const std::string& path) const {
    return files_.find(path) != files_.end();
}

std::string MemFs::read(const std::string& path) const {
    auto it = files_.find(path);
    if (it == files_.end()) throw std::runtime_error("MemFs: read of missing file " + path);
    return it->second.data;
}

std::size_t MemFs::size(const std::string& path) const {
    auto it = files_.find(path);
    return it == files_.end() ? 0 : it->second.data.size();
}

std::size_t MemFs::durable_size(const std::string& path) const {
    auto it = files_.find(path);
    return it == files_.end() ? 0 : it->second.durable;
}

void MemFs::crash() {
    for (auto& [path, file] : files_) {
        (void)path;
        if (file.data.size() > file.durable) file.data.resize(file.durable);
    }
    ++crashes_;
}

void MemFs::set_fail_writes(bool value) { fail_writes_ = value; }

u64 MemFs::fsyncs() const { return fsyncs_; }

u64 MemFs::writes() const { return writes_; }

u64 MemFs::crashes() const { return crashes_; }

}  // namespace archdesign
