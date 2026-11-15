#pragma once

// Transactions: MULTI queues commands, EXEC runs them together, and WATCH makes EXEC fail
// if a watched key changed in between. The server runs one command at a time, so EXEC
// needs no locking; this module only keeps the per-client queue and the watched keys.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kv {

using Args = std::vector<std::string>;
class Database;

// Transaction state of one client.
struct Session {
    bool in_multi = false;
    // A command was rejected while queueing (unknown command, wrong arity); EXEC then
    // discards the transaction with EXECABORT.
    bool queue_error = false;
    // A watched key was modified; EXEC then fails with a null reply.
    bool watch_dirty = false;
    std::vector<Args> queued;
    // Watched keys and whether each existed when it was watched, so a key that expires
    // before EXEC counts as modified, as in Redis.
    std::vector<std::pair<std::string, bool>> watched;

    void end_multi() {
        in_multi = false;
        queue_error = false;
        queued.clear();
    }
};

// Which sessions watch which keys. Write commands call touch() for every key they
// modify; with no watched keys that is a single emptiness check.
class Watches {
  public:
    void watch(Session& session, const std::string& key, bool exists);
    void unwatch_all(Session& session);
    void touch(std::string_view key);
    // Marks the watchers of every watched key that currently exists. Used by FLUSHALL,
    // which (like Redis) does not affect watchers of keys that did not exist.
    void touch_existing(Database& db, std::int64_t now);
    std::size_t watched_keys() const { return sessions_by_key_.size(); }

  private:
    // Transparent hashing lets touch() look keys up without copying them into a
    // std::string, which allocates for keys longer than the small-string buffer.
    struct KeyHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view key) const { return std::hash<std::string_view>{}(key); }
    };

    // A filter in front of the map. A write to a key whose filter bit is clear skips the
    // map lookup. Measured in the server with 10,000 watched keys, looking up the map, or
    // a 256 KiB array of counters, on every write cost about 15% of pipelined SET
    // throughput, because the rest of the server evicts them from the CPU cache
    // (ADR 0009). The bits (32 KiB) are read on every write and stay cached; the counts,
    // read only by WATCH and UNWATCH, say when a bit can be cleared. Both are allocated
    // with the first watched key.
    static constexpr std::size_t kFilterBits = 1 << 18;
    static std::size_t slot(std::string_view key) { return KeyHash{}(key) & (kFilterBits - 1); }
    bool maybe_watched(std::size_t index) const { return (filter_bits_[index / 64] >> (index % 64)) & 1U; }
    void filter_add(std::string_view key);
    void filter_remove(std::string_view key);
    std::vector<std::uint64_t> filter_bits_;
    std::vector<std::uint32_t> filter_counts_;

    std::unordered_map<std::string, std::vector<Session*>, KeyHash, std::equal_to<>> sessions_by_key_;
};

// True if a key that existed when it was watched no longer exists, for example because
// it expired.
bool watched_key_vanished(const Session& session, Database& db, std::int64_t now);

}  // namespace kv
