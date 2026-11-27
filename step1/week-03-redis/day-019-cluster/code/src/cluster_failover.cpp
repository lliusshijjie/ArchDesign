// cluster_failover.cpp
//
// 简化版 gossip 故障举报 + epoch/quorum 故障转移。
//
// 真实 Redis 用 cluster bus 的 PING/PONG 携带 gossip 段，master 在
// cluster-node-timeout 后把节点标成 FAIL；replica 发起 FAILOVER_AUTH_REQUEST，
// 拿到多数 master 的 ACK 后提升自己并接管槽。

#include "cluster.hpp"

#include <algorithm>
#include <stdexcept>

namespace archdesign {

void Cluster::report_failure(const NodeId& reporter, const NodeId& target) {
    Node* r = node(reporter);
    Node* t = node(target);
    if (r == nullptr || t == nullptr || !r->master || r->failed) return;
    if (reporter == target) return;

    t->failure_reports.insert(reporter);
    const std::size_t quorum = master_count() / 2 + 1;
    if (t->failure_reports.size() >= quorum) t->failed = true;
    refresh_health();
}

bool Cluster::failed(const NodeId& id) const {
    const Node* n = node(id);
    return n != nullptr && n->failed;
}

Epoch Cluster::begin_failover(NodeId replica) {
    Node* r = node(replica);
    if (r == nullptr || r->master) throw std::runtime_error("failover requires a replica");
    Node* master = node(r->master_id);
    if (master == nullptr || !master->failed) {
        throw std::runtime_error("master is not marked as failed");
    }
    ++current_epoch_;
    votes_[replica].clear();
    return current_epoch_;
}

bool Cluster::grant_vote(NodeId voter, NodeId replica) {
    Node* v = node(voter);
    Node* r = node(replica);
    if (v == nullptr || r == nullptr || !v->master || v->failed) return false;
    if (r->master || r->master_id.empty()) return false;
    Node* old_master = node(r->master_id);
    if (old_master == nullptr || !old_master->failed) return false;
    if (current_epoch_ == 0) return false;

    auto last = last_vote_epoch_.find(voter);
    if (last != last_vote_epoch_.end() && last->second >= current_epoch_) return false;
    last_vote_epoch_[voter] = current_epoch_;
    votes_[replica].insert(voter);
    return true;
}

bool Cluster::promote(NodeId replica) {
    Node* r = node(replica);
    if (r == nullptr || r->master || r->master_id.empty()) return false;
    Node* old_master = node(r->master_id);
    if (old_master == nullptr || !old_master->failed) return false;

    const std::size_t quorum = master_count() / 2 + 1;
    if (votes_[replica].size() < quorum) return false;

    r->master = true;
    r->master_id.clear();
    r->epoch = current_epoch_;
    old_master->master = false;
    old_master->master_id = replica;

    for (std::size_t slot = 0; slot < slots_.slot_count(); ++slot) {
        if (slots_.owner(static_cast<Slot>(slot)) == old_master->id) {
            slots_.assign(static_cast<Slot>(slot), replica);
        }
    }
    refresh_health();
    return true;
}

Epoch Cluster::current_epoch() const { return current_epoch_; }

std::size_t Cluster::votes(NodeId replica) const {
    auto it = votes_.find(replica);
    return it == votes_.end() ? 0 : it->second.size();
}

void Cluster::sync_replica(NodeId replica) {
    Node* r = node(replica);
    if (r == nullptr || r->master) throw std::runtime_error("sync target is not a replica");
    Node* master = node(r->master_id);
    if (master == nullptr) throw std::runtime_error("replica master missing");
    r->data = master->data;
}

}  // namespace archdesign
