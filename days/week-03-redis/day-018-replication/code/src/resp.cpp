#include "resp.hpp"

#include <charconv>
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

i64 parse_int(std::string_view text, const char* what) {
    i64 value = 0;
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        throw std::runtime_error(std::string("invalid RESP integer for ") + what);
    }
    return value;
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
        if (data[pos] != '*') {
            throw std::runtime_error("RESP: expected '*' at offset " + std::to_string(pos));
        }
        ++pos;

        std::string line;
        if (!read_line(data, pos, line)) return truncated();
        const i64 argc = parse_int(line, "array length");
        if (argc < 0 || argc > 1'000'000) throw std::runtime_error("RESP: invalid array length");

        Command command;
        command.args.reserve(static_cast<std::size_t>(argc));
        for (i64 i = 0; i < argc; ++i) {
            if (pos >= data.size()) return truncated();
            if (data[pos] != '$') {
                throw std::runtime_error("RESP: expected '$' at offset " + std::to_string(pos));
            }
            ++pos;
            std::string len_line;
            if (!read_line(data, pos, len_line)) return truncated();
            const i64 len = parse_int(len_line, "bulk length");
            if (len < 0) throw std::runtime_error("RESP: negative bulk length");
            if (static_cast<u64>(len) > data.size() - pos) return truncated();
            command.args.emplace_back(data.substr(pos, static_cast<std::size_t>(len)));
            pos += static_cast<std::size_t>(len);
            if (pos + 1 >= data.size()) return truncated();
            if (data[pos] != '\r' || data[pos + 1] != '\n') {
                throw std::runtime_error("RESP: bulk not terminated by CRLF");
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
