#include "server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <sstream>

#include "glob.hpp"
#include "log.hpp"

namespace kv {
namespace {

constexpr std::size_t kReadChunk = 16 * 1024;
// Bytes read from one client per readiness event, so a fast writer cannot starve others.
constexpr std::size_t kMaxReadPerEvent = 1024 * 1024;
// Unsent reply bytes above which a client's further commands wait until it reads.
constexpr std::size_t kOutputSoftLimit = 4 * 1024 * 1024;
constexpr std::size_t kQueryBufferLimit = 1024ULL * 1024 * 1024;

std::string errno_text() {
    return std::strerror(errno);
}

}  // namespace

sigset_t shutdown_signals() {
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    return signals;
}

Server::Server(Config config) : config_(std::move(config)) {}

Server::~Server() {
    for (auto& [fd, client] : clients_) ::close(fd);
    for (int fd : {listen_fd_, timer_fd_, signal_fd_, epoll_fd_}) {
        if (fd >= 0) ::close(fd);
    }
}

bool Server::setup(std::string& error) {
    std::signal(SIGPIPE, SIG_IGN);

    // main() blocks these in every thread before any thread starts, so they are only
    // delivered through this descriptor.
    const sigset_t signals = shutdown_signals();
    signal_fd_ = ::signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC);

    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    timer_fd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (signal_fd_ < 0 || epoll_fd_ < 0 || timer_fd_ < 0) {
        error = "cannot create event loop descriptors: " + errno_text();
        return false;
    }

    const long period_ns = 1'000'000'000L / std::max(1, config_.hz);
    itimerspec spec{};
    spec.it_interval.tv_sec = period_ns / 1'000'000'000L;
    spec.it_interval.tv_nsec = period_ns % 1'000'000'000L;
    spec.it_value = spec.it_interval;
    ::timerfd_settime(timer_fd_, 0, &spec, nullptr);

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) {
        error = "cannot create socket: " + errno_text();
        return false;
    }
    const int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(config_.port));
    if (::inet_pton(AF_INET, config_.bind.c_str(), &address.sin_addr) != 1) {
        error = "invalid bind address: " + config_.bind;
        return false;
    }
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 ||
        ::listen(listen_fd_, 511) != 0) {
        error = "cannot listen on " + config_.bind + ":" + std::to_string(config_.port) + ": " + errno_text();
        return false;
    }

    for (int fd : {listen_fd_, timer_fd_, signal_fd_}) {
        epoll_event event{};
        event.events = EPOLLIN;
        event.data.fd = fd;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event);
    }
    return true;
}

int Server::run() {
    started_ms_ = now_ms();

    if (config_.appendonly) {
        const std::string path = config_.dir + "/" + config_.appendfilename;
        const auto loaded = AppendOnlyFile::load(path, db_, config_.limits);
        if (!loaded.ok) {
            log::error(loaded.error);
            return 1;
        }
        log::info("loaded " + std::to_string(loaded.commands) + " commands from " + path + " in " +
                  std::to_string(now_ms() - started_ms_) + " ms, " + std::to_string(db_.size()) + " keys");
        aof_ = std::make_unique<AppendOnlyFile>(path, config_.appendfsync, jobs_);
        std::string error;
        if (!aof_->open(error)) {
            log::error(error);
            return 1;
        }
    }

    std::string error;
    if (!setup(error)) {
        log::error(error);
        return 1;
    }
    log::info("ready to accept connections on " + config_.bind + ":" + std::to_string(config_.port));

    std::vector<epoll_event> events(1024);
    while (!stopping_) {
        const int count = ::epoll_wait(epoll_fd_, events.data(), static_cast<int>(events.size()), -1);
        if (count < 0) {
            if (errno == EINTR) continue;
            log::error("epoll_wait failed: " + errno_text());
            break;
        }
        for (int i = 0; i < count; ++i) {
            const int fd = events[static_cast<std::size_t>(i)].data.fd;
            const std::uint32_t ready = events[static_cast<std::size_t>(i)].events;
            if (fd == listen_fd_) {
                accept_clients();
            } else if (fd == timer_fd_) {
                std::uint64_t expirations = 0;
                [[maybe_unused]] const ssize_t n = ::read(timer_fd_, &expirations, sizeof expirations);
                cron();
            } else if (fd == signal_fd_) {
                signalfd_siginfo info{};
                [[maybe_unused]] const ssize_t n = ::read(signal_fd_, &info, sizeof info);
                log::info(std::string("received ") + (info.ssi_signo == SIGINT ? "SIGINT" : "SIGTERM") +
                          ", shutting down");
                stopping_ = true;
            } else {
                auto it = clients_.find(fd);
                if (it == clients_.end()) continue;
                Client& client = *it->second;
                if (ready & (EPOLLIN | EPOLLHUP | EPOLLERR)) {
                    handle_readable(client);
                    if (clients_.find(fd) == clients_.end()) continue;
                }
                if (ready & EPOLLOUT) handle_writable(client);
            }
        }
        before_sleep();
    }
    shutdown();
    return 0;
}

void Server::accept_clients() {
    for (;;) {
        const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) log::warning("accept failed: " + errno_text());
            return;
        }
        ++total_connections_;
        if (clients_.size() >= config_.maxclients) {
            static constexpr std::string_view kFull = "-ERR max number of clients reached\r\n";
            [[maybe_unused]] const ssize_t n = ::write(fd, kFull.data(), kFull.size());
            ::close(fd);
            ++rejected_connections_;
            continue;
        }
        const int yes = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes);
        auto client = std::make_unique<Client>();
        client->fd = fd;
        client->id = next_client_id_++;
        client->parser = resp::RequestParser(config_.limits);
        client->events = EPOLLIN;
        epoll_event event{};
        event.events = EPOLLIN;
        event.data.fd = fd;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) != 0) {
            ::close(fd);
            continue;
        }
        clients_.emplace(fd, std::move(client));
    }
}

void Server::handle_readable(Client& client) {
    std::size_t total = 0;
    while (total < kMaxReadPerEvent) {
        const std::size_t old_size = client.in.size();
        client.in.resize(old_size + kReadChunk);
        const ssize_t n = ::read(client.fd, client.in.data() + old_size, kReadChunk);
        if (n <= 0) {
            client.in.resize(old_size);
            if (n == 0) return close_client(client);
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return close_client(client);
        }
        client.in.resize(old_size + static_cast<std::size_t>(n));
        total += static_cast<std::size_t>(n);
        if (static_cast<std::size_t>(n) < kReadChunk) break;
    }
    net_input_bytes_ += total;
    if (client.in.size() > kQueryBufferLimit) {
        log::warning("closing client " + std::to_string(client.id) + ": query buffer limit exceeded");
        return close_client(client);
    }
    process_input(client);
}

void Server::process_input(Client& client) {
    while (!client.close_after_reply && client.out.size() - client.out_pos < kOutputSoftLimit) {
        const auto status = client.parser.parse(client.in, client.in_pos, client.args);
        if (status == resp::ParseStatus::incomplete) break;
        if (status == resp::ParseStatus::error) {
            resp::error(client.out, "ERR " + client.parser.error());
            client.close_after_reply = true;
            break;
        }
        if (client.args.empty()) continue;

        ExecContext ctx{db_, client.out, now_ms(), this};
        const std::vector<Args> records = execute(ctx, client.args);
        ++total_commands_;
        if (aof_) {
            for (const Args& record : records) aof_->append(record);
        }
        if (ctx.close_connection) client.close_after_reply = true;
    }
    if (client.in_pos > 0) {
        client.in.erase(0, client.in_pos);
        client.in_pos = 0;
    }

    // Backpressure: a client that sends commands faster than it reads replies stops being
    // read until its replies drain, so its unread output cannot grow without bound.
    const bool backlog = client.out.size() - client.out_pos >= kOutputSoftLimit;
    if (backlog && !client.reading_paused) {
        client.reading_paused = true;
        update_events(client, client.events & ~static_cast<std::uint32_t>(EPOLLIN));
    }
    if (client.out.size() > client.out_pos) queue_write(client);
}

void Server::queue_write(Client& client) {
    if (client.queued_for_write) return;
    client.queued_for_write = true;
    pending_writes_.push_back(client.fd);
}

bool Server::write_output(Client& client) {
    while (client.out_pos < client.out.size()) {
        const ssize_t n =
            ::write(client.fd, client.out.data() + client.out_pos, client.out.size() - client.out_pos);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            close_client(client);
            return false;
        }
        client.out_pos += static_cast<std::size_t>(n);
        net_output_bytes_ += static_cast<std::uint64_t>(n);
    }

    if (client.out_pos < client.out.size()) {
        if (client.out_pos > (1 << 20)) {  // reclaim the sent prefix of a large backlog
            client.out.erase(0, client.out_pos);
            client.out_pos = 0;
        }
        update_events(client, client.events | EPOLLOUT);
        return true;
    }

    client.out.clear();
    client.out_pos = 0;
    if (client.close_after_reply) {
        close_client(client);
        return false;
    }
    std::uint32_t events = client.events & ~static_cast<std::uint32_t>(EPOLLOUT);
    if (client.reading_paused) {
        client.reading_paused = false;
        events |= EPOLLIN;
        update_events(client, events);
        process_input(client);  // commands that arrived while paused
        return true;
    }
    update_events(client, events);
    return true;
}

void Server::handle_writable(Client& client) {
    // Replies of a write command must not reach the client before its record reaches the
    // append-only file, so even direct writes go through before_sleep ordering.
    queue_write(client);
}

void Server::update_events(Client& client, std::uint32_t events) {
    if (events == client.events) return;
    client.events = events;
    epoll_event event{};
    event.events = events;
    event.data.fd = client.fd;
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, client.fd, &event);
}

void Server::close_client(Client& client) {
    const int fd = client.fd;
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    ::close(fd);
    clients_.erase(fd);  // destroys client
}

void Server::before_sleep() {
    if (aof_) aof_->flush(now_ms());
    // Writing replies can resume paused clients, whose new replies are queued again.
    while (!pending_writes_.empty()) {
        std::vector<int> batch;
        batch.swap(pending_writes_);
        for (int fd : batch) {
            auto it = clients_.find(fd);
            if (it == clients_.end()) continue;
            Client& client = *it->second;
            client.queued_for_write = false;
            write_output(client);
        }
        if (aof_ && !pending_writes_.empty()) aof_->flush(now_ms());
    }
}

void Server::cron() {
    const std::int64_t now = now_ms();
    // Up to a quarter of each cron period goes to expiry, as in Redis.
    const auto budget = std::chrono::microseconds(250'000 / std::max(1, config_.hz));
    db_.active_expire(now, budget);
    db_.rehash_idle(std::chrono::microseconds(1000));
    if (aof_) {
        aof_->poll_rewrite();
        if (aof_->should_auto_rewrite(config_.auto_aof_rewrite_percentage,
                                      config_.auto_aof_rewrite_min_size)) {
            log::info("append-only file grew by " + std::to_string(config_.auto_aof_rewrite_percentage) +
                      "% since the last rewrite, starting a rewrite");
            aof_->start_rewrite(db_);
        }
    }
}

void Server::shutdown() {
    before_sleep();
    if (aof_) {
        aof_->sync_now();
        log::info("append-only file synced");
    }
    log::info("shutdown complete");
}

std::string Server::start_aof_rewrite() {
    if (!aof_) return "ERR append-only file is disabled";
    return aof_->start_rewrite(db_);
}

std::vector<std::pair<std::string, std::string>> Server::config(std::string_view pattern) {
    const std::vector<std::pair<std::string, std::string>> all = {
        {"appendfilename", config_.appendfilename},
        {"appendfsync", fsync_policy_name(config_.appendfsync)},
        {"appendonly", config_.appendonly ? "yes" : "no"},
        {"auto-aof-rewrite-min-size", std::to_string(config_.auto_aof_rewrite_min_size)},
        {"auto-aof-rewrite-percentage", std::to_string(config_.auto_aof_rewrite_percentage)},
        {"bind", config_.bind},
        {"databases", "1"},
        {"dir", config_.dir},
        {"hz", std::to_string(config_.hz)},
        {"maxclients", std::to_string(config_.maxclients)},
        {"port", std::to_string(config_.port)},
        {"proto-max-bulk-len", std::to_string(config_.limits.max_bulk_length)},
        {"save", ""},
    };
    std::string lowered(pattern);
    for (char& c : lowered) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::vector<std::pair<std::string, std::string>> matched;
    for (const auto& entry : all) {
        if (glob_match(lowered, entry.first)) matched.push_back(entry);
    }
    return matched;
}

std::string Server::info(std::string_view section) {
    const bool everything = section == "default" || section == "all" || section == "everything";
    std::ostringstream out;
    auto want = [&](std::string_view name) { return everything || section == name; };

    if (want("server")) {
        out << "# Server\r\n"
            << "kvstore_version:0.1.0\r\n"
            << "process_id:" << ::getpid() << "\r\n"
            << "tcp_port:" << config_.port << "\r\n"
            << "uptime_in_seconds:" << (now_ms() - started_ms_) / 1000 << "\r\n"
            << "hz:" << config_.hz << "\r\n\r\n";
    }
    if (want("clients")) {
        out << "# Clients\r\n"
            << "connected_clients:" << clients_.size() << "\r\n"
            << "maxclients:" << config_.maxclients << "\r\n\r\n";
    }
    if (want("persistence")) {
        out << "# Persistence\r\n"
            << "aof_enabled:" << (aof_ ? 1 : 0) << "\r\n";
        if (aof_) {
            out << "aof_rewrite_in_progress:" << (aof_->rewrite_in_progress() ? 1 : 0) << "\r\n"
                << "aof_rewrites:" << aof_->rewrites() << "\r\n"
                << "aof_last_bgrewrite_status:" << (aof_->last_rewrite_ok() ? "ok" : "err") << "\r\n"
                << "aof_last_write_status:" << (aof_->last_write_ok() ? "ok" : "err") << "\r\n"
                << "aof_current_size:" << aof_->current_size() << "\r\n"
                << "aof_base_size:" << aof_->base_size() << "\r\n"
                << "aof_fsync:" << fsync_policy_name(aof_->policy()) << "\r\n";
        }
        out << "\r\n";
    }
    if (want("stats")) {
        out << "# Stats\r\n"
            << "total_connections_received:" << total_connections_ << "\r\n"
            << "total_commands_processed:" << total_commands_ << "\r\n"
            << "total_net_input_bytes:" << net_input_bytes_ << "\r\n"
            << "total_net_output_bytes:" << net_output_bytes_ << "\r\n"
            << "rejected_connections:" << rejected_connections_ << "\r\n"
            << "expired_keys:" << db_.expired_total() << "\r\n\r\n";
    }
    if (want("keyspace")) {
        out << "# Keyspace\r\n";
        if (db_.size() > 0) out << "db0:keys=" << db_.size() << ",expires=" << db_.expires_size() << "\r\n";
    }
    return out.str();
}

}  // namespace kv
