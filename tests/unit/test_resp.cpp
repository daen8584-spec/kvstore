#include <string>
#include <vector>

#include "resp.hpp"
#include "test.hpp"

using kv::resp::ParseStatus;
using kv::resp::RequestParser;

namespace {

struct Parsed {
    std::vector<std::vector<std::string>> commands;
    ParseStatus last = ParseStatus::incomplete;
    std::string error;
};

// Feeds input in chunks of `step` bytes, like a socket would deliver it.
Parsed feed(const std::string& input, std::size_t step, kv::resp::Limits limits = {}) {
    RequestParser parser(limits);
    Parsed result;
    std::string buffer;
    std::size_t offset = 0;
    std::vector<std::string> args;
    for (std::size_t i = 0; i < input.size(); i += step) {
        buffer.append(input, i, step);
        for (;;) {
            result.last = parser.parse(buffer, offset, args);
            if (result.last == ParseStatus::complete) {
                if (!args.empty()) result.commands.push_back(args);
                continue;
            }
            if (result.last == ParseStatus::error) {
                result.error = parser.error();
                return result;
            }
            break;
        }
        buffer.erase(0, offset);
        offset = 0;
    }
    return result;
}

}  // namespace

TEST("resp parses a command split at every possible byte boundary") {
    const std::string input = "*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$12\r\nhello\r\nworld\r\n";
    for (std::size_t step = 1; step <= input.size(); ++step) {
        const Parsed parsed = feed(input, step);
        REQUIRE(parsed.commands.size() == 1);
        CHECK(parsed.commands[0] == (std::vector<std::string>{"SET", "key", "hello\r\nworld"}));
    }
}

TEST("resp parses pipelined commands and empty values") {
    const Parsed parsed =
        feed("*1\r\n$4\r\nPING\r\n*2\r\n$3\r\nGET\r\n$0\r\n\r\n*0\r\n*1\r\n$4\r\nPING\r\n", 7);
    REQUIRE(parsed.commands.size() == 3);
    CHECK(parsed.commands[1] == (std::vector<std::string>{"GET", ""}));
    CHECK(parsed.last == ParseStatus::incomplete);
}

TEST("resp parses inline commands") {
    const Parsed parsed = feed("PING\r\nSET  a   b\nGET a\r\n", 3);
    REQUIRE(parsed.commands.size() == 3);
    CHECK(parsed.commands[1] == (std::vector<std::string>{"SET", "a", "b"}));
}

TEST("resp rejects malformed input") {
    CHECK(feed("*x\r\n", 64).last == ParseStatus::error);
    CHECK(feed("*1\r\n+PING\r\n", 64).last == ParseStatus::error);
    CHECK(feed("*1\r\n$-5\r\n", 64).last == ParseStatus::error);
    CHECK(feed("*1\r\n$4\r\nPINGxx", 64).last == ParseStatus::error);
    CHECK(feed("*1\r\n$4\r\nPING\r\n", 64).commands.size() == 1);
}

TEST("resp enforces size limits") {
    kv::resp::Limits limits;
    limits.max_bulk_length = 10;
    limits.max_array_length = 4;
    limits.max_inline_length = 32;
    CHECK(feed("*1\r\n$11\r\n", 64, limits).last == ParseStatus::error);
    CHECK(feed("*5\r\n", 64, limits).last == ParseStatus::error);
    CHECK(feed(std::string(64, 'a'), 64, limits).last == ParseStatus::error);
}

TEST("resp scans a large value once while it arrives") {
    const std::string value(1 << 20, 'v');
    const std::string input = "*2\r\n$3\r\nGET\r\n$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
    const Parsed parsed = feed(input, 4096);
    REQUIRE(parsed.commands.size() == 1);
    CHECK(parsed.commands[0][1] == value);
}

TEST("resp encodes replies") {
    std::string out;
    kv::resp::simple_string(out, "OK");
    kv::resp::integer(out, -42);
    kv::resp::bulk(out, "hi");
    kv::resp::null_bulk(out);
    kv::resp::array_header(out, 2);
    CHECK_EQ(out, std::string("+OK\r\n:-42\r\n$2\r\nhi\r\n$-1\r\n*2\r\n"));
}
