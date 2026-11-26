// cluster.hpp
//
// 一个可执行的 Redis Cluster 教学模型：槽映射、节点、迁移状态、gossip 故障举报、
// epoch + quorum 故障转移、MOVED/ASK/CROSSSLOT/CLUSTERDOWN 回复。
//
// 它不是完整的 cluster bus 实现；消息格式、总线端口、gossip 频率在文章第 3 节讲，
// 这里只把“路由和故障转移的决策”做成可测试的状态机。

#pragma once

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "common.hpp"
#include "node.hpp"
#include "slot_map.hpp"

namespace archdesign {

class Cluster {
public:
    explicit Cluster(std::size_t slot_count = kClusterSlots);

    // ---- 拓扑 ----
    void add_master(NodeId id, Epoch epoch = 0);
    void add_replica(NodeId id, NodeId master_id, Epoch epoch = 0);
    void assign_range(Slot begin, Slot end, NodeId master_id);

    Node* node(const NodeId& id);
    const Node* node(const NodeId& id) const;
    const SlotMap& slots() const;
    std::size_t master_count() const;
    bool healthy() const;

    // ---- 路由/执行 ----
    Reply execute(const NodeId& receiving_node, const Command& command, bool asking = false);

    // ---- 槽迁移 ----
    void begin_migration(Slot slot, NodeId from, NodeId to);
    std::size_t migrate_keys(Slot slot, NodeId from, NodeId to,
                             std::size_t max_keys = static_cast<std::size_t>(-1));
    void finish_migration(Slot slot, NodeId to);
    bool is_migrating(Slot slot) const;
    bool is_importing(Slot slot) const;
    NodeId migration_target(Slot slot) const;
    NodeId import_source(Slot slot) const;

    // ---- gossip / 故障检测 ----
    void report_failure(const NodeId& reporter, const NodeId& target);
    bool failed(const NodeId& id) const;

    // ---- 故障转移 ----
    Epoch begin_failover(NodeId replica);
    bool grant_vote(NodeId voter, NodeId replica);
    bool promote(NodeId replica);
    Epoch current_epoch() const;
    std::size_t votes(NodeId replica) const;
    void sync_replica(NodeId replica);

private:
    std::vector<std::string> keys_of(const Command& command) const;
    Reply serve(Node& local, const Command& command) const;
    Reply moved(Slot slot, const NodeId& target) const;
    void refresh_health();

    SlotMap slots_;
    bool healthy_ = false;
    std::map<NodeId, Node> nodes_;
    std::map<Slot, NodeId> migrating_;  // slot -> destination
    std::map<Slot, NodeId> importing_;  // slot -> source

    Epoch current_epoch_ = 0;
    std::map<NodeId, std::set<NodeId>> votes_;
    std::map<NodeId, Epoch> last_vote_epoch_;
};

}  // namespace archdesign
