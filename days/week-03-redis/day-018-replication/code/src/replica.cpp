#include "replica.hpp"

#include <stdexcept>
#include <utility>

#include "resp.hpp"

namespace archdesign {

Replica::Replica(std::string id, ReplId replid)
    : id_(std::move(id)), replid_(std::move(replid)) {}

bool Replica::connect(Master& master, i64 now_ms) {
    const PsyncResult result = master.psync(replid_, offset_, now_ms);
    if (result.type == PsyncResult::Type::Continue) {
        replid_ = result.replid;
        apply_stream(result.payload, now_ms);
        online_ = true;
        return true;
    }

    // FULLRESYNC：先装快照，再补 full sync 期间 master 追加的 tail。
    kv_.load(result.payload);
    offset_ = result.offset;
    pending_.clear();
    replid_ = result.replid;
    const std::string tail = master.finish_full_sync(result.full_sync_id);
    apply_stream(tail, now_ms);
    online_ = true;
    return true;
}

bool Replica::poll(Master& master, i64 now_ms) {
    if (!online_) return false;
    std::string data;
    if (!master.read_from(offset_, data)) {
        online_ = false;
        return false;
    }
    apply_stream(data, now_ms);
    return true;
}

void Replica::ack(Master& master, i64 now_ms) { master.ack(id_, offset_, now_ms); }

void Replica::disconnect() { online_ = false; }

void Replica::apply_stream(std::string_view bytes, i64 now_ms) {
    pending_.append(bytes.data(), bytes.size());
    const RespParseResult parsed = resp_parse(pending_);
    for (const Command& command : parsed.commands) kv_.apply(command, now_ms);
    offset_ += static_cast<Offset>(parsed.consumed);
    pending_.erase(0, parsed.consumed);
    if (!pending_.empty() && !parsed.truncated) {
        throw std::runtime_error("replica: parser left bytes without truncation");
    }
}

const std::string& Replica::id() const { return id_; }

const ReplId& Replica::replid() const { return replid_; }

Offset Replica::offset() const { return offset_; }

bool Replica::online() const { return online_; }

const Kv& Replica::kv() const { return kv_; }

Kv& Replica::kv() { return kv_; }

}  // namespace archdesign
