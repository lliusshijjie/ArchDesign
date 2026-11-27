#include "backlog.hpp"

#include <algorithm>
#include <stdexcept>

namespace archdesign {

Backlog::Backlog(std::size_t capacity) : ring_(std::max<std::size_t>(1, capacity)) {}

void Backlog::append(std::string_view bytes) {
    const std::size_t cap = ring_.size();
    for (char byte : bytes) {
        if (size_ == cap) {
            // 覆盖最旧的一个字节，逻辑起点随之前进。
            start_index_ = (start_index_ + 1) % cap;
            --size_;
            ++start_;
        }
        ring_[(start_index_ + size_) % cap] = byte;
        ++size_;
    }
}

void Backlog::seed(Offset start, std::string_view bytes) {
    std::fill(ring_.begin(), ring_.end(), '\0');
    start_index_ = 0;
    size_ = 0;
    start_ = start;
    append(bytes);
}

bool Backlog::contains(Offset offset) const {
    return offset >= start_ && offset <= end();
}

std::string Backlog::read_from(Offset offset) const {
    if (!contains(offset)) {
        throw std::runtime_error("Backlog: offset " + std::to_string(offset) +
                                 " outside [" + std::to_string(start_) + ", " +
                                 std::to_string(end()) + "]");
    }
    const std::size_t skip = static_cast<std::size_t>(offset - start_);
    const std::size_t len = size_ - skip;
    std::string out;
    out.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        out.push_back(ring_[(start_index_ + skip + i) % ring_.size()]);
    }
    return out;
}

Offset Backlog::start() const { return start_; }

Offset Backlog::end() const { return start_ + static_cast<Offset>(size_); }

std::size_t Backlog::size() const { return size_; }

std::size_t Backlog::capacity() const { return ring_.size(); }

}  // namespace archdesign
