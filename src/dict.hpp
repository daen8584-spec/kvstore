#pragma once

// Hash table with incremental rehashing.
//
// Growing a large table in one step stalls a single-threaded server for as long as it
// takes to move every entry. Like Redis, this table keeps two bucket arrays while it
// grows or shrinks and moves a few buckets on every operation (and in idle time), so the
// cost of a resize is spread across many requests instead of landing on one of them.

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "siphash.hpp"

namespace kv {

template <class V>
class Dict {
  public:
    struct Entry {
        std::string key;
        V value;
        Entry* next = nullptr;
    };

    Dict() = default;
    Dict(const Dict&) = delete;
    Dict& operator=(const Dict&) = delete;
    Dict(Dict&& other) noexcept { swap(other); }
    Dict& operator=(Dict&& other) noexcept {
        if (this != &other) {
            clear();
            swap(other);
        }
        return *this;
    }
    ~Dict() { clear(); }

    std::size_t size() const { return tables_[0].used + tables_[1].used; }
    bool empty() const { return size() == 0; }
    bool rehashing() const { return rehash_index_ >= 0; }
    std::size_t bucket_count() const { return tables_[0].buckets.size() + tables_[1].buckets.size(); }

    V* find(std::string_view key) {
        if (rehashing()) rehash_step(1);
        Entry* entry = locate(key);
        return entry ? &entry->value : nullptr;
    }

    const V* find(std::string_view key) const {
        const Entry* entry = const_cast<Dict*>(this)->locate(key);
        return entry ? &entry->value : nullptr;
    }

    // Inserts key with a value constructed from args unless the key exists. Returns the
    // value and whether it was inserted.
    template <class... Args>
    std::pair<V*, bool> try_emplace(std::string_view key, Args&&... args) {
        if (rehashing()) rehash_step(1);
        if (Entry* existing = locate(key)) return {&existing->value, false};
        expand_if_needed();
        Table& table = rehashing() ? tables_[1] : tables_[0];
        const std::size_t index = hash(key) & table.mask();
        auto* entry = new Entry{std::string(key), V(std::forward<Args>(args)...), table.buckets[index]};
        table.buckets[index] = entry;
        ++table.used;
        return {&entry->value, true};
    }

    bool erase(std::string_view key) {
        if (empty()) return false;
        if (rehashing()) rehash_step(1);
        const std::uint64_t h = hash(key);
        for (int t = 0; t < 2; ++t) {
            Table& table = tables_[t];
            if (table.buckets.empty()) continue;
            Entry** link = &table.buckets[h & table.mask()];
            while (*link) {
                Entry* entry = *link;
                if (entry->key == key) {
                    *link = entry->next;
                    delete entry;
                    --table.used;
                    shrink_if_needed();
                    return true;
                }
                link = &entry->next;
            }
            if (!rehashing()) break;
        }
        return false;
    }

    void clear() {
        for (Table& table : tables_) {
            for (Entry*& head : table.buckets) {
                while (head) {
                    Entry* next = head->next;
                    delete head;
                    head = next;
                }
            }
            table = Table{};
        }
        rehash_index_ = -1;
    }

    // Moves up to `buckets` buckets to the new table. Returns false once rehashing is done.
    bool rehash_step(std::size_t buckets) {
        if (!rehashing()) return false;
        std::size_t empty_visits = buckets * 10;
        Table& from = tables_[0];
        Table& to = tables_[1];
        while (buckets-- > 0 && from.used > 0) {
            while (from.buckets[static_cast<std::size_t>(rehash_index_)] == nullptr) {
                ++rehash_index_;
                if (--empty_visits == 0) return true;
            }
            Entry* entry = from.buckets[static_cast<std::size_t>(rehash_index_)];
            while (entry) {
                Entry* next = entry->next;
                const std::size_t index = hash(entry->key) & to.mask();
                entry->next = to.buckets[index];
                to.buckets[index] = entry;
                --from.used;
                ++to.used;
                entry = next;
            }
            from.buckets[static_cast<std::size_t>(rehash_index_)] = nullptr;
            ++rehash_index_;
        }
        if (from.used == 0) {
            tables_[0] = std::move(tables_[1]);
            tables_[1] = Table{};
            rehash_index_ = -1;
            return false;
        }
        return true;
    }

    // Rehashes in batches for roughly the given time budget. Used in idle time.
    void rehash_for(std::chrono::microseconds budget) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (rehash_step(100)) {
            if (std::chrono::steady_clock::now() >= deadline) break;
        }
    }

    template <class F>
    void for_each(F&& visit) const {
        for (const Table& table : tables_) {
            for (const Entry* entry : table.buckets) {
                for (; entry; entry = entry->next) visit(entry->key, entry->value);
            }
        }
    }

    // Returns a random entry, or nullptr when the table is empty. Used to sample keys for
    // active expiry. Selection is uniform over buckets, not exactly uniform over keys.
    template <class Rng>
    Entry* random_entry(Rng& rng) {
        if (empty()) return nullptr;
        if (rehashing()) rehash_step(1);
        Entry* head = nullptr;
        if (rehashing()) {
            const std::size_t start = static_cast<std::size_t>(rehash_index_);
            const std::size_t total = tables_[0].buckets.size() + tables_[1].buckets.size();
            std::uniform_int_distribution<std::size_t> pick(start, total - 1);
            while (!head) {
                const std::size_t i = pick(rng);
                head = i < tables_[0].buckets.size() ? tables_[0].buckets[i]
                                                     : tables_[1].buckets[i - tables_[0].buckets.size()];
            }
        } else {
            std::uniform_int_distribution<std::size_t> pick(0, tables_[0].buckets.size() - 1);
            while (!head) head = tables_[0].buckets[pick(rng)];
        }
        std::size_t length = 0;
        for (Entry* e = head; e; e = e->next) ++length;
        std::uniform_int_distribution<std::size_t> pick(0, length - 1);
        for (std::size_t skip = pick(rng); skip > 0; --skip) head = head->next;
        return head;
    }

    // Cursor-based iteration that tolerates resizes between calls (the SCAN guarantee):
    // every key present for the whole iteration is returned at least once. The cursor
    // is incremented in bit-reversed order, so the buckets already visited map onto
    // visited buckets in a table of any other power-of-two size. Returns the next cursor;
    // 0 ends the iteration.
    template <class F>
    std::uint64_t scan(std::uint64_t cursor, F&& visit) const {
        if (empty()) return 0;
        auto emit = [&](const Table& table, std::uint64_t index) {
            for (const Entry* e = table.buckets[index & table.mask()]; e; e = e->next)
                visit(e->key, e->value);
        };
        if (!rehashing()) {
            const Table& table = tables_[0];
            const std::uint64_t mask = table.mask();
            emit(table, cursor);
            cursor |= ~mask;
            cursor = reverse_bits(reverse_bits(cursor) + 1);
            return cursor;
        }
        const Table* small = &tables_[0];
        const Table* large = &tables_[1];
        if (small->buckets.size() > large->buckets.size()) std::swap(small, large);
        const std::uint64_t small_mask = small->mask();
        const std::uint64_t large_mask = large->mask();
        emit(*small, cursor);
        do {
            emit(*large, cursor);
            cursor |= ~large_mask;
            cursor = reverse_bits(reverse_bits(cursor) + 1);
        } while (cursor & (small_mask ^ large_mask));
        return cursor;
    }

    void swap(Dict& other) noexcept {
        std::swap(tables_, other.tables_);
        std::swap(rehash_index_, other.rehash_index_);
    }

  private:
    struct Table {
        std::vector<Entry*> buckets;
        std::size_t used = 0;
        std::uint64_t mask() const { return buckets.empty() ? 0 : buckets.size() - 1; }
    };

    static constexpr std::size_t kInitialSize = 4;

    static std::uint64_t hash(std::string_view key) { return StringHash{}(key); }

    static std::uint64_t reverse_bits(std::uint64_t v) {
        v = ((v >> 1) & 0x5555555555555555ULL) | ((v & 0x5555555555555555ULL) << 1);
        v = ((v >> 2) & 0x3333333333333333ULL) | ((v & 0x3333333333333333ULL) << 2);
        v = ((v >> 4) & 0x0F0F0F0F0F0F0F0FULL) | ((v & 0x0F0F0F0F0F0F0F0FULL) << 4);
        v = ((v >> 8) & 0x00FF00FF00FF00FFULL) | ((v & 0x00FF00FF00FF00FFULL) << 8);
        v = ((v >> 16) & 0x0000FFFF0000FFFFULL) | ((v & 0x0000FFFF0000FFFFULL) << 16);
        return (v >> 32) | (v << 32);
    }

    Entry* locate(std::string_view key) {
        if (empty()) return nullptr;
        const std::uint64_t h = hash(key);
        for (int t = 0; t < 2; ++t) {
            const Table& table = tables_[t];
            if (table.buckets.empty()) continue;
            for (Entry* e = table.buckets[h & table.mask()]; e; e = e->next) {
                if (e->key == key) return e;
            }
            if (!rehashing()) break;
        }
        return nullptr;
    }

    void start_resize(std::size_t target) {
        const std::size_t size = std::bit_ceil(std::max(target, kInitialSize));
        if (tables_[0].buckets.empty()) {
            tables_[0].buckets.assign(size, nullptr);
            return;
        }
        if (size == tables_[0].buckets.size()) return;
        tables_[1].buckets.assign(size, nullptr);
        tables_[1].used = 0;
        rehash_index_ = 0;
    }

    void expand_if_needed() {
        if (rehashing()) return;
        if (tables_[0].buckets.empty()) {
            start_resize(kInitialSize);
        } else if (tables_[0].used >= tables_[0].buckets.size()) {
            start_resize(tables_[0].used + 1);
        }
    }

    void shrink_if_needed() {
        if (rehashing()) return;
        const std::size_t buckets = tables_[0].buckets.size();
        if (buckets > kInitialSize && tables_[0].used * 8 < buckets) start_resize(tables_[0].used);
    }

    Table tables_[2];
    std::ptrdiff_t rehash_index_ = -1;
};

}  // namespace kv
