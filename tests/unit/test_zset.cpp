#include <cmath>
#include <iterator>
#include <map>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "test.hpp"
#include "zset.hpp"

using kv::ScoreRange;
using kv::SkipList;
using kv::ZSet;

namespace {

using Reference = std::set<std::pair<double, std::string>>;

std::vector<std::pair<double, std::string>> walk(const SkipList& list) {
    std::vector<std::pair<double, std::string>> out;
    for (const SkipList::Node* node = list.first(); node; node = node->next())
        out.emplace_back(node->score, node->member);
    return out;
}

}  // namespace

TEST("skip list stays ordered and ranked like a reference set under random operations") {
    ZSet zset;
    std::map<std::string, double> scores;
    Reference reference;
    std::mt19937 rng(11);
    std::uniform_int_distribution<int> member_dist(0, 3000);
    std::uniform_int_distribution<int> score_dist(-50, 50);  // many equal scores, ordered by member

    for (int i = 0; i < 40000; ++i) {
        const std::string member = "m" + std::to_string(member_dist(rng));
        if (rng() % 4 == 0) {
            const bool removed = zset.erase(member);
            CHECK_EQ(removed, scores.count(member) == 1);
            if (removed) {
                reference.erase({scores[member], member});
                scores.erase(member);
            }
        } else {
            const double score = score_dist(rng) / 2.0;
            zset.set(member, score);
            if (scores.count(member)) reference.erase({scores[member], member});
            scores[member] = score;
            reference.insert({score, member});
        }
    }

    REQUIRE(zset.size() == reference.size());
    const std::vector<std::pair<double, std::string>> expected(reference.begin(), reference.end());
    CHECK(walk(zset.ordered()) == expected);

    std::size_t position = 0;
    for (const auto& [score, member] : reference) {
        CHECK_EQ(zset.rank(member, false).value_or(999999), position);
        CHECK_EQ(zset.rank(member, true).value_or(999999), reference.size() - 1 - position);
        const SkipList::Node* node = zset.ordered().at_rank(position + 1);
        REQUIRE(node != nullptr);
        CHECK_EQ(node->member, member);
        ++position;
    }
    CHECK(zset.ordered().at_rank(0) == nullptr);
    CHECK(zset.ordered().at_rank(reference.size() + 1) == nullptr);
}

TEST("skip list backward links mirror forward order") {
    ZSet zset;
    for (int i = 0; i < 500; ++i) zset.set("m" + std::to_string(i), i % 17);
    std::vector<std::string> forward, backward;
    for (const SkipList::Node* n = zset.ordered().first(); n; n = n->next()) forward.emplace_back(n->member);
    for (const SkipList::Node* n = zset.ordered().last(); n; n = n->prev()) backward.emplace_back(n->member);
    CHECK(forward == std::vector<std::string>(backward.rbegin(), backward.rend()));
}

TEST("skip list finds score ranges with inclusive and exclusive bounds") {
    ZSet zset;
    for (int i = 1; i <= 10; ++i) zset.set("m" + std::to_string(i), i);
    const SkipList& list = zset.ordered();

    const ScoreRange closed{3, 7, false, false};
    CHECK_EQ(list.first_in_range(closed)->score, 3.0);
    CHECK_EQ(list.last_in_range(closed)->score, 7.0);

    const ScoreRange open{3, 7, true, true};
    CHECK_EQ(list.first_in_range(open)->score, 4.0);
    CHECK_EQ(list.last_in_range(open)->score, 6.0);

    CHECK(list.first_in_range(ScoreRange{11, 20, false, false}) == nullptr);
    CHECK(list.first_in_range(ScoreRange{5, 5, true, false}) == nullptr);
    CHECK_EQ(list.last_in_range(ScoreRange{-INFINITY, INFINITY, false, false})->score, 10.0);
}

TEST("sorted set reports whether a write changed it") {
    ZSet zset;
    CHECK(zset.set("a", 1) == ZSet::Change::added);
    CHECK(zset.set("a", 1) == ZSet::Change::unchanged);
    CHECK(zset.set("a", 2) == ZSet::Change::updated);
    CHECK_EQ(zset.score("a").value_or(0), 2.0);
    CHECK(zset.erase("a"));
    CHECK(!zset.erase("a"));
    CHECK(zset.empty());
}

TEST("scores parse and format like Redis") {
    double score = 0;
    bool exclusive = false;
    CHECK(kv::parse_score("1.5", score) && score == 1.5);
    CHECK(kv::parse_score("+inf", score) && std::isinf(score) && score > 0);
    CHECK(kv::parse_score("-inf", score) && std::isinf(score) && score < 0);
    CHECK(kv::parse_score("-2e3", score) && score == -2000);
    CHECK(!kv::parse_score("nan", score));
    CHECK(!kv::parse_score("1.5x", score));
    CHECK(!kv::parse_score("", score));
    CHECK(!kv::parse_score("+-5", score));
    CHECK(!kv::parse_score("++5", score));
    CHECK(!kv::parse_score("+-inf", score));
    CHECK(!kv::parse_score("+", score));
    CHECK(kv::parse_score("+5", score) && score == 5);
    CHECK(kv::parse_score_bound("(5", score, exclusive) && exclusive && score == 5);
    CHECK(kv::parse_score_bound("5", score, exclusive) && !exclusive);

    CHECK_EQ(kv::format_score(1), std::string("1"));
    CHECK_EQ(kv::format_score(0.1), std::string("0.1"));
    CHECK_EQ(kv::format_score(-2.5), std::string("-2.5"));
    CHECK_EQ(kv::format_score(INFINITY), std::string("inf"));
    CHECK_EQ(kv::format_score(-INFINITY), std::string("-inf"));
}

TEST("an empty skip list is small and grows its header with the list") {
    // No per-list random generator or full-height header.
    CHECK(sizeof(kv::SkipList) <= 64);
    kv::SkipList list;
    std::vector<std::string> members;
    members.reserve(20000);
    for (int i = 0; i < 20000; ++i) members.push_back("m" + std::to_string(i));
    for (int i = 0; i < 20000; ++i) list.insert(i, members[static_cast<std::size_t>(i)]);
    CHECK_EQ(list.size(), std::size_t{20000});
    CHECK_EQ(list.rank(12345, members[12345]), std::size_t{12346});
    const auto* node = list.at_rank(19999);
    REQUIRE(node != nullptr);
    CHECK(node->member == "m19998");
}
