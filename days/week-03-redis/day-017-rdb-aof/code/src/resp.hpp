// resp.hpp
//
// AOF 使用的 Redis 协议（RESP）编解码。
// AOF 不是自定义二进制格式：每条写命令就是一段 *N\r\n$len\r\n... 的数组。

#pragma once

#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"

namespace archdesign {

struct Command {
    std::vector<std::string> args;

    Command() = default;
    Command(std::initializer_list<std::string> values) : args(values) {}
};

std::string resp_encode(const Command& command);

struct RespParseResult {
    std::vector<Command> commands;
    std::size_t consumed = 0;  // 最后一个完整命令结束的位置
    bool truncated = false;    // 尾部是否有半条命令
};

// 解析完整命令；尾部不完整时返回 truncated=true，而不是抛异常。
// 中间出现非法字节时抛异常（不是“截断”，不能静默跳过）。
RespParseResult resp_parse(std::string_view data);

}  // namespace archdesign
