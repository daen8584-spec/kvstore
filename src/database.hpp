#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <variant>

#include "dict.hpp"

namespace kv {

using List = std::deque<std::string>;
using Hash = Dict<std::string>;
using Value = std::variant<std::string, List, Hash>;

const char* type_name(const Value& value);

// Milliseconds since the Unix epoch. Expiry times are absolute, so they survive restarts
// and replay identically from the append-only file.
std::int64_t now_ms();

struct ExpireStats {
    std::size_t sampled = 0;
    std::size_t expired = 0;
};

// The keyspace: values plus a second table of expiry times for keys that have one, so
// active expiry samples only keys that can expire.
class Database {
  public:
    // Returns the value, or nullptr if the key does not exist or has expired. An expired
    // key is deleted on access (lazy expiry).
    Value* find(std::string_view key, std::int64_t now);

    // Stores a value, replacing any previous value and clearing its expiry unless
    // keep_ttl is set.
    Value& set(std::string_view key, Value value, bool keep_ttl = false);

    // Returns the value for key, creating it with `make()` if missing.
    template <class Make>
    Value& find_or_create(std::string_view key, std::int64_t now, Make&& make) {
        if (Value* existing = find(key, now)) return *existing;
        return *keys_.try_emplace(key, make()).first;
    }

    bool erase(std::string_view key);
    void clear();

    bool set_expire(std::string_view key, std::int64_t when_ms);
    std::optional<std::int64_t> expire_time(std::string_view key) const;
    bool persist(std::string_view key);

    std::size_t size() const { return keys_.size(); }
    std::size_t expires_size() const { return expires_.size(); }
    std::uint64_t expired_total() const { return expired_total_; }

    // Removes expired keys by random sampling, Redis style: sample up to 20 keys with an
    // expiry, delete the expired ones, and repeat while more than a quarter of a sample
    // was expired and the time budget allows.
    ExpireStats active_expire(std::int64_t now, std::chrono::microseconds budget);

    // Advances incremental rehashing of both tables during idle time.
    void rehash_idle(std::chrono::microseconds budget);

    const Dict<Value>& keys() const { return keys_; }

  private:
    bool expired(std::string_view key, std::int64_t now) const;

    Dict<Value> keys_;
    Dict<std::int64_t> expires_;
    std::mt19937_64 rng_{std::random_device{}()};
    std::uint64_t expired_total_ = 0;
};

}  // namespace kv
