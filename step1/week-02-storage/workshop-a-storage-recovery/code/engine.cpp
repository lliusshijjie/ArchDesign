// engine.cpp

#include "engine.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <istream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace arch {

namespace {

constexpr std::size_t kMaxKeyBytes = 1u << 20;
constexpr std::size_t kMaxValueBytes = 1u << 24;
// magic(4) + type(1) + key_len(4) + value_len(4) + crc(4)
constexpr std::size_t kWalHeaderBytes = 17;
// magic(4) + count(4)
constexpr std::size_t kSstHeaderBytes = 8;

} // namespace

Engine::Engine(fs::path directory, Options options, CrashInjection* crash)
    : disk_(directory), options_(std::move(options)), crash_(crash) {
    if (options_.memtable_limit == 0) {
        throw std::invalid_argument("memtable_limit must be > 0");
    }
    if (options_.compaction_trigger < 2) {
        throw std::invalid_argument("compaction_trigger must be >= 2");
    }
    if (options_.group_commit == 0) {
        throw std::invalid_argument("group_commit must be >= 1");
    }

    const auto start = std::chrono::steady_clock::now();
    std::error_code error;
    fs::create_directories(directory, error);
    if (error) {
        throw std::runtime_error("cannot create directory: " + error.message());
    }

    recover();
    stats_.open_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count();
}

// ---------------------------------------------------------------- 恢复

void Engine::recover() {
    // 0. 上次崩溃留下的临时文件（SST.tmp / MANIFEST.tmp）一律不可信。
    for (const auto& name : disk_.list()) {
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) {
            disk_.remove(name);
            ++stats_.temp_files_removed;
        }
    }

    // 1. manifest 是唯一真相来源；没有 manifest 就等于没有 SST。
    load_manifest();

    // 2. 只加载 manifest 引用的 SST，并逐个校验。
    for (const auto& ref : manifest_) {
        load_sst(ref.id);
    }

    // 3. 没有被引用的 SST 是崩溃留下的孤儿：文件在，但它属于“未提交的 flush/compaction”。
    remove_extra_files();

    // 4. WAL 重放。坏尾在这里被截断，后面的追加才不会继续污染。
    replay_wal();
}

void Engine::load_manifest() {
    if (!disk_.exists(std::string(manifest_file))) {
        return;
    }
    const auto blob = disk_.read_file(std::string(manifest_file));
    if (blob.empty()) {
        // 从未成功提交过：等价于“还没有任何 SST”。
        return;
    }

    const auto crc_pos = blob.rfind("CRC ");
    if (crc_pos == std::string::npos || crc_pos == 0) {
        throw Corruption("manifest: missing checksum line");
    }
    std::uint32_t expected = 0;
    try {
        expected = static_cast<std::uint32_t>(std::stoul(blob.substr(crc_pos + 4), nullptr, 16));
    } catch (const std::exception&) {
        throw Corruption("manifest: bad checksum line");
    }
    const std::string_view prefix(blob.data(), crc_pos);
    if (crc32(prefix) != expected) {
        throw Corruption("manifest: checksum mismatch");
    }

    std::istringstream lines{std::string(prefix)};
    std::string line;
    if (!std::getline(lines, line) || line != std::string(kManifestMagic)) {
        throw Corruption("manifest: bad header");
    }
    while (std::getline(lines, line)) {
        if (line.empty()) {
            continue;
        }
        std::istringstream fields{line};
        std::string tag;
        SSTRef ref;
        fields >> tag >> ref.id >> ref.entries;
        if (tag != "sst" || !fields) {
            throw Corruption("manifest: bad entry: " + line);
        }
        manifest_.push_back(ref);
        next_sst_id_ = std::max(next_sst_id_, ref.id + 1);
    }
}

void Engine::load_sst(std::uint64_t id) {
    const auto name = sst_name(id);
    if (!disk_.exists(name)) {
        throw Corruption("manifest references missing sst: " + name);
    }
    const auto blob = disk_.read_file(name);
    if (blob.size() < kSstHeaderBytes + 4) {
        throw Corruption("sst too small: " + name);
    }
    const std::string_view view(blob);
    if (read_u32(view, 0) != kSstMagic) {
        throw Corruption("sst magic mismatch: " + name);
    }
    const auto count = read_u32(view, 4);
    const auto payload_size = blob.size() - 4;
    if (crc32(view.substr(0, payload_size)) != read_u32(view, payload_size)) {
        throw Corruption("sst checksum mismatch: " + name);
    }

    SSTable table;
    table.id = id;
    table.entries.reserve(count);
    std::size_t offset = kSstHeaderBytes;
    for (std::uint32_t index = 0; index < count; ++index) {
        if (offset + 9 > payload_size) {
            throw Corruption("sst truncated entry header: " + name);
        }
        const auto key_size = read_u32(view, offset);
        const auto value_size = read_u32(view, offset + 4);
        const bool tombstone = read_u8(view, offset + 8) != 0;
        offset += 9;
        if (key_size > kMaxKeyBytes || value_size > kMaxValueBytes ||
            offset + key_size + value_size > payload_size) {
            throw Corruption("sst entry out of range: " + name);
        }
        KVEntry entry;
        entry.key.assign(view.data() + offset, key_size);
        offset += key_size;
        entry.value.assign(view.data() + offset, value_size);
        offset += value_size;
        entry.tombstone = tombstone;
        table.entries.push_back(std::move(entry));
    }
    if (offset != payload_size) {
        throw Corruption("sst trailing bytes: " + name);
    }
    const bool sorted = std::is_sorted(
        table.entries.begin(), table.entries.end(),
        [](const KVEntry& left, const KVEntry& right) { return left.key < right.key; });
    if (!sorted) {
        throw Corruption("sst keys are not sorted: " + name);
    }
    ssts_.push_back(std::move(table));
}

void Engine::remove_extra_files() {
    for (const auto& name : disk_.list()) {
        if (!name.starts_with("sst-") || !name.ends_with(".dat")) {
            continue;
        }
        std::uint64_t id = 0;
        try {
            id = std::stoull(name.substr(4, name.size() - 8));
        } catch (const std::exception&) {
            continue;
        }
        const bool referenced =
            std::any_of(manifest_.begin(), manifest_.end(),
                        [id](const SSTRef& ref) { return ref.id == id; });
        if (!referenced) {
            disk_.remove(name);
            ++stats_.orphan_ssts_removed;
        }
    }
}

void Engine::replay_wal() {
    const auto name = std::string(wal_file);
    if (!disk_.exists(name)) {
        return;
    }
    const auto blob = disk_.read_file(name);
    const std::string_view view(blob);
    std::size_t offset = 0;
    std::uint64_t valid_bytes = 0;

    while (offset + kWalHeaderBytes <= view.size()) {
        if (read_u32(view, offset) != kWalMagic) {
            break;
        }
        const bool tombstone = read_u8(view, offset + 4) != 0;
        const auto key_size = read_u32(view, offset + 5);
        const auto value_size = read_u32(view, offset + 9);
        const auto checksum = read_u32(view, offset + 13);
        if (key_size > kMaxKeyBytes || value_size > kMaxValueBytes) {
            break;
        }
        const auto record_size =
            kWalHeaderBytes + static_cast<std::size_t>(key_size) + value_size;
        if (offset + record_size > view.size()) {
            break;  // 尾部只写了一半
        }
        const auto key = view.substr(offset + kWalHeaderBytes, key_size);
        const auto value = view.substr(offset + kWalHeaderBytes + key_size, value_size);

        std::string covered(view.substr(offset + 4, 9));  // type + klen + vlen
        covered.append(key.data(), key.size());
        covered.append(value.data(), value.size());
        if (crc32(covered) != checksum) {
            break;  // 校验失败：从这里开始不可信
        }

        apply_memtable(std::string(key), tombstone ? std::nullopt
                                                   : std::optional<std::string>(std::string(value)));
        ++stats_.wal_records_replayed;
        offset += record_size;
        valid_bytes = offset;
    }

    if (valid_bytes < view.size()) {
        stats_.wal_truncated_bytes = view.size() - valid_bytes;
        disk_.truncate(name, valid_bytes, true);
    }
}

// ---------------------------------------------------------------- 编码

std::string Engine::sst_name(std::uint64_t id) {
    char name[32];
    std::snprintf(name, sizeof(name), "sst-%06llu.dat",
                  static_cast<unsigned long long>(id));
    return name;
}

std::string Engine::encode_wal_record(bool tombstone, std::string_view key,
                                      std::string_view value) const {
    std::string out;
    out.reserve(kWalHeaderBytes + key.size() + value.size());
    put_u32(out, kWalMagic);
    put_u8(out, tombstone ? 1u : 0u);
    put_u32(out, static_cast<std::uint32_t>(key.size()));
    put_u32(out, static_cast<std::uint32_t>(value.size()));

    std::string covered(out.substr(4));  // type + key_len + value_len
    covered.append(key.data(), key.size());
    covered.append(value.data(), value.size());
    put_u32(out, crc32(covered));

    out.append(key.data(), key.size());
    out.append(value.data(), value.size());
    return out;
}

std::string Engine::serialize_sst(const std::vector<KVEntry>& entries) const {
    std::string blob;
    put_u32(blob, kSstMagic);
    put_u32(blob, static_cast<std::uint32_t>(entries.size()));
    for (const auto& entry : entries) {
        put_u32(blob, static_cast<std::uint32_t>(entry.key.size()));
        put_u32(blob, static_cast<std::uint32_t>(entry.value.size()));
        put_u8(blob, entry.tombstone ? 1u : 0u);
        blob.append(entry.key);
        blob.append(entry.value);
    }
    put_u32(blob, crc32(blob));
    return blob;
}

std::string Engine::serialize_manifest() const {
    std::string blob{kManifestMagic};
    blob.push_back('\n');
    for (const auto& ref : manifest_) {
        blob += "sst " + std::to_string(ref.id) + " " + std::to_string(ref.entries);
        blob.push_back('\n');
    }
    char line[32];
    std::snprintf(line, sizeof(line), "CRC %08x\n", crc32(blob));
    blob += line;
    return blob;
}

// ---------------------------------------------------------------- 写路径

void Engine::append_wal(bool tombstone, std::string_view key, std::string_view value) {
    hit(crash_, kWalBeforeAppend);
    const auto record = encode_wal_record(tombstone, key, value);
    disk_.append(std::string(wal_file), record, false);
    stats_.wal_bytes += record.size();
    hit(crash_, kWalAfterAppend);

    if (options_.fsync_wal) {
        ++wal_records_since_sync_;
        if (wal_records_since_sync_ >= options_.group_commit) {
            disk_.sync_file(std::string(wal_file));
            wal_records_since_sync_ = 0;
        }
    }
    hit(crash_, kWalAfterDurability);
}

void Engine::apply_memtable(const std::string& key, std::optional<std::string> value) {
    const auto it = memtable_.find(key);
    if (it != memtable_.end()) {
        memtable_bytes_ -= entry_cost(it->first, it->second);
    }
    const auto [position, inserted] = memtable_.insert_or_assign(key, std::move(value));
    (void)inserted;
    memtable_bytes_ += entry_cost(position->first, position->second);
}

std::size_t Engine::entry_cost(const std::string& key,
                               const std::optional<std::string>& value) const {
    return key.size() + (value.has_value() ? value->size() : 0) + 32;
}

void Engine::put(const std::string& key, std::string_view value) {
    ensure_open();
    append_wal(false, key, value);
    apply_memtable(key, std::string(value));
    hit(crash_, kWalAfterMemtable);
    ++stats_.puts;
    stats_.user_bytes += key.size() + value.size();
    if (memtable_bytes_ >= options_.memtable_limit) {
        flush();
    }
}

void Engine::erase(const std::string& key) {
    ensure_open();
    append_wal(true, key, std::string_view{});
    apply_memtable(key, std::nullopt);
    hit(crash_, kWalAfterMemtable);
    ++stats_.erases;
    stats_.user_bytes += key.size();
    if (memtable_bytes_ >= options_.memtable_limit) {
        flush();
    }
}

void Engine::flush() {
    ensure_open();
    if (memtable_.empty()) {
        return;
    }

    std::vector<KVEntry> entries;
    entries.reserve(memtable_.size());
    for (const auto& [key, value] : memtable_) {
        entries.push_back(KVEntry{key, value.value_or(""), !value.has_value()});
    }

    const auto id = next_sst_id_++;
    const auto name = sst_name(id);
    const auto payload = serialize_sst(entries);

    // 1. 先写临时文件并落盘：此刻它还是“没人认识的垃圾”，丢掉也无所谓。
    disk_.write_file(name + ".tmp", payload, options_.fsync_sst);
    stats_.sst_bytes += payload.size();
    hit(crash_, kFlushSstBeforeRename);

    // 2. rename 是发布动作：要么看不到，要么完整可见。
    disk_.atomic_replace(name + ".tmp", name);
    hit(crash_, kFlushSstAfterRename);

    // 3. 提交 manifest：这一步之后，新的 SST 才算“官方数据”。
    manifest_.push_back(SSTRef{id, entries.size()});
    ssts_.push_back(SSTable{id, std::move(entries)});
    commit_manifest(kFlushManifestBeforeCommit, kFlushManifestAfterCommit);

    // 4. manifest 落盘之后，才可以丢弃内存里的副本和 WAL。
    memtable_.clear();
    memtable_bytes_ = 0;
    hit(crash_, kFlushWalBeforeTruncate);
    disk_.truncate(std::string(wal_file), 0, true);
    wal_records_since_sync_ = 0;
    hit(crash_, kFlushWalAfterTruncate);
    ++stats_.flushes;

    if (manifest_.size() >= options_.compaction_trigger) {
        compact();
    }
}

void Engine::commit_manifest(std::string_view before_commit, std::string_view after_commit) {
    const auto blob = serialize_manifest();
    disk_.write_file(std::string(manifest_temp), blob, options_.fsync_manifest);
    stats_.manifest_bytes += blob.size();
    hit(crash_, before_commit);
    disk_.atomic_replace(std::string(manifest_temp), std::string(manifest_file));
    hit(crash_, after_commit);
}

void Engine::compact() {
    if (ssts_.size() < 2) {
        return;
    }

    // 全量合并：从新到旧，每个 key 保留第一次出现的版本。
    // 合并的是“全部” SST，最老的一层也在里面，所以 tombstone 可以安全丢弃。
    std::map<std::string, KVEntry> merged;
    for (auto it = ssts_.rbegin(); it != ssts_.rend(); ++it) {
        for (const auto& entry : it->entries) {
            if (merged.find(entry.key) != merged.end()) {
                continue;
            }
            merged.emplace(entry.key, entry);
        }
    }

    std::vector<KVEntry> entries;
    entries.reserve(merged.size());
    for (auto& [key, entry] : merged) {
        (void)key;
        if (entry.tombstone) {
            continue;
        }
        entries.push_back(entry);
    }

    const auto id = next_sst_id_++;
    const auto name = sst_name(id);
    const auto payload = serialize_sst(entries);
    disk_.write_file(name + ".tmp", payload, options_.fsync_sst);
    stats_.sst_bytes += payload.size();
    hit(crash_, kCompactSstBeforeRename);
    disk_.atomic_replace(name + ".tmp", name);
    hit(crash_, kCompactSstAfterRename);

    const auto old_manifest = manifest_;
    manifest_.clear();
    manifest_.push_back(SSTRef{id, entries.size()});
    ssts_.clear();
    ssts_.push_back(SSTable{id, std::move(entries)});
    commit_manifest(kCompactManifestBeforeCommit, kCompactManifestAfterCommit);

    // 新 manifest 已经落盘；旧 SST 现在只是垃圾。
    for (const auto& ref : old_manifest) {
        disk_.remove(sst_name(ref.id));
    }
    hit(crash_, kCompactOldDeleted);
    ++stats_.compactions;
}

void Engine::close() {
    if (closed_) {
        return;
    }
    flush();
    if (wal_records_since_sync_ > 0) {
        disk_.sync_file(std::string(wal_file));
        wal_records_since_sync_ = 0;
    }
    closed_ = true;
}

void Engine::power_loss() {
    disk_.power_loss();
}

// ---------------------------------------------------------------- 读路径

std::optional<std::string> Engine::lookup_in_ssts(const std::string& key) const {
    for (auto it = ssts_.rbegin(); it != ssts_.rend(); ++it) {
        const auto position =
            std::lower_bound(it->entries.begin(), it->entries.end(), key,
                             [](const KVEntry& entry, const std::string& value) {
                                 return entry.key < value;
                             });
        if (position != it->entries.end() && position->key == key) {
            return position->tombstone ? std::nullopt
                                       : std::optional<std::string>(position->value);
        }
    }
    return std::nullopt;
}

std::optional<std::string> Engine::get(const std::string& key) const {
    const auto memory = memtable_.find(key);
    if (memory != memtable_.end()) {
        return memory->second;
    }
    return lookup_in_ssts(key);
}

Snapshot Engine::dump() const {
    Snapshot result;
    for (auto it = ssts_.rbegin(); it != ssts_.rend(); ++it) {
        for (const auto& entry : it->entries) {
            if (result.find(entry.key) != result.end()) {
                continue;
            }
            result.emplace(entry.key, entry.tombstone
                                          ? std::nullopt
                                          : std::optional<std::string>(entry.value));
        }
    }
    for (const auto& [key, value] : memtable_) {
        result[key] = value;
    }
    return result;
}

// ---------------------------------------------------------------- 杂项

void Engine::hit(CrashInjection* crash, std::string_view point) {
    if (crash != nullptr) {
        crash->hit(point);
    }
}

void Engine::ensure_open() const {
    if (closed_) {
        throw std::logic_error("engine is closed");
    }
}

Engine::Stats Engine::stats() const {
    Stats result = stats_;
    result.fsyncs = disk_.fsyncs();
    result.ssts = ssts_.size();
    return result;
}

} // namespace arch
