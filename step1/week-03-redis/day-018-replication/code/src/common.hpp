// common.hpp
//
// Day 18 的基础类型：复制偏移量、复制 ID、命令。
//
// 复制的核心坐标是 (replid, offset)：一个 replid 标记一段历史，
// offset 是这段历史里“已经产生了多少字节”的逻辑时间。

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace archdesign {

using i64 = std::int64_t;
using u64 = std::uint64_t;
using u32 = std::uint32_t;

// Redis 的 master_repl_offset / repl_ack_off 都是 long long。
using Offset = i64;

using ReplId = std::string;

struct Command {
    std::vector<std::string> args;
};

}  // namespace archdesign
