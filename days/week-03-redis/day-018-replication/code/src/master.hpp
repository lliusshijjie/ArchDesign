// master.hpp
//
// Master：维护 (replid, offset)、环形 backlog、全量同步快照和 replica ACK。
//
// 复制流只有一条：每条写命令按 RESP 追加到 backlog，offset 按字节增长。
// replica 发 PSYNC 时，master 只回答两件事：
//   +CONTINUE <replid>   —— 从你的 offset 继续（backlog 里还有）
//   +FULLRESYNC ...      —— 从当前快照重新开始（backlog 不够/ID 不匹配）

#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "backlog.hpp"
#include "common.hpp"
#include "kv.hpp"
#include "resp.hpp"

namespace archdesign {

struct PsyncResult {
    enum class Type { Continue, Full };

    Type type = Type::Full;
    ReplId replid;
    Offset offset = 0;       // Continue: 请求的 offset；Full: snapshot 的 offset
    std::string payload;     // Continue: backlog 字节；Full: 快照
    u64 full_sync_id = 0;    // Full 时用于领取 full sync 期间的 tail
};

class Master {
public:
    Master(ReplId replid, std::size_t backlog_capacity);
    Master(ReplId replid, ReplId replid2, Offset second_replid_offset,
           std::size_t backlog_capacity);

    // ---- 写命令：先改内存，再进入复制流 ----
    void set(const std::string& key, const std::string& value, i64 now_ms, i64 ttl_ms = -1);
    bool del(const std::string& key);
    bool expire_at(const std::string& key, i64 when_ms);
    bool persist(const std::string& key);
    std::size_t expire_due(i64 now_ms);

    // ---- PSYNC ----
    PsyncResult psync(const ReplId& requested_replid, Offset offset, i64 now_ms);
    std::string finish_full_sync(u64 full_sync_id);

    // ---- 在线 replica 拉取 + ACK ----
    bool read_from(Offset offset, std::string& out) const;
    void ack(const std::string& replica_id, Offset offset, i64 now_ms);
    int count_acks(Offset target) const;
    int wait_for_acks(int replicas, Offset target, i64 timeout_ms, i64 now_ms) const;

    // ---- 观测/测试 ----
    const ReplId& replid() const;
    const ReplId& replid2() const;
    Offset second_replid_offset() const;
    Offset offset() const;
    const Backlog& backlog() const;
    const Kv& kv() const;

    // 故障转移测试用：把旧 master 的一段历史装进新 master 的 backlog。
    void seed_backlog(Offset start, std::string_view bytes);
    // 更完整的历史镜像：promoted 的 offset / 数据 / backlog 都来自旧主库。
    void seed_history(Offset current_offset, Offset backlog_start,
                      std::string_view backlog_bytes, Kv kv);

private:
    struct FullSyncState {
        ReplId replid;
        Offset offset = 0;
        std::string snapshot;
        std::string tail;
    };

    struct ReplicaAck {
        Offset offset = 0;
        i64 last_ack_ms = 0;
        bool online = false;
    };

    PsyncResult begin_full_sync();
    void feed(const Command& command);

    ReplId replid_;
    ReplId replid2_;
    Offset second_replid_offset_ = -1;
    Offset offset_ = 0;
    Backlog backlog_;
    Kv kv_;
    std::map<u64, FullSyncState> full_syncs_;
    u64 next_full_sync_id_ = 1;
    std::map<std::string, ReplicaAck> replicas_;
};

}  // namespace archdesign
