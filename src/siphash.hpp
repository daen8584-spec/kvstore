#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace kv {

struct SipKey {
    std::uint64_t k0 = 0;
    std::uint64_t k1 = 0;
};

// SipHash with configurable compression and finalization rounds. The keyspace uses
// SipHash-1-3 with a per-process random key, so clients cannot choose keys that collide
// in the hash table (hash flooding).
std::uint64_t siphash(std::string_view data, const SipKey& key, int compression_rounds, int final_rounds);

inline std::uint64_t siphash13(std::string_view data, const SipKey& key) {
    return siphash(data, key, 1, 3);
}
inline std::uint64_t siphash24(std::string_view data, const SipKey& key) {
    return siphash(data, key, 2, 4);
}

// Random key generated once per process.
const SipKey& process_hash_key();

struct StringHash {
    std::uint64_t operator()(std::string_view s) const { return siphash13(s, process_hash_key()); }
};

}  // namespace kv
