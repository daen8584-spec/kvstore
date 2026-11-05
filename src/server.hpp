#pragma once

#include <csignal>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "aof.hpp"
#include "commands.hpp"
#include "database.hpp"
#include "resp.hpp"

namespace kv {

// SIGINT and SIGTERM. They must be blocked in every thread, before any thread is
// created, for the server to receive them through its signalfd.
sigset_t shutdown_signals();

struct Config {
    std::string bind = "127.0.0.1";
    int port = 6379;
    std::string dir = ".";
    bool appendonly = false;
    std::string appendfilename = "appendonly.aof";
    FsyncPolicy appendfsync = FsyncPolicy::everysec;
    int auto_aof_rewrite_percentage = 100;
    std::uint64_t auto_aof_rewrite_min_size = 64ULL * 1024 * 1024;
    std::size_t maxclients = 10000;
    int hz = 10;
    resp::Limits limits;
};

// Single-threaded server: one epoll loop owns every connection and the dataset, so
// commands run one at a time without locks. File syncs and closes run on a worker
// thread, and append-only file compaction runs in a forked child.
class Server final : public ServerControl {
  public:
    explicit Server(Config config);
    ~Server() override;
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Loads persisted data, listens and serves until SIGINT or SIGTERM. Returns the
    // process exit code.
    int run();

    std::string info(std::string_view section) override;
    std::vector<std::pair<std::string, std::string>> config(std::string_view pattern) override;
    std::string start_aof_rewrite() override;

  private:
    struct Client {
        int fd = -1;
        std::uint64_t id = 0;
        std::string in;
        std::size_t in_pos = 0;
        resp::RequestParser parser;
        Args args;
        std::string out;
        std::size_t out_pos = 0;
        std::uint32_t events = 0;
        bool queued_for_write = false;
        bool close_after_reply = false;
        bool reading_paused = false;
    };

    bool setup(std::string& error);
    void accept_clients();
    void handle_readable(Client& client);
    void handle_writable(Client& client);
    void process_input(Client& client);
    bool write_output(Client& client);  // false if the client was closed
    void queue_write(Client& client);
    void update_events(Client& client, std::uint32_t events);
    void close_client(Client& client);
    void cron();
    void before_sleep();
    void shutdown();

    Config config_;
    Database db_;
    BackgroundJobs jobs_;
    std::unique_ptr<AppendOnlyFile> aof_;

    int epoll_fd_ = -1;
    int listen_fd_ = -1;
    int timer_fd_ = -1;
    int signal_fd_ = -1;
    bool stopping_ = false;

    std::unordered_map<int, std::unique_ptr<Client>> clients_;
    std::vector<int> pending_writes_;
    std::uint64_t next_client_id_ = 1;

    std::int64_t started_ms_ = 0;
    std::uint64_t total_connections_ = 0;
    std::uint64_t rejected_connections_ = 0;
    std::uint64_t total_commands_ = 0;
    std::uint64_t net_input_bytes_ = 0;
    std::uint64_t net_output_bytes_ = 0;
};

}  // namespace kv
