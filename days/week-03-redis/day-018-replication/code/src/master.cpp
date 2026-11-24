#include "master.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace archdesign {

Master::Master(ReplId replid, std::size_t backlog_capacity)
    : replid_(std::move(replid)), backlog_(backlog_capacity) {}

Master::Master(ReplId replid, ReplId replid2, Offset second_replid_offset,
               std::size_t backlog_capacity)
    : replid_(std::move(replid)),
      replid2_(std::move(replid2)),
      second_replid_offset_(second_replid_offset),
      backlog_(backlog_capacity) {}

void Master::feed(const Command& command) {
    const std::string bytes = resp_encode(command);
    backlog_.append(bytes);
    offset_ += static_cast<Offset>(bytes.size());
    for (auto& [id, state] : full_syncs_) {
        (void)id;
        state.tail += bytes;
    }
}

void Master::set(const std::string& key, const std::string& value, i64 now_ms, i64 ttl_ms) {
    const i64 expire_at = ttl_ms < 0 ? -1 : now_ms + ttl_ms;
    kv_.set(key, value, expire_at);
    feed(Command{{"SET", key, value}});
    if (expire_at != -1) {
        feed(Command{{"PEXPIREAT", key, std::to_string(expire_at)}});
    }
}

bool Master::del(const std::string& key) {
    if (!kv_.del(key)) return false;
    feed(Command{{"DEL", key}});
    return true;
}

bool Master::expire_at(const std::string& key, i64 when_ms) {
    if (!kv_.expire_at(key, when_ms)) return false;
    feed(Command{{"PEXPIREAT", key, std::to_string(when_ms)}});
    return true;
}

bool Master::persist(const std::string& key) {
    if (!kv_.persist(key)) return false;
    feed(Command{{"PERSIST", key}});
    return true;
}

std::size_t Master::expire_due(i64 now_ms) {
    const std::vector<std::string> keys = kv_.expired_keys(now_ms);
    for (const std::string& key : keys) {
        if (kv_.del(key)) feed(Command{{"DEL", key}});
    }
    return keys.size();
}

PsyncResult Master::begin_full_sync() {
    FullSyncState state;
    state.replid = replid_;
    state.offset = offset_;
    state.snapshot = kv_.snapshot();
    const u64 id = next_full_sync_id_++;
    full_syncs_.emplace(id, std::move(state));

    PsyncResult result;
    result.type = PsyncResult::Type::Full;
    result.replid = replid_;
    result.offset = offset_;
    result.payload = full_syncs_.at(id).snapshot;
    result.full_sync_id = id;
    return result;
}

PsyncResult Master::psync(const ReplId& requested_replid, Offset offset, i64 now_ms) {
    (void)now_ms;
    const bool id_matches =
        requested_replid == replid_ ||
        (!replid2_.empty() && requested_replid == replid2_ && offset <= second_replid_offset_);

    if (id_matches && offset >= 0 && offset <= offset_ && backlog_.contains(offset)) {
        PsyncResult result;
        result.type = PsyncResult::Type::Continue;
        result.replid = replid_;
        result.offset = offset;
        result.payload = backlog_.read_from(offset);
        return result;
    }
    return begin_full_sync();
}

std::string Master::finish_full_sync(u64 full_sync_id) {
    auto it = full_syncs_.find(full_sync_id);
    if (it == full_syncs_.end()) throw std::runtime_error("full sync id not found");
    std::string tail = std::move(it->second.tail);
    full_syncs_.erase(it);
    return tail;
}

bool Master::read_from(Offset offset, std::string& out) const {
    if (offset < 0 || offset > offset_ || !backlog_.contains(offset)) return false;
    out = backlog_.read_from(offset);
    return true;
}

void Master::ack(const std::string& replica_id, Offset offset, i64 now_ms) {
    ReplicaAck& ack = replicas_[replica_id];
    ack.offset = std::max(ack.offset, offset);
    ack.last_ack_ms = now_ms;
    ack.online = true;
}

int Master::count_acks(Offset target) const {
    int count = 0;
    for (const auto& [id, ack] : replicas_) {
        (void)id;
        if (ack.online && ack.offset >= target) ++count;
    }
    return count;
}

int Master::wait_for_acks(int replicas, Offset target, i64 timeout_ms, i64 now_ms) const {
    (void)timeout_ms;
    (void)now_ms;
    if (replicas <= 0) return 0;
    return count_acks(target);
}

const ReplId& Master::replid() const { return replid_; }

const ReplId& Master::replid2() const { return replid2_; }

Offset Master::second_replid_offset() const { return second_replid_offset_; }

Offset Master::offset() const { return offset_; }

const Backlog& Master::backlog() const { return backlog_; }

const Kv& Master::kv() const { return kv_; }

void Master::seed_backlog(Offset start, std::string_view bytes) {
    backlog_.seed(start, bytes);
}

void Master::seed_history(Offset current_offset, Offset backlog_start,
                          std::string_view backlog_bytes, Kv kv) {
    offset_ = current_offset;
    kv_ = std::move(kv);
    backlog_.seed(backlog_start, backlog_bytes);
}

}  // namespace archdesign
