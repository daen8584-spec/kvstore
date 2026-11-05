#include "glob.hpp"

#include <cstddef>

namespace kv {
namespace {

// Matches a character class starting after '['. Advances p past the closing ']'.
bool match_class(std::string_view pattern, std::size_t& p, char c) {
    bool negate = false;
    if (p < pattern.size() && pattern[p] == '^') {
        negate = true;
        ++p;
    }
    bool matched = false;
    while (p < pattern.size() && pattern[p] != ']') {
        if (pattern[p] == '\\' && p + 1 < pattern.size()) {
            ++p;
            if (pattern[p] == c) matched = true;
        } else if (p + 2 < pattern.size() && pattern[p + 1] == '-' && pattern[p + 2] != ']') {
            char lo = pattern[p];
            char hi = pattern[p + 2];
            if (lo > hi) {
                const char t = lo;
                lo = hi;
                hi = t;
            }
            if (c >= lo && c <= hi) matched = true;
            p += 2;
        } else if (pattern[p] == c) {
            matched = true;
        }
        ++p;
    }
    if (p < pattern.size()) ++p;  // skip ']'
    return negate ? !matched : matched;
}

}  // namespace

bool glob_match(std::string_view pattern, std::string_view text) {
    // Iterative matcher with single-star backtracking: linear in practice and immune to
    // the exponential blow-up of naive recursion on patterns such as "*a*a*a*b".
    std::size_t p = 0;
    std::size_t t = 0;
    std::size_t star_p = std::string_view::npos;
    std::size_t star_t = 0;

    while (t < text.size()) {
        if (p < pattern.size()) {
            const char pc = pattern[p];
            if (pc == '*') {
                while (p < pattern.size() && pattern[p] == '*') ++p;
                if (p == pattern.size()) return true;
                star_p = p;
                star_t = t;
                continue;
            }
            if (pc == '?') {
                ++p;
                ++t;
                continue;
            }
            if (pc == '[') {
                std::size_t q = p + 1;
                if (match_class(pattern, q, text[t])) {
                    p = q;
                    ++t;
                    continue;
                }
            } else {
                char expected = pc;
                std::size_t next = p + 1;
                if (pc == '\\' && p + 1 < pattern.size()) {
                    expected = pattern[p + 1];
                    next = p + 2;
                }
                if (expected == text[t]) {
                    p = next;
                    ++t;
                    continue;
                }
            }
        }
        if (star_p == std::string_view::npos) return false;
        p = star_p;
        t = ++star_t;
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

}  // namespace kv
