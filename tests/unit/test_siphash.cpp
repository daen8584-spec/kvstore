#include <string>

#include "siphash.hpp"
#include "test.hpp"

namespace {

// Reference key from the SipHash paper: bytes 00..0f.
const kv::SipKey kReferenceKey{0x0706050403020100ULL, 0x0f0e0d0c0b0a0908ULL};

std::string bytes(std::size_t n) {
    std::string s;
    for (std::size_t i = 0; i < n; ++i) s.push_back(static_cast<char>(i));
    return s;
}

}  // namespace

TEST("siphash24 matches the reference test vectors") {
    CHECK_EQ(kv::siphash24(bytes(0), kReferenceKey), 0x726fdb47dd0e0e31ULL);
    CHECK_EQ(kv::siphash24(bytes(1), kReferenceKey), 0x74f839c593dc67fdULL);
    CHECK_EQ(kv::siphash24(bytes(15), kReferenceKey), 0xa129ca6149be45e5ULL);
}

TEST("siphash13 depends on every byte and on the key") {
    const std::string a = "the quick brown fox";
    std::string b = a;
    b.back() = 'y';
    CHECK(kv::siphash13(a, kReferenceKey) != kv::siphash13(b, kReferenceKey));
    CHECK(kv::siphash13(a, kReferenceKey) != kv::siphash13(a, kv::SipKey{1, 2}));
    CHECK_EQ(kv::siphash13(a, kReferenceKey), kv::siphash13(a, kReferenceKey));
}
