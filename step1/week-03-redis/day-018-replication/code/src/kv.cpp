#include "kv.hpp"

#include <algorithm>
#include <stdexcept>

namespace archdesign {

namespace {

void put_u32(std::string& out, u32 value) {
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
}

u32 read_u32(std::string_view data, std::size_t& pos) {
    if (pos + 4 > data.size()) throw std::runtime_error("KV snapshot: truncated length");
    u32 value = 0;
    for (int i = 0; i < 4; ++i) {
        value = (value << 8) | static_cast<u32>(static_cast<unsigned char>(data[pos++]));
    }
    return value;
}

void put_i64(std::string& out, i64 value) {
    u64 bits = static_cast<u64>(value);
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((bits >> (8 * i)) & 0xff));
}

i64 read_i64(std::string_view data, std::size_t& pos) {
    if (pos + 8 > data.size()) throw std::runtime_error("KV snapshot: truncated TTL");
    u64 bits = 0;
    for (int i = 0; i < 8; ++i) {
        bits |= static_cast<u64>(static_cast<unsigned char>(data[pos++])) << (8 * i);
    }
    return static_cast<i64>(bits);
}

void put_string(std::string& out, std::string_view value) {
    put_u32(out, static_cast<u32>(value.size()));
    out.append(value.data(), value.size());
}

std::string read_string(std::string_view data, std::size_t& pos) {
    const u32 size = read_u32(data, pos);
    if (size > data.size() - pos) throw std::runtime_error("KV snapshot: truncated string");
    std::string value(data.substr(pos, size));
    pos += size;
    return value;
}

std::string upper(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

}  // namespace

void Kv::set(std::string key, std::string value, i64 expire_at_ms) {
    data_[std::move(key)] = Value{std::move(value), expire_at_ms};
}

bool Kv::del(std::string_view key) { return data_.erase(std::string(key)) != 0; }

bool Kv::expire_at(std::string_view key, i64 when_ms) {
    auto it = data_.find(std::string(key));
    if (it == data_.end()) return false;
    it->second.expire_at_ms = when_ms;
    return true;
}

bool Kv::persist(std::string_view key) {
    auto it = data_.find(std::string(key));
    if (it == data_.end() || it->second.expire_at_ms == -1) return false;
    it->second.expire_at_ms = -1;
    return true;
}

std::optional<std::string> Kv::get(std::string_view key, i64 now_ms) const {
    auto it = data_.find(std::string(key));
    if (it == data_.end()) return std::nullopt;
    if (it->second.expire_at_ms != -1 && it->second.expire_at_ms <= now_ms) return std::nullopt;
    return it->second.value;
}

std::optional<Value> Kv::raw(std::string_view key) const {
    auto it = data_.find(std::string(key));
    if (it == data_.end()) return std::nullopt;
    return it->second;
}

bool Kv::erase_if_expired(std::string_view key, i64 now_ms) {
    auto it = data_.find(std::string(key));
    if (it == data_.end()) return false;
    if (it->second.expire_at_ms == -1 || it->second.expire_at_ms > now_ms) return false;
    data_.erase(it);
    return true;
}

std::vector<std::string> Kv::expired_keys(i64 now_ms) const {
    std::vector<std::string> out;
    for (const auto& [key, value] : data_) {
        if (value.expire_at_ms != -1 && value.expire_at_ms <= now_ms) out.push_back(key);
    }
    return out;
}

std::size_t Kv::size(i64 now_ms) const {
    std::size_t count = 0;
    for (const auto& [key, value] : data_) {
        (void)key;
        if (value.expire_at_ms == -1 || value.expire_at_ms > now_ms) ++count;
    }
    return count;
}

std::size_t Kv::raw_size() const { return data_.size(); }

std::string Kv::snapshot() const {
    std::string out;
    put_u32(out, static_cast<u32>(data_.size()));
    for (const auto& [key, value] : data_) {
        put_string(out, key);
        put_string(out, value.value);
        put_i64(out, value.expire_at_ms);
    }
    return out;
}

void Kv::load(std::string_view snapshot) {
    std::size_t pos = 0;
    const u32 count = read_u32(snapshot, pos);
    std::map<std::string, Value> loaded;
    for (u32 i = 0; i < count; ++i) {
        std::string key = read_string(snapshot, pos);
        std::string value = read_string(snapshot, pos);
        const i64 expire_at = read_i64(snapshot, pos);
        loaded[std::move(key)] = Value{std::move(value), expire_at};
    }
    if (pos != snapshot.size()) throw std::runtime_error("KV snapshot: trailing bytes");
    data_ = std::move(loaded);
}

void Kv::apply(const Command& command, i64 now_ms) {
    if (command.args.empty()) throw std::runtime_error("KV: empty command");
    const std::string verb = upper(command.args[0]);

    if (verb == "SET") {
        if (command.args.size() != 3 && command.args.size() != 5) {
            throw std::runtime_error("KV: SET expects 3 or 5 arguments");
        }
        i64 expire_at = -1;
        if (command.args.size() == 5) {
            const std::string option = upper(command.args[3]);
            const i64 value = std::stoll(command.args[4]);
            if (option == "PXAT") {
                expire_at = value;
            } else if (option == "EXAT") {
                expire_at = value * 1000;
            } else {
                throw std::runtime_error("KV: unsupported SET option " + option);
            }
        }
        if (expire_at != -1 && expire_at <= now_ms) {
            data_.erase(command.args[1]);
        } else {
            set(command.args[1], command.args[2], expire_at);
        }
        return;
    }

    if (verb == "DEL" || verb == "UNLINK") {
        for (std::size_t i = 1; i < command.args.size(); ++i) del(command.args[i]);
        return;
    }

    if (verb == "PEXPIREAT") {
        if (command.args.size() != 3) throw std::runtime_error("KV: PEXPIREAT expects 2 arguments");
        const i64 deadline = std::stoll(command.args[2]);
        if (deadline <= now_ms) {
            del(command.args[1]);
        } else {
            expire_at(command.args[1], deadline);
        }
        return;
    }

    if (verb == "PERSIST") {
        if (command.args.size() != 2) throw std::runtime_error("KV: PERSIST expects 1 argument");
        (void)persist(command.args[1]);
        return;
    }

    throw std::runtime_error("KV: unsupported command " + verb);
}

std::vector<std::pair<std::string, Value>> Kv::entries() const {
    std::vector<std::pair<std::string, Value>> out;
    out.reserve(data_.size());
    for (const auto& item : data_) out.push_back(item);
    return out;
}

}  // namespace archdesign
