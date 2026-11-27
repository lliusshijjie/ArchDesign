// main.cpp
//
// Day 16 的命令行入口。核心逻辑在 src/，测试在 tests/，演示/基准各自一个文件。

#include <iostream>
#include <string>

#include "tests/test_support.hpp"

namespace {

int run_named(const std::string& name) {
    for (const auto& test : archdesign::test::test_cases()) {
        if (name != test.name) continue;
        try {
            test.fn();
            std::cout << "[PASS] " << name << "\n";
            return 0;
        } catch (const std::exception& e) {
            std::cerr << "[FAIL] " << name << ": " << e.what() << "\n";
            return 1;
        }
    }
    std::cerr << "unknown case: " << name << "\n";
    return 2;
}

int run_all() {
    int failed = 0;
    for (const auto& test : archdesign::test::test_cases()) {
        try {
            test.fn();
            std::cout << "[PASS] " << test.name << "\n";
        } catch (const std::exception& e) {
            ++failed;
            std::cerr << "[FAIL] " << test.name << ": " << e.what() << "\n";
        }
    }
    std::cout << "\n" << (archdesign::test::test_cases().size() - static_cast<std::size_t>(failed))
              << "/" << archdesign::test::test_cases().size() << " cases passed\n";
    return failed == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        const std::string arg = argv[1];
        if (arg == "--case" && argc > 2) return run_named(argv[2]);
        if (arg == "--demo") {
            archdesign::test::run_demo();
            return 0;
        }
        if (arg == "--bench") {
            archdesign::test::run_bench();
            return 0;
        }
        std::cerr << "usage: " << argv[0] << " [--case NAME | --demo | --bench]\n";
        return 2;
    }
    return run_all();
}
