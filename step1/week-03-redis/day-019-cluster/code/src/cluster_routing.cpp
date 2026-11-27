// cluster_routing.cpp
//
// 请求到达某个节点后：算槽 -> 查迁移状态 -> 本地执行 / MOVED / ASK / TRYAGAIN。
// 以及槽迁移的三个动作：begin / migrate_keys / finish。

#include "cluster.hpp"

#include <algorithm>
#include <stdexcept>

#include "crc16.hpp"

namespace archdesign {

Reply Cluster::execute(const NodeId& receiving_node, const Command& command, bool asking) {
    const std::vector<std::string> keys = keys_of(command);
    if (keys.empty()) return Reply{ReplyType::Error, "no key in command", {}, 0};
    if (!healthy()) return Reply{ReplyType::ClusterDown, "CLUSTERDOWN", {}, 0};

    std::vector<Slot> key_slots;
    key_slots.reserve(keys.size());
    for (const std::string& key : keys) {
        const Slot slot = key_hash_slot(key);
        if (!slots_.assigned(slot)) return Reply{ReplyType::ClusterDown, "unassigned slot", {}, slot};
        key_slots.push_back(slot);
    }
    const Slot slot = key_slots.front();
    for (Slot s : key_slots) {
        if (s != slot) return Reply{ReplyType::CrossSlot, "CROSSSLOT", {}, s};
    }

    Node* local = node(receiving_node);
    if (local == nullptr) return Reply{ReplyType::Error, "unknown node", {}, slot};
    if (!local->master) return moved(slot, local->master_id);

    auto migrating = migrating_.find(slot);
    auto importing = importing_.find(slot);
    if (migrating != migrating_.end() && importing != importing_.end()) {
        if (receiving_node == importing->second) {  // source
            std::size_t present = 0;
            for (const std::string& key : keys) present += local->data.count(key) != 0 ? 1 : 0;
            if (present == keys.size()) return serve(*local, command);
            if (present > 0) return Reply{ReplyType::TryAgain, "TRYAGAIN", {}, slot};
            return Reply{ReplyType::Ask, "ASK", migrating->second, slot};
        }
        if (receiving_node == migrating->second) {  // target
            if (asking) return serve(*local, command);
            return moved(slot, importing->second);
        }
    }

    const NodeId& owner = slots_.owner(slot);
    if (owner == receiving_node) return serve(*local, command);
    return moved(slot, owner);
}

void Cluster::begin_migration(Slot slot, NodeId from, NodeId to) {
    if (slots_.owner(slot) != from) throw std::runtime_error("slot is not owned by source");
    if (node(from) == nullptr || node(to) == nullptr) throw std::runtime_error("unknown node");
    migrating_[slot] = to;
    importing_[slot] = from;
}

std::size_t Cluster::migrate_keys(Slot slot, NodeId from, NodeId to, std::size_t max_keys) {
    auto fit = migrating_.find(slot);
    auto iit = importing_.find(slot);
    if (fit == migrating_.end() || iit == importing_.end() || fit->second != to ||
        iit->second != from) {
        throw std::runtime_error("slot is not migrating from source to target");
    }
    Node* source = node(from);
    Node* target = node(to);
    if (source == nullptr || target == nullptr) throw std::runtime_error("unknown node");

    std::vector<std::string> victims;
    for (const auto& [key, value] : source->data) {
        (void)value;
        if (key_hash_slot(key) == slot && victims.size() < max_keys) victims.push_back(key);
    }
    for (const std::string& key : victims) {
        auto it = source->data.find(key);
        if (it == source->data.end()) continue;
        target->data[key] = it->second;
        source->data.erase(it);
    }
    return victims.size();
}

void Cluster::finish_migration(Slot slot, NodeId to) {
    auto fit = migrating_.find(slot);
    auto iit = importing_.find(slot);
    if (fit == migrating_.end() || iit == importing_.end() || fit->second != to) {
        throw std::runtime_error("slot migration not active for target");
    }
    for (const auto& [key, value] : node(iit->second)->data) {
        (void)value;
        if (key_hash_slot(key) == slot) {
            throw std::runtime_error("source still has keys in slot");
        }
    }
    slots_.assign(slot, to);
    migrating_.erase(fit);
    importing_.erase(iit);
    refresh_health();
}

bool Cluster::is_migrating(Slot slot) const { return migrating_.count(slot) != 0; }

bool Cluster::is_importing(Slot slot) const { return importing_.count(slot) != 0; }

NodeId Cluster::migration_target(Slot slot) const {
    auto it = migrating_.find(slot);
    return it == migrating_.end() ? NodeId{} : it->second;
}

NodeId Cluster::import_source(Slot slot) const {
    auto it = importing_.find(slot);
    return it == importing_.end() ? NodeId{} : it->second;
}

}  // namespace archdesign
