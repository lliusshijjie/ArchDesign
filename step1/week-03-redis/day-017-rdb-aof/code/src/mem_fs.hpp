// mem_fs.hpp
//
// 把「OS page cache / 磁盘」拆成两层：
//
//   data    = 当前文件内容（包括已 write、未 fsync 的尾部）
//   durable = 已经 fsync 的前缀长度
//
// crash() 只保留 durable 前缀，模拟掉电时 page cache 里未落盘的数据。

#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>

#include "common.hpp"

namespace archdesign {

struct MemFile {
    std::string data;
    std::size_t durable = 0;
};

class MemFs {
public:
    void append(const std::string& path, std::string_view bytes);
    void write_file(const std::string& path, std::string_view bytes);
    void fsync(const std::string& path);
    void rename(const std::string& from, const std::string& to);
    void remove(const std::string& path);
    void truncate(const std::string& path, std::size_t size);

    bool exists(const std::string& path) const;
    std::string read(const std::string& path) const;
    std::size_t size(const std::string& path) const;
    std::size_t durable_size(const std::string& path) const;

    // 模拟掉电：所有文件只剩 fsync 过的前缀。
    void crash();

    void set_fail_writes(bool value);
    u64 fsyncs() const;
    u64 writes() const;
    u64 crashes() const;

private:
    std::map<std::string, MemFile> files_;
    bool fail_writes_ = false;
    u64 fsyncs_ = 0;
    u64 writes_ = 0;
    u64 crashes_ = 0;
};

}  // namespace archdesign
