#pragma once

// Append-only file persistence.
//
// Every write command is appended in RESP format. Replies are sent only after the
// commands of an event loop iteration have been written to the file (and, with the
// `always` policy, flushed to disk), so a client never sees a success for a write
// that a crash could lose under the configured policy.

#include <sys/types.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "commands.hpp"
#include "resp.hpp"

namespace kv {

enum class FsyncPolicy { always, everysec, no };

bool parse_fsync_policy(std::string_view name, FsyncPolicy& policy);
const char* fsync_policy_name(FsyncPolicy policy);

// One worker thread for blocking file operations. fsync on a busy disk can take tens or
// hundreds of milliseconds, and closing the last descriptor of a large replaced file
// frees its blocks synchronously; neither should stall the event loop.
class BackgroundJobs {
  public:
    BackgroundJobs();
    ~BackgroundJobs();
    BackgroundJobs(const BackgroundJobs&) = delete;
    BackgroundJobs& operator=(const BackgroundJobs&) = delete;

    void fsync(int fd);
    void close(int fd);
    std::size_t pending_fsyncs() const;
    void wait_idle();

  private:
    enum class Kind { fsync, close };
    struct Job {
        Kind kind;
        int fd;
    };
    void run();

    mutable std::mutex mutex_;
    std::condition_variable work_available_;
    std::condition_variable idle_;
    std::deque<Job> jobs_;
    std::size_t pending_fsyncs_ = 0;
    bool busy_ = false;
    bool stopping_ = false;
    std::thread worker_;
};

struct AofLoadResult {
    bool ok = true;
    std::size_t commands = 0;
    std::uint64_t valid_bytes = 0;
    bool truncated = false;
    std::string error;
};

class AppendOnlyFile {
  public:
    AppendOnlyFile(std::string path, FsyncPolicy policy, BackgroundJobs& jobs);
    ~AppendOnlyFile();
    AppendOnlyFile(const AppendOnlyFile&) = delete;
    AppendOnlyFile& operator=(const AppendOnlyFile&) = delete;

    // Replays the file into db. A command cut off at the end of the file (a crash during
    // a write) is discarded and the file truncated to the last complete command; any
    // other damage is reported as an error.
    static AofLoadResult load(const std::string& path, Database& db, const resp::Limits& limits);

    bool open(std::string& error);
    void append(const Args& command);

    // Writes buffered commands and applies the fsync policy. Called once per event loop
    // iteration, before replies are sent.
    void flush(std::int64_t now_ms);
    // Writes and fsyncs synchronously. Used at shutdown.
    void sync_now();

    // Compaction: a child process writes the current dataset as a minimal sequence of
    // commands while the parent keeps serving and buffers new writes; the parent then
    // appends that buffer and atomically replaces the old file.
    std::string start_rewrite(const Database& db);
    void poll_rewrite();
    bool rewrite_in_progress() const { return child_ > 0; }
    bool should_auto_rewrite(int growth_percent, std::uint64_t min_size) const;

    std::uint64_t current_size() const { return current_size_; }
    std::uint64_t base_size() const { return base_size_; }
    bool last_rewrite_ok() const { return last_rewrite_ok_; }
    bool last_write_ok() const { return last_write_ok_; }
    std::uint64_t rewrites() const { return rewrites_; }
    FsyncPolicy policy() const { return policy_; }

  private:
    static bool write_snapshot(const Database& db, int fd, std::int64_t now);
    void finish_rewrite(pid_t child);
    std::string temp_path(pid_t pid) const;

    std::string path_;
    FsyncPolicy policy_;
    BackgroundJobs& jobs_;
    int fd_ = -1;
    std::string buffer_;
    std::string rewrite_buffer_;
    std::uint64_t current_size_ = 0;
    std::uint64_t base_size_ = 0;
    std::int64_t last_fsync_ms_ = 0;
    pid_t child_ = -1;
    std::int64_t rewrite_started_ms_ = 0;
    bool last_rewrite_ok_ = true;
    bool last_write_ok_ = true;
    std::uint64_t rewrites_ = 0;
};

}  // namespace kv
