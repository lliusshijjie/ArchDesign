// backlog.hpp
//
// 复制积压缓冲区（replication backlog）：一个按字节计数的环形缓冲。
//
// offset 的语义：
//   [start, end) 是当前还能提供的字节范围；
//   end == master_repl_offset；replica 请求 offset 时，
//   只要 offset 落在 [start, end] 内，就能从 backlog 续传。
//   offset == end 表示“我全跟上了，只差新命令”，仍是有效的续传点。

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"

namespace archdesign {

class Backlog {
public:
    explicit Backlog(std::size_t capacity);

    void append(std::string_view bytes);
    // 测试/故障转移用：用一段已有历史初始化缓冲区。
    void seed(Offset start, std::string_view bytes);
    bool contains(Offset offset) const;
    std::string read_from(Offset offset) const;

    Offset start() const;
    Offset end() const;
    std::size_t size() const;
    std::size_t capacity() const;

private:
    std::vector<char> ring_;
    std::size_t start_index_ = 0;  // 第一个有效字节的物理下标
    std::size_t size_ = 0;
    Offset start_ = 0;
};

}  // namespace archdesign
