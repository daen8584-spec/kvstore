// Fuzz target for the request parser and command execution.
//
// The input is split at a position chosen by its first byte and fed in two parts, so the
// fuzzer also explores requests that arrive across reads. Every complete command runs
// against a fresh database. The target checks parser invariants and that each command
// writes exactly one well-formed RESP2 reply; sanitizers catch memory and
// undefined-behaviour errors.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "commands.hpp"
#include "database.hpp"
#include "resp.hpp"

namespace {

// Reads a line ending in CRLF that contains no other CR or LF.
bool read_line(std::string_view out, std::size_t& pos, std::string_view& line) {
    const std::size_t end = out.find("\r\n", pos);
    if (end == std::string_view::npos) return false;
    line = out.substr(pos, end - pos);
    if (line.find_first_of("\r\n") != std::string_view::npos) return false;
    pos = end + 2;
    return true;
}

bool read_length(std::string_view line, long long& value) {
    if (line.empty() || line.size() > 20) return false;
    std::size_t i = line[0] == '-' ? 1 : 0;
    if (i == line.size()) return false;
    value = 0;
    for (; i < line.size(); ++i) {
        if (line[i] < '0' || line[i] > '9') return false;
        value = value * 10 + (line[i] - '0');
    }
    if (line[0] == '-') value = -value;
    return true;
}

// Consumes one RESP2 reply starting at pos.
bool read_reply(std::string_view out, std::size_t& pos, int depth) {
    if (pos >= out.size() || depth > 8) return false;
    const char type = out[pos++];
    std::string_view line;
    if (!read_line(out, pos, line)) return false;
    long long length = 0;
    switch (type) {
        case '+':
        case '-':
            return true;
        case ':':
            return read_length(line, length);
        case '$':
            if (!read_length(line, length) || length < -1) return false;
            if (length == -1) return true;
            if (out.size() - pos < static_cast<std::size_t>(length) + 2) return false;
            pos += static_cast<std::size_t>(length);
            if (out.compare(pos, 2, "\r\n") != 0) return false;
            pos += 2;
            return true;
        case '*':
            if (!read_length(line, length) || length < -1) return false;
            for (long long i = 0; i < length; ++i) {
                if (!read_reply(out, pos, depth + 1)) return false;
            }
            return true;
        default:
            return false;
    }
}

bool exactly_one_reply(std::string_view out) {
    std::size_t pos = 0;
    return read_reply(out, pos, 0) && pos == out.size();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) return 0;
    const std::string input(reinterpret_cast<const char*>(data) + 1, size - 1);
    const std::size_t split = input.empty() ? 0 : data[0] % (input.size() + 1);

    kv::resp::Limits limits;
    limits.max_bulk_length = 1 << 16;
    limits.max_array_length = 1024;
    limits.max_inline_length = 1 << 12;
    kv::resp::RequestParser parser(limits);
    kv::Database db;
    std::string buffer;
    std::string out;
    std::size_t offset = 0;
    kv::Args args;

    for (const std::size_t end : {split, input.size()}) {
        buffer.append(input, buffer.size(), end - buffer.size());
        for (;;) {
            const std::size_t before = offset;
            const auto status = parser.parse(buffer, offset, args);
            if (offset < before || offset > buffer.size()) std::abort();
            if (status == kv::resp::ParseStatus::error) return 0;
            if (status == kv::resp::ParseStatus::incomplete) break;
            if (offset == before) std::abort();  // a complete command must consume input
            if (!args.empty()) {
                out.clear();
                kv::ExecContext ctx{db, out, 1'000'000};
                kv::execute(ctx, args);
                if (!exactly_one_reply(out)) std::abort();
            }
        }
    }
    return 0;
}
