#pragma once

// Sorted sets: members ordered by score, as in Redis.
//
// A sorted set keeps two structures over the same members. A hash table maps each
// member to its score for O(1) lookups (ZSCORE, and finding the old score on update).
// A skip list keeps (score, member) pairs in order for range queries. Each forward link
// in the skip list also records its span, the number of nodes it skips, so the rank of a
// member and the member at a given rank are found in O(log n) rather than by walking the
// list.

#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "dict.hpp"

namespace kv {

struct ScoreRange {
    double min = 0;
    double max = 0;
    bool min_exclusive = false;
    bool max_exclusive = false;

    bool above_min(double score) const { return min_exclusive ? score > min : score >= min; }
    bool below_max(double score) const { return max_exclusive ? score < max : score <= max; }
    bool contains(double score) const { return above_min(score) && below_max(score); }
    bool empty() const { return min > max || (min == max && (min_exclusive || max_exclusive)); }
};

class SkipList {
  public:
    struct Node;
    struct Level {
        Node* forward = nullptr;
        std::size_t span = 0;
    };
    // A node and its levels are one allocation. The member is a view of a string owned by
    // the caller (the sorted set's hash table), so each member is stored once.
    struct Node {
        std::string_view member;
        double score = 0;
        Node* backward = nullptr;
        int height = 0;

        Level* levels() { return std::launder(static_cast<Level*>(static_cast<void*>(this + 1))); }
        const Level* levels() const {
            return std::launder(static_cast<const Level*>(static_cast<const void*>(this + 1)));
        }
        const Node* next() const { return levels()[0].forward; }
        const Node* prev() const { return backward; }
    };

    static constexpr int kMaxLevel = 32;

    SkipList();
    ~SkipList();
    SkipList(const SkipList&) = delete;
    SkipList& operator=(const SkipList&) = delete;
    SkipList(SkipList&& other) noexcept;
    SkipList& operator=(SkipList&& other) noexcept;

    std::size_t size() const { return length_; }

    // The pair must not already be present, and `member` must outlive the node.
    void insert(double score, std::string_view member);
    bool erase(double score, std::string_view member);
    // Changes the score of a present pair. The node stays in place when its order does
    // not change, and is relinked without reallocating otherwise.
    void update(double old_score, std::string_view member, double new_score);

    // 1-based rank of the pair, or 0 if it is not present.
    std::size_t rank(double score, std::string_view member) const;
    // Node at a 1-based rank, or nullptr when out of range.
    const Node* at_rank(std::size_t rank) const;

    const Node* first() const { return header_->levels()[0].forward; }
    const Node* last() const { return tail_; }
    const Node* first_in_range(const ScoreRange& range) const;
    const Node* last_in_range(const ScoreRange& range) const;

  private:
    static Node* make_node(int height, double score, std::string_view member);
    static void free_node(Node* node);
    int random_level();
    // Finds the predecessors of (score, member) at every level.
    Node* find_path(double score, std::string_view member, Node** update) const;
    void link(Node* node);
    void unlink(Node* node, Node** update);
    void release();

    Node* header_;
    Node* tail_ = nullptr;
    std::size_t length_ = 0;
    int level_ = 1;
    std::mt19937 rng_{std::random_device{}()};
};

class ZSet {
  public:
    enum class Change { added, updated, unchanged };

    std::size_t size() const { return members_.size(); }
    bool empty() const { return members_.empty(); }

    std::optional<double> score(std::string_view member) const;
    Change set(std::string_view member, double score);

    // Looks the member up once and calls decide(current score, or nullopt if absent).
    // A returned score is stored; nullopt leaves the set unchanged.
    template <class Decide>
    Change upsert(std::string_view member, Decide&& decide) {
        if (auto* entry = members_.find_entry(member)) {
            const std::optional<double> next = decide(std::optional<double>(entry->value));
            if (!next || *next == entry->value) return Change::unchanged;
            list_.update(entry->value, entry->key, *next);
            entry->value = *next;
            return Change::updated;
        }
        const std::optional<double> next = decide(std::optional<double>());
        if (!next) return Change::unchanged;
        auto* entry = members_.try_emplace_entry(member, *next).first;
        list_.insert(*next, entry->key);
        return Change::added;
    }

    bool erase(std::string_view member);
    // 0-based position in ascending (or, with reverse, descending) order.
    std::optional<std::size_t> rank(std::string_view member, bool reverse) const;

    const SkipList& ordered() const { return list_; }

  private:
    Dict<double> members_;
    SkipList list_;
};

// Parses a score as Redis does: a decimal or "inf", "+inf", "-inf". NaN is rejected.
bool parse_score(std::string_view text, double& score);
// Parses a range bound, optionally prefixed with '(' for an exclusive bound.
bool parse_score_bound(std::string_view text, double& score, bool& exclusive);
// The shortest representation that parses back to the same value ("1", "0.1", "inf").
std::string format_score(double score);

}  // namespace kv
