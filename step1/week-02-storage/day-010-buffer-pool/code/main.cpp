// main.cpp
//
// Day 10 的 Buffer Pool 测试：命中、淘汰、pin、young/old、脏页回写。

#include "buffer_pool.hpp"

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using archdesign::BufferPool;
using archdesign::BufferPoolFull;
using archdesign::MemoryPageStore;
using archdesign::PageData;
using archdesign::PageId;

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_load_modify_evict_reload() {
    MemoryPageStore store;
    PageData initial{};
    initial[0] = std::byte{0x11};
    store.write(1, initial);

    BufferPool pool(store, 2);

    auto* page = pool.fetch(1);
    check(page->data[0] == std::byte{0x11}, "initial page content");
    page->data[5] = std::byte{0x22};
    pool.unpin(1, true);

    pool.fetch(2);
    pool.unpin(2);

    pool.fetch(3);
    pool.unpin(3);

    check(!pool.contains(1), "page 1 should be evicted");
    check(pool.contains(2), "page 2 should remain resident");

    auto* reloaded = pool.fetch(1);
    check(reloaded->data[5] == std::byte{0x22},
          "dirty page should be written before eviction");
    pool.unpin(1);

    const auto stats = pool.stats();
    check(stats.misses >= 4, "all loads should be misses");
    check(stats.evictions >= 1, "at least one eviction");
    check(stats.flushes >= 1, "dirty eviction should flush");

    pool.flush_all();
}

void test_hit_promotes_and_evicts_old() {
    MemoryPageStore store;
    BufferPool pool(store, 2);

    pool.fetch(1);
    pool.unpin(1);
    pool.fetch(2);
    pool.unpin(2);

    // 命中 page 1：从 old 提升到 young。
    pool.fetch(1);
    pool.unpin(1);

    pool.fetch(3);
    pool.unpin(3);

    check(pool.contains(1), "hot page should survive the scan");
    check(!pool.contains(2), "old page should be evicted");

    const auto stats = pool.stats();
    check(stats.hits >= 1, "page 1 access should be a hit");
    check(stats.evictions >= 1, "one eviction expected");
}

void test_pin_blocks_eviction() {
    MemoryPageStore store;
    BufferPool pool(store, 1);

    pool.fetch(7);

    bool full = false;
    try {
        pool.fetch(8);
    } catch (const BufferPoolFull&) {
        full = true;
    }
    check(full, "pinned page must not be evicted");

    pool.unpin(7);
    auto* page = pool.fetch(8);
    check(page->id == 8, "fetch should succeed after unpin");
    pool.unpin(8);
}

void test_scan_resistance() {
    MemoryPageStore store;
    BufferPool pool(store, 4);

    // 把 page 1 变成热页（第二次访问提升到 young）。
    pool.fetch(1);
    pool.unpin(1);
    pool.fetch(1);
    pool.unpin(1);

    // 顺序扫描 2..6，全部只访问一次。
    for (PageId id = 2; id <= 6; ++id) {
        pool.fetch(id);
        pool.unpin(id);
    }

    check(pool.contains(1), "midpoint LRU should protect the hot page");
    check(!pool.contains(2), "scan pages should be evicted from old");

    const auto stats = pool.stats();
    check(stats.evictions >= 2, "scan should cause evictions");
}

void test_flush_all() {
    MemoryPageStore store;
    BufferPool pool(store, 2);

    auto* page1 = pool.fetch(1);
    page1->data[0] = std::byte{0xAB};
    pool.unpin(1, true);

    auto* page2 = pool.fetch(2);
    page2->data[0] = std::byte{0xCD};
    pool.unpin(2, true);

    pool.flush_all();

    check(store.get(1)[0] == std::byte{0xAB}, "page 1 should be flushed");
    check(store.get(2)[0] == std::byte{0xCD}, "page 2 should be flushed");
    check(!pool.dirty(1) && !pool.dirty(2), "flush should clear dirty flags");
    check(pool.stats().flushes == 2, "two dirty pages should be flushed");
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests = {
        {"load_modify_evict_reload", test_load_modify_evict_reload},
        {"hit_promotes_and_evicts_old", test_hit_promotes_and_evicts_old},
        {"pin_blocks_eviction", test_pin_blocks_eviction},
        {"scan_resistance", test_scan_resistance},
        {"flush_all", test_flush_all},
    };

    int failed = 0;
    for (const auto& [name, run] : tests) {
        try {
            run();
            std::cout << "[  OK  ] " << name << '\n' << std::flush;
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "[FAILED] " << name << ": " << error.what() << '\n'
                      << std::flush;
        } catch (...) {
            ++failed;
            std::cerr << "[FAILED] " << name << ": unknown exception\n"
                      << std::flush;
        }
    }

    if (failed != 0) {
        std::cerr << failed << " test(s) failed\n";
        return 1;
    }
    std::cout << "\nAll " << tests.size() << " tests passed.\n";
    return 0;
}
