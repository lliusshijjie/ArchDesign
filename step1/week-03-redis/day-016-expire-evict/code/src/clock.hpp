// clock.hpp
//
// 生产里是 mstime()/steady_clock，测试里是手动推进的变量。
// 把时间注入进来，过期/淘汰的测试才能不 sleep、不 flaky。

#pragma once

#include <chrono>
#include <functional>
#include <utility>

#include "common.hpp"

namespace archdesign {

class Clock {
public:
    static Clock system();
    static Clock manual(const i64* now_ms);

    i64 now() const;

private:
    explicit Clock(std::function<i64()> fn) : fn_(std::move(fn)) {}

    std::function<i64()> fn_;
};

}  // namespace archdesign
