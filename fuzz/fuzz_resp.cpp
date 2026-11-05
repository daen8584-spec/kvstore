// Fuzz target for the request parser and command execution.
//
// The input is split at a position chosen by its first byte and fed in two parts, so the
// fuzzer also explores requests that arrive across reads. Every complete command runs
// against a fresh database. The target checks parser invariants; sanitizers catch memory
// and undefined-behaviour errors.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "commands.hpp"
#include "database.hpp"
#include "resp.hpp"

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
                if (out.empty() ||
                    (out[0] != '+' && out[0] != '-' && out[0] != ':' && out[0] != '$' && out[0] != '*')) {
                    std::abort();  // every command must produce exactly a RESP reply
                }
            }
        }
    }
    return 0;
}
