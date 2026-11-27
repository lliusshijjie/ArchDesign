// disk.hpp
//
// FaultDisk：把“持久化”显式化的最小磁盘模型。
//
// 真实文件系统里，write() 只把字节交给 page cache；只有 fsync 之后，
// 这些字节才被认为能在断电后存活。为了让实验可复现，这里用一个很小的
// 状态机模拟这个过程：
//
//   write/append   : logical 增长
//   sync           : durable = logical，并记录一次 fsync
//   power_loss()   : 所有 logical > durable 的文件截回 durable
//
// 这是“最坏情况”模型：真实机器可能恰好写回了更多字节，但绝不会更少。
// 它把“没 fsync 的确认写入在断电后消失”这件抽象的事，变成了可断言的结果。

#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace arch {

namespace fs = std::filesystem;

// 不可恢复的损坏：数据已经不在“可识别”的范围内，只能拒绝启动 / 拒绝读取。
class Corruption : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Disk {
public:
    explicit Disk(fs::path directory);

    // 整文件重写（SST 临时文件、manifest 临时文件）
    void write_file(const std::string& name, std::string_view bytes, bool sync);
    // 追加（WAL）
    void append(const std::string& name, std::string_view bytes, bool sync);
    // 截断：截断是破坏性的，剩余前缀立即视为持久（丢掉的字节不可能靠 cache 回来）
    void truncate(const std::string& name, std::uint64_t size, bool sync);
    void sync_file(const std::string& name);
    // 原子替换：manifest 提交的唯一手段
    void atomic_replace(const std::string& from, const std::string& to);
    void remove(const std::string& name);

    [[nodiscard]] bool exists(const std::string& name) const;
    [[nodiscard]] std::uint64_t size(const std::string& name) const;
    [[nodiscard]] std::string read_file(const std::string& name) const;
    [[nodiscard]] std::vector<std::string> list() const;

    // 模拟断电：未 fsync 的字节全部丢失。
    void power_loss();

    [[nodiscard]] std::uint64_t fsyncs() const { return fsyncs_; }
    [[nodiscard]] const fs::path& directory() const { return directory_; }

private:
    struct FileState {
        std::uint64_t durable = 0;
        std::uint64_t logical = 0;
    };

    [[nodiscard]] fs::path path_of(const std::string& name) const;
    FileState& state_for(const std::string& name);
    void sync_handle(std::FILE* file);

    fs::path directory_;
    std::map<std::string, FileState> state_;
    std::uint64_t fsyncs_ = 0;
};

} // namespace arch
