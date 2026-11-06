#include "zset.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <random>
#include <utility>

namespace kv {
namespace {

// Strict (score, member) order: score first, then member bytes.
bool before(const SkipList::Node* node, double score, std::string_view member) {
    return node->score < score || (node->score == score && node->member < member);
}

std::size_t at(int level) {
    return static_cast<std::size_t>(level);
}

}  // namespace

SkipList::Node* SkipList::make_node(int height, double score, std::string_view member) {
    void* memory = ::operator new(sizeof(Node) + at(height) * sizeof(Level));
    auto* node = new (memory) Node{member, score, nullptr, height};
    for (int i = 0; i < height; ++i) new (static_cast<void*>(node->levels() + i)) Level{};
    return node;
}

void SkipList::free_node(Node* node) {
    // Node and Level are trivially destructible; only the storage is released.
    ::operator delete(static_cast<void*>(node));
}

SkipList::SkipList() : header_(make_node(kInitialHeaderLevel, 0, {})) {}

SkipList::~SkipList() {
    release();
}

SkipList::SkipList(SkipList&& other) noexcept
    : header_(std::exchange(other.header_, make_node(kInitialHeaderLevel, 0, {}))),
      tail_(std::exchange(other.tail_, nullptr)),
      length_(std::exchange(other.length_, 0)),
      level_(std::exchange(other.level_, 1)) {}

SkipList& SkipList::operator=(SkipList&& other) noexcept {
    if (this != &other) {
        release();
        header_ = std::exchange(other.header_, make_node(kInitialHeaderLevel, 0, {}));
        tail_ = std::exchange(other.tail_, nullptr);
        length_ = std::exchange(other.length_, 0);
        level_ = std::exchange(other.level_, 1);
    }
    return *this;
}

void SkipList::release() {
    Node* node = header_;
    while (node) {
        Node* next = node->levels()[0].forward;
        free_node(node);
        node = next;
    }
    header_ = nullptr;
}

int SkipList::random_level() {
    // One generator for every skip list on the thread: a std::mt19937 is about 5 KB of
    // state, too much to keep per sorted set, and seeding reads the system's entropy
    // source.
    thread_local std::mt19937 rng{std::random_device{}()};
    // Each level is kept with probability 1/4, as in Redis, giving an expected 1.33
    // links per node and O(log n) search.
    int level = 1;
    while (level < kMaxLevel && (rng() & 0xFFFF) < 0xFFFF / 4) ++level;
    return level;
}

void SkipList::ensure_header_height(int height) {
    if (height <= header_->height) return;
    // Nothing points at the header, so it can be moved to a taller allocation.
    const int grown = std::min(kMaxLevel, std::max(height, header_->height * 2));
    Node* header = make_node(grown, 0, {});
    std::memcpy(static_cast<void*>(header->levels()), static_cast<const void*>(header_->levels()),
                at(header_->height) * sizeof(Level));
    free_node(header_);
    header_ = header;
}

SkipList::Node* SkipList::find_path(double score, std::string_view member, Node** update) const {
    Node* node = header_;
    for (int i = level_ - 1; i >= 0; --i) {
        while (node->levels()[at(i)].forward && before(node->levels()[at(i)].forward, score, member)) {
            node = node->levels()[at(i)].forward;
        }
        update[i] = node;
    }
    return node->levels()[0].forward;
}

void SkipList::link(Node* node) {
    ensure_header_height(node->height);
    Node* update[kMaxLevel];
    std::size_t rank[kMaxLevel];
    Node* cursor = header_;
    for (int i = level_ - 1; i >= 0; --i) {
        rank[i] = i == level_ - 1 ? 0 : rank[i + 1];
        while (cursor->levels()[at(i)].forward &&
               before(cursor->levels()[at(i)].forward, node->score, node->member)) {
            rank[i] += cursor->levels()[at(i)].span;
            cursor = cursor->levels()[at(i)].forward;
        }
        update[i] = cursor;
    }

    const int height = node->height;
    if (height > level_) {
        for (int i = level_; i < height; ++i) {
            rank[i] = 0;
            update[i] = header_;
            update[i]->levels()[at(i)].span = length_;
        }
        level_ = height;
    }

    for (int i = 0; i < height; ++i) {
        Level& previous = update[i]->levels()[at(i)];
        Level& own = node->levels()[at(i)];
        own.forward = previous.forward;
        previous.forward = node;
        // The span of the previous link is split around the new node.
        own.span = previous.span - (rank[0] - rank[i]);
        previous.span = (rank[0] - rank[i]) + 1;
    }
    // Links above the node's height now skip one more node.
    for (int i = height; i < level_; ++i) ++update[i]->levels()[at(i)].span;

    node->backward = update[0] == header_ ? nullptr : update[0];
    if (node->levels()[0].forward) {
        node->levels()[0].forward->backward = node;
    } else {
        tail_ = node;
    }
    ++length_;
}

void SkipList::unlink(Node* node, Node** update) {
    for (int i = 0; i < level_; ++i) {
        Level& link = update[i]->levels()[at(i)];
        if (link.forward == node) {
            link.span += node->levels()[at(i)].span - 1;
            link.forward = node->levels()[at(i)].forward;
        } else {
            --link.span;
        }
    }
    if (node->levels()[0].forward) {
        node->levels()[0].forward->backward = node->backward;
    } else {
        tail_ = node->backward;
    }
    while (level_ > 1 && header_->levels()[at(level_ - 1)].forward == nullptr) --level_;
    --length_;
}

void SkipList::insert(double score, std::string_view member) {
    link(make_node(random_level(), score, member));
}

bool SkipList::erase(double score, std::string_view member) {
    Node* update[kMaxLevel];
    Node* node = find_path(score, member, update);
    if (!node || node->score != score || node->member != member) return false;
    unlink(node, update);
    free_node(node);
    return true;
}

void SkipList::update(double old_score, std::string_view member, double new_score) {
    Node* update[kMaxLevel];
    Node* node = find_path(old_score, member, update);
    if (!node || node->score != old_score || node->member != member) return;
    // Fast path: the new score keeps the node between its neighbours.
    const Node* prev = node->backward;
    const Node* next = node->levels()[0].forward;
    if ((!prev || prev->score < new_score) && (!next || next->score > new_score)) {
        node->score = new_score;
        return;
    }
    unlink(node, update);
    node->score = new_score;
    for (int i = 0; i < node->height; ++i) node->levels()[at(i)] = Level{};
    link(node);
}

std::size_t SkipList::rank(double score, std::string_view member) const {
    std::size_t traversed = 0;
    const Node* node = header_;
    for (int i = level_ - 1; i >= 0; --i) {
        const Level* levels = node->levels();
        while (levels[at(i)].forward &&
               (before(levels[at(i)].forward, score, member) ||
                (levels[at(i)].forward->score == score && levels[at(i)].forward->member == member))) {
            traversed += levels[at(i)].span;
            node = levels[at(i)].forward;
            levels = node->levels();
        }
        if (node != header_ && node->score == score && node->member == member) return traversed;
    }
    return 0;
}

const SkipList::Node* SkipList::at_rank(std::size_t target) const {
    if (target == 0 || target > length_) return nullptr;
    std::size_t traversed = 0;
    const Node* node = header_;
    for (int i = level_ - 1; i >= 0; --i) {
        while (node->levels()[at(i)].forward && traversed + node->levels()[at(i)].span <= target) {
            traversed += node->levels()[at(i)].span;
            node = node->levels()[at(i)].forward;
        }
        if (traversed == target) return node;
    }
    return nullptr;
}

const SkipList::Node* SkipList::first_in_range(const ScoreRange& range) const {
    if (range.empty() || !tail_ || !range.above_min(tail_->score)) return nullptr;
    const Node* node = header_;
    for (int i = level_ - 1; i >= 0; --i) {
        while (node->levels()[at(i)].forward && !range.above_min(node->levels()[at(i)].forward->score)) {
            node = node->levels()[at(i)].forward;
        }
    }
    node = node->levels()[0].forward;
    return node && range.below_max(node->score) ? node : nullptr;
}

const SkipList::Node* SkipList::last_in_range(const ScoreRange& range) const {
    const Node* head = first();
    if (range.empty() || !head || !range.below_max(head->score)) return nullptr;
    const Node* node = header_;
    for (int i = level_ - 1; i >= 0; --i) {
        while (node->levels()[at(i)].forward && range.below_max(node->levels()[at(i)].forward->score)) {
            node = node->levels()[at(i)].forward;
        }
    }
    return node != header_ && range.above_min(node->score) ? node : nullptr;
}

std::optional<double> ZSet::score(std::string_view member) const {
    const double* found = members_.find(member);
    if (!found) return std::nullopt;
    return *found;
}

ZSet::Change ZSet::set(std::string_view member, double score) {
    return upsert(member, [score](std::optional<double>) { return std::optional<double>(score); });
}

bool ZSet::erase(std::string_view member) {
    auto* entry = members_.find_entry(member);
    if (!entry) return false;
    list_.erase(entry->value, entry->key);
    members_.erase(member);
    return true;
}

std::optional<std::size_t> ZSet::rank(std::string_view member, bool reverse) const {
    const double* found = members_.find(member);
    if (!found) return std::nullopt;
    const std::size_t rank = list_.rank(*found, member);
    return reverse ? list_.size() - rank : rank - 1;
}

bool parse_score(std::string_view text, double& score) {
    if (text.empty()) return false;
    std::string_view rest = text;
    if (rest.front() == '+') {
        rest.remove_prefix(1);
        // One sign only: "+-5" is not a number.
        if (rest.empty() || rest.front() == '-' || rest.front() == '+') return false;
    }
    if (rest == "inf" || rest == "INF" || rest == "Inf") {
        score = INFINITY;
        return true;
    }
    if (text == "-inf" || text == "-INF" || text == "-Inf") {
        score = -INFINITY;
        return true;
    }
    auto [ptr, ec] = std::from_chars(rest.data(), rest.data() + rest.size(), score);
    return ec == std::errc{} && ptr == rest.data() + rest.size() && !std::isnan(score);
}

bool parse_score_bound(std::string_view text, double& score, bool& exclusive) {
    exclusive = !text.empty() && text.front() == '(';
    if (exclusive) text.remove_prefix(1);
    return parse_score(text, score);
}

std::string format_score(double score) {
    if (std::isinf(score)) return score > 0 ? "inf" : "-inf";
    char buffer[32];
    auto [end, ec] = std::to_chars(buffer, buffer + sizeof buffer, score);
    return std::string(buffer, end);
}

}  // namespace kv
