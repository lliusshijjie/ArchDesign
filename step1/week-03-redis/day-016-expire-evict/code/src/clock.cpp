#include "clock.hpp"

namespace archdesign {

Clock Clock::system() {
    return Clock([] {
        return static_cast<i64>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    });
}

Clock Clock::manual(const i64* now_ms) {
    return Clock([now_ms] { return *now_ms; });
}

i64 Clock::now() const { return fn_(); }

}  // namespace archdesign
