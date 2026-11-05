#pragma once

#include <string_view>

namespace kv {

// Glob-style matching with Redis semantics: '*', '?', character classes ("[abc]",
// "[^a-z]") and backslash escapes. Used by KEYS and SCAN MATCH.
bool glob_match(std::string_view pattern, std::string_view text);

}  // namespace kv
