#include "resp.hpp"

#include <stdexcept>

namespace archdesign {

namespace {

bool read_line(std::string_view data, std::size_t& pos, std::string& line) {
    const std::size_t start = pos;
    while (pos + 1 < data.size()) {
        if (data[pos] == '\r' && data[pos + 1] == '\n') {
            line.assign(data.substr(start, pos - start));
            pos += 2;
            return true;
        }
        ++pos;
    }
    return false;
}

}  // namespace

std::string resp_encode(const Command& command) {
    std::string out = "*" + std::to_string(command.args.size()) + "\r\n";
    for (const std::string& arg : command.args) {
        out += "$" + std::to_string(arg.size()) + "\r\n";
        out += arg;
        out += "\r\n";
    }
    return out;
}

RespParseResult resp_parse(std::string_view data) {
    RespParseResult result;
    std::size_t pos = 0;
    std::size_t last_complete = 0;

    auto truncated = [&]() {
        result.truncated = true;
        result.consumed = last_complete;
        return result;
    };

    while (pos < data.size()) {
        // Redis 7 的 AOF 可以带 "#TS:<unix>\r\n" 时间戳注释。
        if (data[pos] == '#') {
            std::string ignored;
            if (!read_line(data, pos, ignored)) return truncated();
            last_complete = pos;
            continue;
        }
        if (data[pos] != '*') {
            throw std::runtime_error(
                "AOF: expected '*', got byte " +
                std::to_string(static_cast<unsigned>(static_cast<u8>(data[pos]))) +
                " at offset " + std::to_string(pos));
        }
        ++pos;

        std::string count_line;
        if (!read_line(data, pos, count_line)) return truncated();
        const i64 count = parse_i64(count_line, "RESP array length");
        if (count < 0 || count > 1'000'000) throw std::runtime_error("AOF: invalid array length");

        Command command;
        command.args.reserve(static_cast<std::size_t>(count));
        for (i64 i = 0; i < count; ++i) {
            if (pos >= data.size()) return truncated();
            if (data[pos] != '$') {
                throw std::runtime_error("AOF: expected '$' at offset " + std::to_string(pos));
            }
            ++pos;
            std::string length_line;
            if (!read_line(data, pos, length_line)) return truncated();
            const i64 length = parse_i64(length_line, "RESP bulk length");
            if (length < 0) throw std::runtime_error("AOF: negative bulk length");
            if (static_cast<u64>(length) > data.size() - pos) return truncated();
            command.args.emplace_back(data.substr(pos, static_cast<std::size_t>(length)));
            pos += static_cast<std::size_t>(length);
            if (pos + 1 >= data.size()) return truncated();
            if (data[pos] != '\r' || data[pos + 1] != '\n') {
                throw std::runtime_error("AOF: bulk string not terminated by CRLF");
            }
            pos += 2;
        }
        result.commands.push_back(std::move(command));
        last_complete = pos;
    }

    result.consumed = last_complete;
    return result;
}

}  // namespace archdesign
