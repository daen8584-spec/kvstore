#include <random>
#include <set>
#include <string>
#include <unordered_map>

#include "dict.hpp"
#include "test.hpp"

using kv::Dict;

TEST("dict inserts, finds and erases") {
    Dict<int> d;
    CHECK(d.empty());
    auto [value, inserted] = d.try_emplace("a", 1);
    CHECK(inserted);
    CHECK_EQ(*value, 1);
    CHECK(!d.try_emplace("a", 2).second);
    CHECK_EQ(*d.find("a"), 1);
    CHECK(d.find("b") == nullptr);
    CHECK(d.erase("a"));
    CHECK(!d.erase("a"));
    CHECK(d.empty());
}

TEST("dict stays consistent with a reference map through growth and shrinking") {
    Dict<int> d;
    std::unordered_map<std::string, int> reference;
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> key_dist(0, 20000);
    std::uniform_int_distribution<int> op(0, 9);
    bool saw_rehash = false;

    for (int i = 0; i < 200000; ++i) {
        const std::string key = "k" + std::to_string(key_dist(rng));
        // Insert-heavy first half, delete-heavy second half, so the table grows and shrinks.
        const bool insert = i < 100000 ? op(rng) < 7 : op(rng) < 2;
        if (insert) {
            auto [slot, inserted] = d.try_emplace(key, i);
            if (!inserted) *slot = i;
            reference[key] = i;
        } else {
            CHECK_EQ(d.erase(key), reference.erase(key) == 1);
        }
        saw_rehash = saw_rehash || d.rehashing();
        if (i % 10007 == 0) {
            REQUIRE(d.size() == reference.size());
            for (const auto& [k, v] : reference) {
                const int* found = d.find(k);
                REQUIRE(found != nullptr);
                CHECK_EQ(*found, v);
            }
        }
    }
    CHECK(saw_rehash);
    CHECK_EQ(d.size(), reference.size());
}

TEST("dict for_each visits every entry while rehashing") {
    Dict<int> d;
    for (int i = 0; i < 1000; ++i) d.try_emplace(std::to_string(i), i);
    while (!d.rehashing()) d.try_emplace("extra" + std::to_string(d.size()), 0);
    std::size_t visited = 0;
    d.for_each([&](const std::string&, const int&) { ++visited; });
    CHECK_EQ(visited, d.size());
}

TEST("dict scan returns every key present for the whole scan across resizes") {
    Dict<int> d;
    for (int i = 0; i < 5000; ++i) d.try_emplace("stable" + std::to_string(i), i);
    std::set<std::string> seen;
    std::uint64_t cursor = 0;
    int step = 0;
    do {
        cursor = d.scan(cursor, [&](const std::string& key, const int&) { seen.insert(key); });
        // Grow and shrink the table between calls with keys that are not part of the check.
        if (step % 3 == 0) {
            for (int j = 0; j < 400; ++j) d.try_emplace("churn" + std::to_string(step * 1000 + j), 0);
        } else if (step % 3 == 1) {
            for (int j = 0; j < 400; ++j) d.erase("churn" + std::to_string((step - 1) * 1000 + j));
        }
        ++step;
    } while (cursor != 0);

    for (int i = 0; i < 5000; ++i) CHECK(seen.count("stable" + std::to_string(i)) == 1);
}

TEST("dict random_entry only returns live entries") {
    Dict<int> d;
    std::mt19937_64 rng(7);
    CHECK(d.random_entry(rng) == nullptr);
    for (int i = 0; i < 100; ++i) d.try_emplace(std::to_string(i), i);
    std::set<std::string> sampled;
    for (int i = 0; i < 2000; ++i) {
        auto* entry = d.random_entry(rng);
        REQUIRE(entry != nullptr);
        CHECK_EQ(std::to_string(entry->value), entry->key);
        sampled.insert(entry->key);
    }
    CHECK(sampled.size() > 90);
}

TEST("dict move transfers ownership") {
    Dict<std::string> a;
    a.try_emplace("x", "1");
    Dict<std::string> b = std::move(a);
    CHECK(a.empty());
    CHECK_EQ(*b.find("x"), std::string("1"));
}
