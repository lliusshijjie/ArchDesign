#include "test_support.hpp"

#include <chrono>
#include <stdexcept>

namespace archdesign::test {

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string key_of(int index, const char* prefix) {
    return std::string(prefix) + std::to_string(index);
}

std::string value_of(std::size_t size) { return std::string(size, 'v'); }

std::size_t elapsed_ms(WallClock::time_point start) {
    return static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(WallClock::now() - start).count());
}

double as_mb(u64 bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

std::string human_bytes(u64 bytes) {
    if (bytes >= 1024ULL * 1024ULL) return std::to_string(bytes / (1024ULL * 1024ULL)) + " MB";
    if (bytes >= 1024ULL) return std::to_string(bytes / 1024ULL) + " KB";
    return std::to_string(bytes) + " B";
}

}  // namespace archdesign::test
