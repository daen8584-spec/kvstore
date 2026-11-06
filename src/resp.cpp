#include "resp.hpp"

#include <algorithm>
#include <charconv>

namespace kv::resp {
namespace {

bool parse_int(std::string_view text, std::int64_t& value) {
    if (text.empty()) return false;
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(begin, end, value);
    return ec == std::errc{} && ptr == end;
}

}  // namespace

ParseStatus RequestParser::fail(std::string message) {
    error_ = std::move(message);
    array_remaining_ = 0;
    bulk_length_ = -1;
    return ParseStatus::error;
}

ParseStatus RequestParser::parse(std::string_view input, std::size_t& offset,
                                 std::vector<std::string>& args) {
    if (!in_progress()) {
        args.clear();
        if (offset >= input.size()) return ParseStatus::incomplete;
        if (input[offset] != '*') return parse_inline(input, offset, args);

        const std::size_t eol = input.find("\r\n", offset);
        if (eol == std::string_view::npos) {
            if (input.size() - offset > limits_.max_inline_length) {
                return fail("Protocol error: too big mbulk count string");
            }
            return ParseStatus::incomplete;
        }
        std::int64_t count = 0;
        if (!parse_int(input.substr(offset + 1, eol - offset - 1), count) ||
            count > static_cast<std::int64_t>(limits_.max_array_length)) {
            return fail("Protocol error: invalid multibulk length");
        }
        offset = eol + 2;
        if (count <= 0) return ParseStatus::complete;  // empty command, ignored by the caller
        array_remaining_ = count;
        // Capped so a large declared count cannot force a large allocation before any data arrives.
        args.reserve(static_cast<std::size_t>(std::min<std::int64_t>(count, 1024)));
    }

    while (array_remaining_ > 0) {
        if (bulk_length_ < 0) {
            if (offset >= input.size()) return ParseStatus::incomplete;
            if (input[offset] != '$') {
                return fail(std::string("Protocol error: expected '$', got '") + input[offset] + "'");
            }
            const std::size_t eol = input.find("\r\n", offset);
            if (eol == std::string_view::npos) {
                if (input.size() - offset > limits_.max_inline_length) {
                    return fail("Protocol error: too big bulk count string");
                }
                return ParseStatus::incomplete;
            }
            std::int64_t length = 0;
            if (!parse_int(input.substr(offset + 1, eol - offset - 1), length) || length < 0 ||
                length > static_cast<std::int64_t>(limits_.max_bulk_length)) {
                return fail("Protocol error: invalid bulk length");
            }
            bulk_length_ = length;
            offset = eol + 2;
        }
        const auto length = static_cast<std::size_t>(bulk_length_);
        if (input.size() - offset < length + 2) return ParseStatus::incomplete;
        if (input[offset + length] != '\r' || input[offset + length + 1] != '\n') {
            return fail("Protocol error: bulk string not terminated by CRLF");
        }
        args.emplace_back(input.substr(offset, length));
        offset += length + 2;
        bulk_length_ = -1;
        --array_remaining_;
    }
    return ParseStatus::complete;
}

ParseStatus RequestParser::parse_inline(std::string_view input, std::size_t& offset,
                                        std::vector<std::string>& args) {
    const std::size_t eol = input.find('\n', offset);
    if (eol == std::string_view::npos) {
        if (input.size() - offset > limits_.max_inline_length) {
            return fail("Protocol error: too big inline request");
        }
        return ParseStatus::incomplete;
    }
    std::string_view line = input.substr(offset, eol - offset);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    offset = eol + 1;

    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i == line.size()) break;
        const std::size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
        args.emplace_back(line.substr(start, i - start));
    }
    return ParseStatus::complete;
}

namespace {

// A status or error line ends at the first CR or LF, so either inside the text would end
// the reply early and let the rest be read as further replies. Replaced with spaces, as
// Redis does.
void line(std::string& out, char type, std::string_view text) {
    out += type;
    const std::size_t start = out.size();
    out += text;
    for (std::size_t i = start; i < out.size(); ++i) {
        if (out[i] == '\r' || out[i] == '\n') out[i] = ' ';
    }
    out += "\r\n";
}

}  // namespace

void simple_string(std::string& out, std::string_view value) {
    line(out, '+', value);
}

void error(std::string& out, std::string_view message) {
    line(out, '-', message);
}

void integer(std::string& out, std::int64_t value) {
    out += ':';
    out += std::to_string(value);
    out += "\r\n";
}

void bulk(std::string& out, std::string_view value) {
    out += '$';
    out += std::to_string(value.size());
    out += "\r\n";
    out += value;
    out += "\r\n";
}

void null_bulk(std::string& out) {
    out += "$-1\r\n";
}

void array_header(std::string& out, std::size_t length) {
    out += '*';
    out += std::to_string(length);
    out += "\r\n";
}

void null_array(std::string& out) {
    out += "*-1\r\n";
}

void command(std::string& out, const std::vector<std::string>& args) {
    array_header(out, args.size());
    for (const std::string& arg : args) bulk(out, arg);
}

}  // namespace kv::resp
