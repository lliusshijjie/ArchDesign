// cluster_core.cpp
//
// 拓扑管理、健康检查、命令解析与本地执行。

#include "cluster.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace archdesign {

namespace {

std::string upper(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

}  // namespace

Cluster::Cluster(std::size_t slot_count) : slots_(slot_count) {}

void Cluster::add_master(NodeId id, Epoch epoch) {
    Node n;
    n.id = id;
    n.master = true;
    n.epoch = epoch;
    nodes_[std::move(id)] = std::move(n);
    refresh_health();
}

void Cluster::add_replica(NodeId id, NodeId master_id, Epoch epoch) {
    auto it = nodes_.find(master_id);
    if (it == nodes_.end() || !it->second.master) {
        throw std::runtime_error("replica master not found: " + master_id);
    }
    Node n;
    n.id = id;
    n.master_id = master_id;
    n.master = false;
    n.epoch = epoch;
    nodes_[std::move(id)] = std::move(n);
    refresh_health();
}

void Cluster::assign_range(Slot begin, Slot end, NodeId master_id) {
    Node* n = node(master_id);
    if (n == nullptr || !n->master) throw std::runtime_error("slot owner is not a master");
    slots_.assign_range(begin, end, std::move(master_id));
    refresh_health();
}

Node* Cluster::node(const NodeId& id) {
    auto it = nodes_.find(id);
    return it == nodes_.end() ? nullptr : &it->second;
}

const Node* Cluster::node(const NodeId& id) const {
    auto it = nodes_.find(id);
    return it == nodes_.end() ? nullptr : &it->second;
}

const SlotMap& Cluster::slots() const { return slots_; }

std::size_t Cluster::master_count() const {
    return static_cast<std::size_t>(
        std::count_if(nodes_.begin(), nodes_.end(), [](const auto& item) {
            return item.second.master;
        }));
}

bool Cluster::healthy() const { return healthy_; }

void Cluster::refresh_health() {
    healthy_ = true;
    if (!slots_.all_assigned()) {
        healthy_ = false;
        return;
    }
    for (std::size_t i = 0; i < slots_.slot_count(); ++i) {
        const NodeId& owner = slots_.owner(static_cast<Slot>(i));
        const Node* n = node(owner);
        if (n == nullptr || !n->master || n->failed) {
            healthy_ = false;
            return;
        }
    }
}

std::vector<std::string> Cluster::keys_of(const Command& command) const {
    if (command.args.empty()) return {};
    const std::string verb = upper(command.args[0]);
    if (verb == "SET" || verb == "GET") {
        if (command.args.size() < 2) return {};
        return {command.args[1]};
    }
    if (verb == "DEL" || verb == "MGET") {
        std::vector<std::string> keys;
        for (std::size_t i = 1; i < command.args.size(); ++i) keys.push_back(command.args[i]);
        return keys;
    }
    return {};
}

Reply Cluster::serve(Node& local, const Command& command) const {
    if (command.args.empty()) return Reply{ReplyType::Error, "empty command", {}, 0};
    const std::string verb = upper(command.args[0]);

    if (verb == "SET") {
        if (command.args.size() != 3) return Reply{ReplyType::Error, "SET arity", {}, 0};
        local.data[command.args[1]] = command.args[2];
        return Reply{ReplyType::Ok, "OK", {}, 0};
    }
    if (verb == "GET") {
        if (command.args.size() != 2) return Reply{ReplyType::Error, "GET arity", {}, 0};
        auto it = local.data.find(command.args[1]);
        return Reply{ReplyType::Ok, it == local.data.end() ? std::string() : it->second, {}, 0};
    }
    if (verb == "DEL") {
        std::size_t removed = 0;
        for (std::size_t i = 1; i < command.args.size(); ++i) removed += local.data.erase(command.args[i]);
        return Reply{ReplyType::Ok, std::to_string(removed), {}, 0};
    }
    if (verb == "MGET") {
        std::string out;
        for (std::size_t i = 1; i < command.args.size(); ++i) {
            auto it = local.data.find(command.args[i]);
            if (i > 1) out.push_back(',');
            if (it != local.data.end()) out += it->second;
        }
        return Reply{ReplyType::Ok, out, {}, 0};
    }
    return Reply{ReplyType::Error, "unsupported command", {}, 0};
}

Reply Cluster::moved(Slot slot, const NodeId& target) const {
    return Reply{ReplyType::Moved, "MOVED", target, slot};
}

}  // namespace archdesign
