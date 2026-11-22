// main.cpp
//
// Day 17 的命令行入口。核心逻辑在 src/，测试在 tests/，演示/基准各自一个文件。

#include <iostream>
#include <string>

#include "tests/test_support.hpp"

namespace {

int run_case(const std::string& name) {
    for (const auto& test : archdesign::test::test_cases()) {
        if (name != test.name) continue;
        try {
            test.fn();
            std::cout << "[PASS] " << name << '\n';
            return 0;
        } catch (const std::exception& ex) {
            std::cout << "[FAIL] " << name << ": " << ex.what() << '\n';
            return 1;
        }
    }
    std::cout << "[FAIL] unknown case: " << name << '\n';
    return 1;
}

int run_all() {
    int failures = 0;
    for (const auto& test : archdesign::test::test_cases()) {
        try {
            test.fn();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& ex) {
            std::cout << "[FAIL] " << test.name << ": " << ex.what() << '\n';
            ++failures;
        }
    }
    std::cout << (failures == 0 ? "all tests passed"
                                : std::to_string(failures) + " tests failed")
              << '\n';
    return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::ios::sync_with_stdio(false);
    if (argc > 1) {
        const std::string arg = argv[1];
        if (arg == "--demo") {
            archdesign::test::run_demo();
            return 0;
        }
        if (arg == "--bench") {
            archdesign::test::run_bench();
            return 0;
        }
        if (arg == "--list") {
            for (const auto& test : archdesign::test::test_cases()) {
                std::cout << test.name << '\n';
            }
            return 0;
        }
        if (arg == "--case" && argc > 2) return run_case(argv[2]);
        std::cout << "usage: " << argv[0] << " [--demo|--bench|--list|--case <name>]\n";
        return 2;
    }
    return run_all();
}
