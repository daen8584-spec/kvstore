#pragma once

// RESP2 request parsing and reply encoding.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kv::resp {

struct Limits {
    std::size_t max_bulk_length = 512ULL * 1024 * 1024;
    std::size_t max_array_length = 1024 * 1024;
    std::size_t max_inline_length = 64 * 1024;
};

enum class ParseStatus { complete, incomplete, error };

// Incremental parser for client requests: arrays of bulk strings, or inline commands
// ("PING\r\n"). It keeps its position between calls, so a large value that arrives in
// many reads is scanned once rather than re-parsed from the start on every read.
class RequestParser {
  public:
    explicit RequestParser(Limits limits = {}) : limits_(limits) {}

    // Parses from input[offset...]. On `complete`, args holds the command and offset
    // points past it. On `incomplete`, the parser keeps what it consumed and must be
    // called again with the same buffer extended by more data. On `error`, error()
    // describes the problem and the connection should be closed.
    ParseStatus parse(std::string_view input, std::size_t& offset, std::vector<std::string>& args);

    const std::string& error() const { return error_; }

    // True while a command is partially parsed. A buffer may only be compacted
    // (consumed bytes discarded) between commands or after adjusting offset accordingly.
    bool in_progress() const { return array_remaining_ > 0 || bulk_length_ >= 0; }

  private:
    ParseStatus parse_inline(std::string_view input, std::size_t& offset, std::vector<std::string>& args);
    ParseStatus fail(std::string message);

    Limits limits_;
    std::int64_t array_remaining_ = 0;
    std::int64_t bulk_length_ = -1;
    std::string error_;
};

// Reply encoding. Each function appends to `out`.
void simple_string(std::string& out, std::string_view value);
void error(std::string& out, std::string_view message);
void integer(std::string& out, std::int64_t value);
void bulk(std::string& out, std::string_view value);
void null_bulk(std::string& out);
void array_header(std::string& out, std::size_t length);
void null_array(std::string& out);

inline constexpr std::string_view kOk = "+OK\r\n";

// Encodes a command as a RESP array of bulk strings (the append-only file format).
void command(std::string& out, const std::vector<std::string>& args);

}  // namespace kv::resp
