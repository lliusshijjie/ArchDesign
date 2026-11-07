// main.cpp
//
// Day 11 的 slotted page 测试：有序插入、查找、删除/compact、
// 序列化回读和页分裂。

#include "slotted_page.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using archdesign::SlottedPage;

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_insert_find_scan() {
    SlottedPage page(1);

    check(page.insert("c", "3"), "insert c");
    check(page.insert("a", "1"), "insert a");
    check(page.insert("e", "5"), "insert e");
    check(page.insert("b", "2"), "insert b");
    check(page.insert("d", "4"), "insert d");
    check(!page.insert("c", "duplicate"), "duplicate key should fail");

    check(page.size() == 5, "five records expected");
    check(page.find("a") == std::optional<std::string>("1"), "find a");
    check(page.find("c") == std::optional<std::string>("3"), "find c");
    check(page.find("x") == std::nullopt, "missing key");

    const auto entries = page.entries();
    check(entries.size() == 5, "entries size");
    const std::vector<std::string> expected_keys = {"a", "b", "c", "d", "e"};
    for (std::size_t i = 0; i < expected_keys.size(); ++i) {
        check(entries[i].first == expected_keys[i], "entries should be sorted");
    }
}

void test_delete_and_compact() {
    SlottedPage page(2);

    for (int i = 0; i < 60; ++i) {
        const auto key = "key-" + std::to_string(i);
        check(page.insert(key, std::string(20, 'v')), "insert for delete test");
    }

    for (int i = 0; i < 60; i += 2) {
        const auto key = "key-" + std::to_string(i);
        check(page.erase(key), "erase existing key");
        check(page.find(key) == std::nullopt, "erased key should be gone");
    }

    check(page.size() == 30, "half records should remain");
    check(page.fragmentation() > 0, "deleted records should leave holes");

    page.compact();
    check(page.fragmentation() == 0, "compact should remove fragmentation");
    check(page.size() == 30, "compact should preserve records");
    check(page.find("key-1") == std::optional<std::string>(std::string(20, 'v')),
          "odd key should remain after compact");

    check(page.insert("key-new", "value"), "insert after compact");
    check(page.find("key-new") == std::optional<std::string>("value"),
          "find after compact insert");
}

void test_serialize_roundtrip() {
    SlottedPage page(7);
    page.set_next_page(8);
    page.set_prev_page(6);

    check(page.insert("alpha", "one"), "insert alpha");
    check(page.insert("beta", "two"), "insert beta");
    check(page.insert("gamma", "three"), "insert gamma");

    const auto copy = SlottedPage::from_bytes(page.bytes());
    check(copy.page_id() == 7, "page id should survive");
    check(copy.next_page() == 8, "next link should survive");
    check(copy.prev_page() == 6, "prev link should survive");
    check(copy.size() == 3, "record count should survive");
    check(copy.find("beta") == std::optional<std::string>("two"),
          "value should survive serialization");
    check(copy.entries() == page.entries(), "entries should match");
}

void test_split() {
    SlottedPage page(10);

    int inserted = 0;
    for (int i = 0; i < 10000; ++i) {
        char key[16];
        std::snprintf(key, sizeof(key), "k%06d", i);
        if (!page.insert(key, std::string(24, 'x'))) {
            break;
        }
        ++inserted;
    }
    check(inserted > 2, "page should hold more than two records");
    page.set_next_page(99);

    auto [right, separator] = page.split(11);

    check(page.size() > 0, "left page should not be empty");
    check(right.size() > 0, "right page should not be empty");
    check(page.size() + right.size() == static_cast<std::size_t>(inserted),
          "split should preserve all records");
    check(page.next_page() == 11, "left next should point to right");
    check(right.prev_page() == 10, "right prev should point to left");
    check(right.next_page() == 99, "right should inherit old next");
    check(!separator.empty(), "separator should be the first right key");
    check(right.find(separator).has_value(), "separator should exist in right");

    const auto left_entries = page.entries();
    const auto right_entries = right.entries();
    check(!left_entries.empty() && !right_entries.empty(),
          "both pages should have entries");
    check(left_entries.back().first < right_entries.front().first,
          "split boundary should be ordered");
    check(right_entries.front().first == separator,
          "separator should equal first right key");
}

void test_variable_length_records() {
    SlottedPage page(20);
    check(page.insert("aa", std::string(100, 'a')), "long value");
    check(page.insert("bb", "b"), "short value");
    check(page.insert("cc", std::string(500, 'c')), "longer value");

    check(page.find("aa")->size() == 100, "long value size");
    check(page.find("bb") == std::optional<std::string>("b"), "short value");
    check(page.find("cc")->size() == 500, "longer value size");
    check(page.entries().size() == 3, "variable records count");
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests = {
        {"insert_find_scan", test_insert_find_scan},
        {"delete_and_compact", test_delete_and_compact},
        {"serialize_roundtrip", test_serialize_roundtrip},
        {"split", test_split},
        {"variable_length_records", test_variable_length_records},
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
