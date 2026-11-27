#include "page_store.hpp"

#include <algorithm>
#include <utility>

namespace archdesign {

namespace {
constexpr u64 kFnvOffset = 1469598103934665603ULL;
constexpr u64 kFnvPrime = 1099511628211ULL;
}  // namespace

std::size_t page_bytes(const Page& page) {
    std::size_t total = 0;
    for (const auto& [key, entry] : page.entries) {
        // 近似账：key/value 字节 + map 节点/对象头的固定开销。
        total += key.size() + entry.value.size() + 96;
    }
    return total;
}

Snapshot::Snapshot() = default;

Snapshot::Snapshot(std::vector<std::shared_ptr<const Page>> pages, u64 version,
                   std::size_t page_count)
    : pages_(std::move(pages)), version_(version), page_count_(page_count) {}

std::size_t Snapshot::page_count() const { return page_count_; }

u64 Snapshot::version() const { return version_; }

std::size_t Snapshot::key_count() const {
    std::size_t total = 0;
    for (const auto& page : pages_) total += page->entries.size();
    return total;
}

std::size_t Snapshot::expires_count() const {
    std::size_t total = 0;
    for (const auto& page : pages_) {
        for (const auto& [key, entry] : page->entries) {
            (void)key;
            if (entry.expire_at_ms != -1) ++total;
        }
    }
    return total;
}

PageStore::PageStore(std::size_t page_count)
    : page_count_(page_count == 0 ? 1 : page_count) {
    pages_.reserve(page_count_);
    for (std::size_t i = 0; i < page_count_; ++i) pages_.push_back(std::make_shared<Page>());
}

std::optional<Entry> PageStore::get(std::string_view key) const {
    const Page& page = *pages_[page_of(key)];
    auto it = page.entries.find(std::string(key));
    if (it == page.entries.end()) return std::nullopt;
    return it->second;
}

std::optional<Entry> PageStore::get_live(std::string_view key, i64 now_ms) const {
    auto entry = get(key);
    if (!entry) return std::nullopt;
    if (entry->expire_at_ms != -1 && entry->expire_at_ms <= now_ms) return std::nullopt;
    return entry;
}

void PageStore::set(std::string key, std::string value, i64 expire_at_ms) {
    Page& page = writable_page(page_of(key));
    auto [it, inserted] = page.entries.try_emplace(std::move(key), Entry{});
    (void)inserted;
    it->second.value = std::move(value);
    it->second.expire_at_ms = expire_at_ms;
    ++stats_.writes;
}

bool PageStore::del(std::string_view key) {
    Page& page = writable_page(page_of(key));
    return page.entries.erase(std::string(key)) != 0;
}

bool PageStore::expire_at(std::string_view key, i64 when_ms) {
    Page& page = writable_page(page_of(key));
    auto it = page.entries.find(std::string(key));
    if (it == page.entries.end()) return false;
    it->second.expire_at_ms = when_ms;
    ++stats_.writes;
    return true;
}

bool PageStore::persist(std::string_view key) {
    Page& page = writable_page(page_of(key));
    auto it = page.entries.find(std::string(key));
    if (it == page.entries.end() || it->second.expire_at_ms == -1) return false;
    it->second.expire_at_ms = -1;
    ++stats_.writes;
    return true;
}

Snapshot PageStore::fork() const {
    ++stats_.snapshots;
    std::vector<std::shared_ptr<const Page>> pages;
    pages.reserve(pages_.size());
    for (const auto& page : pages_) pages.push_back(page);
    return Snapshot(std::move(pages), version_++, pages_.size());
}

void PageStore::clear() {
    for (auto& page : pages_) page = std::make_shared<Page>();
    version_ = 0;
}

std::size_t PageStore::key_count() const {
    std::size_t total = 0;
    for (const auto& page : pages_) total += page->entries.size();
    return total;
}

std::size_t PageStore::live_key_count(i64 now_ms) const {
    std::size_t total = 0;
    for (const auto& page : pages_) {
        for (const auto& [key, entry] : page->entries) {
            (void)key;
            if (entry.expire_at_ms == -1 || entry.expire_at_ms > now_ms) ++total;
        }
    }
    return total;
}

std::size_t PageStore::page_count() const { return page_count_; }

std::size_t PageStore::page_of(std::string_view key) const {
    u64 hash = kFnvOffset;
    for (unsigned char c : key) {
        hash ^= c;
        hash *= kFnvPrime;
    }
    return static_cast<std::size_t>(hash % page_count_);
}

const CowStats& PageStore::stats() const { return stats_; }

void PageStore::reset_stats() { stats_ = CowStats{}; }

std::vector<std::string> PageStore::keys() const {
    std::vector<std::string> out;
    out.reserve(key_count());
    for (const auto& page : pages_) {
        for (const auto& [key, entry] : page->entries) {
            (void)entry;
            out.push_back(key);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

Page& PageStore::writable_page(std::size_t index) {
    auto& slot = pages_[index];
    if (slot.use_count() > 1) {
        auto copy = std::make_shared<Page>(*slot);
        stats_.pages_copied++;
        stats_.bytes_copied += page_bytes(*copy);
        slot = std::move(copy);
    }
    return *slot;
}

}  // namespace archdesign
