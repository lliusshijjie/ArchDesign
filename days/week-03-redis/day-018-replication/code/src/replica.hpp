// replica.hpp
//
// Replica：保存自己处理到的 offset、当前 master 的 replid，以及一份 MiniKV。
//
// 它不自己删除过期键；读路径只隐藏。真正的删除由 master 合成 DEL 后
// 通过复制流送过来。这样主从时钟不一致也不会导致数据分叉。

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "common.hpp"
#include "kv.hpp"
#include "master.hpp"

namespace archdesign {

class Replica {
public:
    explicit Replica(std::string id, ReplId replid = "?");

    // 建立/恢复连接：内部发 PSYNC，处理 +CONTINUE 或 +FULLRESYNC 快照。
    bool connect(Master& master, i64 now_ms);

    // 在线状态下从 master 的 backlog 拉取新命令；落后太多则返回 false。
    bool poll(Master& master, i64 now_ms);

    // 向 master 上报已处理 offset（真实 Redis 每秒一次 REPLCONF ACK）。
    void ack(Master& master, i64 now_ms);

    // 测试/故障注入：模拟链路断开。再次 connect 会带原 replid + offset 发 PSYNC。
    void disconnect();

    const std::string& id() const;
    const ReplId& replid() const;
    Offset offset() const;
    bool online() const;
    const Kv& kv() const;
    Kv& kv();

private:
    void apply_stream(std::string_view bytes, i64 now_ms);

    std::string id_;
    ReplId replid_ = "?";
    Offset offset_ = 0;
    bool online_ = false;
    Kv kv_;
    std::string pending_;  // 尚未凑成完整 RESP 命令的尾部
};

}  // namespace archdesign
