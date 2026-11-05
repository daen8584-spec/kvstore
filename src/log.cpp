#include "log.hpp"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

namespace kv::log {
namespace {

std::atomic<Level> current_level{Level::info};

const char* label(Level level) {
    switch (level) {
        case Level::debug:
            return "debug";
        case Level::info:
            return "info";
        case Level::warning:
            return "warning";
        case Level::error:
            return "error";
    }
    return "info";
}

}  // namespace

void set_level(Level level) {
    current_level.store(level);
}

bool parse_level(std::string_view name, Level& level) {
    if (name == "debug")
        level = Level::debug;
    else if (name == "info")
        level = Level::info;
    else if (name == "warning")
        level = Level::warning;
    else if (name == "error")
        level = Level::error;
    else
        return false;
    return true;
}

void write(Level level, std::string_view message) {
    if (level < current_level.load()) return;
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm utc{};
    gmtime_r(&seconds, &utc);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%S", &utc);

    std::string line;
    line.reserve(message.size() + 64);
    line += stamp;
    char ms[8];
    std::snprintf(ms, sizeof ms, ".%03dZ", static_cast<int>(millis));
    line += ms;
    line += " pid=";
    line += std::to_string(getpid());
    line += ' ';
    line += label(level);
    line += ' ';
    line += message;
    line += '\n';
    // A single write keeps lines from the server and a rewrite child from interleaving.
    [[maybe_unused]] const ssize_t written = ::write(STDERR_FILENO, line.data(), line.size());
}

}  // namespace kv::log
