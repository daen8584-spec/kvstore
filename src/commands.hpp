#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "database.hpp"

namespace kv {

using Args = std::vector<std::string>;

// Operations a command needs from the server beyond the keyspace.
class ServerControl {
  public:
    virtual ~ServerControl() = default;
    virtual std::string info(std::string_view section) = 0;
    virtual std::vector<std::pair<std::string, std::string>> config(std::string_view pattern) = 0;
    // Returns an empty string on success, or an error message.
    virtual std::string start_aof_rewrite() = 0;
};

struct ExecContext {
    ExecContext(Database& database, std::string& reply, std::int64_t now_ms, ServerControl* control = nullptr)
        : db(database), out(reply), now(now_ms), server(control) {}

    Database& db;
    std::string& out;
    std::int64_t now;
    ServerControl* server = nullptr;

    // Set by write commands that changed the dataset. The executor then records the
    // command in the append-only file, as-is or as the commands in `propagate`.
    bool dirty = false;
    std::vector<Args> propagate;
    bool close_connection = false;
};

struct Command {
    std::string_view name;
    int arity;  // exact argument count including the name, or -minimum
    bool write;
    void (*handler)(ExecContext&, const Args&);
};

const Command* find_command(std::string_view name);
std::size_t command_count();

// Executes one command and writes its reply to ctx.out. Returns the commands to append to
// the append-only file (empty when nothing changed).
std::vector<Args> execute(ExecContext& ctx, const Args& args);

}  // namespace kv
