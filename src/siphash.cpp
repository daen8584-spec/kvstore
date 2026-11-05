#include "siphash.hpp"

#include <random>

namespace kv {
namespace {

constexpr std::uint64_t rotl(std::uint64_t x, int b) {
    return (x << b) | (x >> (64 - b));
}

std::uint64_t load_le64(const unsigned char* p) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

struct State {
    std::uint64_t v0, v1, v2, v3;

    void round() {
        v0 += v1;
        v1 = rotl(v1, 13);
        v1 ^= v0;
        v0 = rotl(v0, 32);
        v2 += v3;
        v3 = rotl(v3, 16);
        v3 ^= v2;
        v0 += v3;
        v3 = rotl(v3, 21);
        v3 ^= v0;
        v2 += v1;
        v1 = rotl(v1, 17);
        v1 ^= v2;
        v2 = rotl(v2, 32);
    }
};

}  // namespace

std::uint64_t siphash(std::string_view data, const SipKey& key, int compression_rounds, int final_rounds) {
    State s{key.k0 ^ 0x736f6d6570736575ULL, key.k1 ^ 0x646f72616e646f6dULL, key.k0 ^ 0x6c7967656e657261ULL,
            key.k1 ^ 0x7465646279746573ULL};
    const auto* p = reinterpret_cast<const unsigned char*>(data.data());
    const std::size_t len = data.size();
    const std::size_t full = len - (len % 8);

    for (std::size_t i = 0; i < full; i += 8) {
        const std::uint64_t m = load_le64(p + i);
        s.v3 ^= m;
        for (int r = 0; r < compression_rounds; ++r) s.round();
        s.v0 ^= m;
    }

    std::uint64_t b = static_cast<std::uint64_t>(len) << 56;
    for (std::size_t i = full; i < len; ++i) b |= static_cast<std::uint64_t>(p[i]) << (8 * (i - full));
    s.v3 ^= b;
    for (int r = 0; r < compression_rounds; ++r) s.round();
    s.v0 ^= b;

    s.v2 ^= 0xff;
    for (int r = 0; r < final_rounds; ++r) s.round();
    return s.v0 ^ s.v1 ^ s.v2 ^ s.v3;
}

const SipKey& process_hash_key() {
    static const SipKey key = [] {
        std::random_device device;
        auto word = [&] { return (static_cast<std::uint64_t>(device()) << 32) | device(); };
        return SipKey{word(), word()};
    }();
    return key;
}

}  // namespace kv
