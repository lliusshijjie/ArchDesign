#include "client.hpp"

#include <utility>

#include "crc16.hpp"

namespace archdesign {

ClusterClient::ClusterClient(Cluster& cluster)
    : cluster_(cluster), cache_(cluster.slots().slot_count()) {}

NodeId ClusterClient::route_slot(Slot slot) {
    if (slot >= cache_.size()) return {};
    if (cache_[slot].empty()) cache_[slot] = cluster_.slots().owner(slot);
    return cache_[slot];
}

Reply ClusterClient::execute(const Command& command) {
    if (command.args.size() < 2) return Reply{ReplyType::Error, "no key", {}, 0};
    const Slot slot = key_hash_slot(command.args[1]);

    for (int attempt = 0; attempt < 8; ++attempt) {
        NodeId target = route_slot(slot);
        if (target.empty()) return Reply{ReplyType::ClusterDown, "unassigned slot", {}, slot};

        Reply reply = cluster_.execute(target, command, false);
        if (reply.type == ReplyType::Moved) {
            cache_[slot] = reply.target;
            ++moved_;
            continue;
        }
        if (reply.type == ReplyType::Ask) {
            ++ask_;
            return cluster_.execute(reply.target, command, true);
        }
        return reply;
    }
    return Reply{ReplyType::Error, "too many redirects", {}, slot};
}

void ClusterClient::learn_slot(Slot slot, NodeId node) {
    if (slot < cache_.size()) cache_[slot] = std::move(node);
}

std::size_t ClusterClient::moved_count() const { return moved_; }

std::size_t ClusterClient::ask_count() const { return ask_; }

}  // namespace archdesign
