// common.hpp
//
// Day 19 的基础类型：槽、节点 ID、逻辑 epoch、命令和重定向回复。
//
// Redis Cluster 的路由坐标只有一个：slot = CRC16(key) & 16383。
// epoch 用来给“拓扑变更/故障转移”排序，避免旧消息覆盖新视图。

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace archdesign {

using Slot = std::uint16_t;         // 0..16383
using NodeId = std::string;
using Epoch = std::uint64_t;

inline constexpr Slot kClusterSlots = 16384;

struct Command {
    std::vector<std::string> args;
};

enum class ReplyType {
    Ok,
    Moved,
    Ask,
    CrossSlot,
    TryAgain,
    ClusterDown,
    Error,
};

struct Reply {
    ReplyType type = ReplyType::Ok;
    std::string value;
    NodeId target;       // MOVED/ASK 的目标节点
    Slot slot = 0;
};

}  // namespace archdesign
