#include <pthread.h>

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "log.hpp"
#include "server.hpp"

namespace {

void usage() {
    std::fputs(
        "Usage: kvstore [options]\n"
        "\n"
        "  --bind ADDRESS                     IPv4 address to listen on (default 127.0.0.1)\n"
        "  --port PORT                        TCP port (default 6379)\n"
        "  --dir PATH                         Directory for the append-only file (default .)\n"
        "  --appendonly yes|no                Enable append-only file persistence (default no)\n"
        "  --appendfilename NAME              Append-only file name (default appendonly.aof)\n"
        "  --appendfsync always|everysec|no   When to flush the file to disk (default everysec)\n"
        "  --auto-aof-rewrite-percentage N    Rewrite after N% growth, 0 disables (default 100)\n"
        "  --auto-aof-rewrite-min-size BYTES  Minimum size for automatic rewrites (default 64 MiB)\n"
        "  --maxclients N                     Maximum connected clients (default 10000)\n"
        "  --hz N                             Background task frequency per second (default 10)\n"
        "  --proto-max-bulk-len BYTES         Largest accepted string (default 512 MiB)\n"
        "  --loglevel debug|info|warning|error\n",
        stderr);
}

template <class T>
bool parse_number(std::string_view text, T& value) {
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc{} && ptr == text.data() + text.size();
}

}  // namespace

int main(int argc, char** argv) {
    kv::Config config;
    for (int i = 1; i < argc; ++i) {
        const std::string_view option = argv[i];
        if (option == "--help" || option == "-h") {
            usage();
            return 0;
        }
        if (i + 1 >= argc) {
            std::fprintf(stderr, "missing value for %s\n", argv[i]);
            return 2;
        }
        const std::string_view value = argv[++i];
        bool ok = true;
        if (option == "--bind")
            config.bind = value;
        else if (option == "--port")
            ok = parse_number(value, config.port) && config.port > 0 && config.port < 65536;
        else if (option == "--dir")
            config.dir = value;
        else if (option == "--appendonly") {
            ok = value == "yes" || value == "no";
            config.appendonly = value == "yes";
        } else if (option == "--appendfilename")
            config.appendfilename = value;
        else if (option == "--appendfsync")
            ok = kv::parse_fsync_policy(value, config.appendfsync);
        else if (option == "--auto-aof-rewrite-percentage")
            ok = parse_number(value, config.auto_aof_rewrite_percentage);
        else if (option == "--auto-aof-rewrite-min-size")
            ok = parse_number(value, config.auto_aof_rewrite_min_size);
        else if (option == "--maxclients")
            ok = parse_number(value, config.maxclients) && config.maxclients > 0;
        else if (option == "--hz")
            ok = parse_number(value, config.hz) && config.hz >= 1 && config.hz <= 500;
        else if (option == "--proto-max-bulk-len")
            ok = parse_number(value, config.limits.max_bulk_length);
        else if (option == "--loglevel") {
            kv::log::Level level{};
            ok = kv::log::parse_level(value, level);
            if (ok) kv::log::set_level(level);
        } else {
            std::fprintf(stderr, "unknown option %s\n", argv[i - 1]);
            usage();
            return 2;
        }
        if (!ok) {
            std::fprintf(stderr, "invalid value for %s: %s\n", argv[i - 1], argv[i]);
            return 2;
        }
    }

    // Must happen before the server starts its worker thread; see shutdown_signals().
    const sigset_t signals = kv::shutdown_signals();
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    kv::Server server(config);
    return server.run();
}
