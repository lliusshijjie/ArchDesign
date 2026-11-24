// resp.hpp
//
// Redis 复制流就是 RESP 命令流：master 把每条写命令编码成
// *N\r\n$len\r\n... 追加到复制缓冲区；replica 再按顺序解析、执行。
//
// 本文件只做“完整命令”的编解码。复制流可能被拆包，所以 parser 返回
// consumed：调用方保留未消费的尾部，等下一次拼上继续解析。

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"

namespace archdesign {

std::string resp_encode(const Command& command);

struct RespParseResult {
    std::vector<Command> commands;
    std::size_t consumed = 0;
    bool truncated = false;
};

RespParseResult resp_parse(std::string_view data);

}  // namespace archdesign
