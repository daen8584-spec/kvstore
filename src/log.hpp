#pragma once

#include <string_view>

namespace kv::log {

enum class Level { debug = 0, info = 1, warning = 2, error = 3 };

void set_level(Level level);
bool parse_level(std::string_view name, Level& level);
void write(Level level, std::string_view message);

inline void debug(std::string_view message) {
    write(Level::debug, message);
}
inline void info(std::string_view message) {
    write(Level::info, message);
}
inline void warning(std::string_view message) {
    write(Level::warning, message);
}
inline void error(std::string_view message) {
    write(Level::error, message);
}

}  // namespace kv::log
