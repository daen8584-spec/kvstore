#include "glob.hpp"
#include "test.hpp"

using kv::glob_match;

TEST("glob matches wildcards") {
    CHECK(glob_match("*", ""));
    CHECK(glob_match("*", "anything"));
    CHECK(glob_match("user:*", "user:42"));
    CHECK(!glob_match("user:*", "session:42"));
    CHECK(glob_match("h?llo", "hello"));
    CHECK(!glob_match("h?llo", "hllo"));
    CHECK(glob_match("*:*:end", "a:b:end"));
}

TEST("glob matches character classes and escapes") {
    CHECK(glob_match("h[ae]llo", "hallo"));
    CHECK(!glob_match("h[ae]llo", "hillo"));
    CHECK(glob_match("h[^e]llo", "hallo"));
    CHECK(!glob_match("h[^e]llo", "hello"));
    CHECK(glob_match("key[0-9]", "key7"));
    CHECK(!glob_match("key[0-9]", "keyx"));
    CHECK(glob_match("a\\*b", "a*b"));
    CHECK(!glob_match("a\\*b", "axb"));
}

TEST("glob stays linear on adversarial patterns") {
    const std::string text(5000, 'a');
    CHECK(!glob_match("*a*a*a*a*a*a*a*a*a*a*b", text));
}
