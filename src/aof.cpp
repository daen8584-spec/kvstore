#include "aof.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>

#include "log.hpp"

namespace kv {
namespace {

bool write_all(int fd, std::string_view data) {
    while (!data.empty()) {
        const ssize_t n = ::write(fd, data.data(), data.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        data.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
}

std::string errno_text() {
    return std::strerror(errno);
}

std::string directory_of(const std::string& path) {
    const auto slash = path.rfind('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

// Makes a rename durable: the new directory entry is only persistent once the directory
// itself is flushed.
void fsync_directory(const std::string& path) {
    const int dir = ::open(directory_of(path).c_str(), O_RDONLY | O_CLOEXEC);
    if (dir < 0) return;
    ::fsync(dir);
    ::close(dir);
}

}  // namespace

bool parse_fsync_policy(std::string_view name, FsyncPolicy& policy) {
    if (name == "always")
        policy = FsyncPolicy::always;
    else if (name == "everysec")
        policy = FsyncPolicy::everysec;
    else if (name == "no")
        policy = FsyncPolicy::no;
    else
        return false;
    return true;
}

const char* fsync_policy_name(FsyncPolicy policy) {
    switch (policy) {
        case FsyncPolicy::always:
            return "always";
        case FsyncPolicy::everysec:
            return "everysec";
        case FsyncPolicy::no:
            return "no";
    }
    return "everysec";
}

// ---- background jobs ----

BackgroundJobs::BackgroundJobs() : worker_([this] { run(); }) {}

BackgroundJobs::~BackgroundJobs() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    work_available_.notify_one();
    worker_.join();
}

void BackgroundJobs::fsync(int fd) {
    {
        std::lock_guard lock(mutex_);
        jobs_.push_back({Kind::fsync, fd});
        ++pending_fsyncs_;
    }
    work_available_.notify_one();
}

void BackgroundJobs::close(int fd) {
    {
        std::lock_guard lock(mutex_);
        jobs_.push_back({Kind::close, fd});
    }
    work_available_.notify_one();
}

std::size_t BackgroundJobs::pending_fsyncs() const {
    std::lock_guard lock(mutex_);
    return pending_fsyncs_;
}

void BackgroundJobs::wait_idle() {
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [this] { return jobs_.empty() && !busy_; });
}

void BackgroundJobs::run() {
    std::unique_lock lock(mutex_);
    for (;;) {
        work_available_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
        if (jobs_.empty()) return;  // stopping, and all jobs are done
        const Job job = jobs_.front();
        jobs_.pop_front();
        busy_ = true;
        lock.unlock();
        if (job.kind == Kind::fsync) {
            ::fdatasync(job.fd);
        } else {
            ::close(job.fd);
        }
        lock.lock();
        busy_ = false;
        if (job.kind == Kind::fsync) --pending_fsyncs_;
        if (jobs_.empty()) idle_.notify_all();
    }
}

// ---- append-only file ----

AppendOnlyFile::AppendOnlyFile(std::string path, FsyncPolicy policy, BackgroundJobs& jobs)
    : path_(std::move(path)), policy_(policy), jobs_(jobs) {}

AppendOnlyFile::~AppendOnlyFile() {
    if (child_ > 0) {
        ::kill(child_, SIGKILL);
        ::waitpid(child_, nullptr, 0);
        ::unlink(temp_path(child_).c_str());
    }
    if (fd_ >= 0) {
        jobs_.wait_idle();
        ::close(fd_);
    }
}

AofLoadResult AppendOnlyFile::load(const std::string& path, Database& db, const resp::Limits& limits) {
    AofLoadResult result;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno != ENOENT) {
            result.ok = false;
            result.error = "cannot open " + path + ": " + errno_text();
        }
        return result;
    }

    resp::RequestParser parser(limits);
    std::string buffer;
    std::size_t offset = 0;
    std::uint64_t consumed_before_buffer = 0;  // file bytes discarded from the front of buffer
    Args args;
    std::string scratch;
    char chunk[1 << 16];
    bool eof = false;

    while (!eof || offset < buffer.size()) {
        if (!eof) {
            const ssize_t n = ::read(fd, chunk, sizeof chunk);
            if (n < 0) {
                if (errno == EINTR) continue;
                result.ok = false;
                result.error = "read error in " + path + ": " + errno_text();
                ::close(fd);
                return result;
            }
            if (n == 0)
                eof = true;
            else
                buffer.append(chunk, static_cast<std::size_t>(n));
        }

        for (;;) {
            const resp::ParseStatus status = parser.parse(buffer, offset, args);
            if (status == resp::ParseStatus::incomplete) break;
            if (status == resp::ParseStatus::error) {
                result.ok = false;
                result.error = "corrupt append-only file at byte " + std::to_string(result.valid_bytes) +
                               ": " + parser.error();
                ::close(fd);
                return result;
            }
            if (!args.empty()) {
                scratch.clear();
                ExecContext ctx{db, scratch, now_ms()};
                execute(ctx, args);
                if (!scratch.empty() && scratch.front() == '-') {
                    result.ok = false;
                    result.error =
                        "command at byte " + std::to_string(result.valid_bytes) +
                        " of the append-only file failed: " + scratch.substr(1, scratch.size() - 3);
                    ::close(fd);
                    return result;
                }
                ++result.commands;
            }
            result.valid_bytes = consumed_before_buffer + offset;
        }

        if (eof && offset < buffer.size()) {
            // The rest of the file does not form a complete command.
            result.truncated = true;
            break;
        }
        // Discard what has been parsed. Partially parsed bulk headers are already part of
        // the parser state, so the consumed bytes are no longer needed.
        if (offset > 0) {
            consumed_before_buffer += offset;
            buffer.erase(0, offset);
            offset = 0;
        }
        if (eof) break;
    }
    if (parser.in_progress()) result.truncated = true;
    ::close(fd);

    if (result.truncated) {
        struct stat info {};
        const auto size = ::stat(path.c_str(), &info) == 0 ? static_cast<std::uint64_t>(info.st_size) : 0;
        log::warning("append-only file ends with an incomplete command; truncating from " +
                     std::to_string(size) + " to " + std::to_string(result.valid_bytes) + " bytes");
        if (::truncate(path.c_str(), static_cast<off_t>(result.valid_bytes)) != 0) {
            result.ok = false;
            result.error = "cannot truncate " + path + ": " + errno_text();
        }
    }
    return result;
}

bool AppendOnlyFile::open(std::string& error) {
    fd_ = ::open(path_.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd_ < 0) {
        error = "cannot open " + path_ + ": " + errno_text();
        return false;
    }
    struct stat info {};
    if (::fstat(fd_, &info) == 0) current_size_ = base_size_ = static_cast<std::uint64_t>(info.st_size);
    last_fsync_ms_ = now_ms();
    return true;
}

void AppendOnlyFile::append(const Args& command) {
    resp::command(buffer_, command);
    if (child_ > 0) resp::command(rewrite_buffer_, command);
}

void AppendOnlyFile::flush(std::int64_t now) {
    if (fd_ < 0) return;
    if (!buffer_.empty()) {
        if (!write_all(fd_, buffer_)) {
            // Keep the data and retry on the next iteration (for example after ENOSPC).
            if (last_write_ok_) log::error("cannot write to the append-only file: " + errno_text());
            last_write_ok_ = false;
            return;
        }
        if (!last_write_ok_) log::info("writes to the append-only file succeed again");
        last_write_ok_ = true;
        current_size_ += buffer_.size();
        buffer_.clear();
        if (policy_ == FsyncPolicy::always) {
            ::fdatasync(fd_);
            last_fsync_ms_ = now;
            return;
        }
    }
    if (policy_ == FsyncPolicy::everysec && now - last_fsync_ms_ >= 1000 && jobs_.pending_fsyncs() == 0) {
        jobs_.fsync(fd_);
        last_fsync_ms_ = now;
    }
}

void AppendOnlyFile::sync_now() {
    if (fd_ < 0) return;
    if (!buffer_.empty() && write_all(fd_, buffer_)) {
        current_size_ += buffer_.size();
        buffer_.clear();
    }
    jobs_.wait_idle();
    ::fsync(fd_);
}

bool AppendOnlyFile::should_auto_rewrite(int growth_percent, std::uint64_t min_size) const {
    if (fd_ < 0 || child_ > 0 || growth_percent <= 0 || current_size_ < min_size) return false;
    const std::uint64_t base = base_size_ == 0 ? 1 : base_size_;
    return (current_size_ - base_size_) * 100 / base >= static_cast<std::uint64_t>(growth_percent);
}

std::string AppendOnlyFile::temp_path(pid_t pid) const {
    return directory_of(path_) + "/temp-rewrite-" + std::to_string(pid) + ".aof";
}

bool AppendOnlyFile::write_snapshot(const Database& db, int fd, std::int64_t now) {
    constexpr std::size_t kBatch = 64;
    constexpr std::size_t kFlushAt = 1 << 20;
    std::string out;
    out.reserve(kFlushAt + 4096);
    bool ok = true;
    Args command;

    auto emit = [&](const Args& c) {
        resp::command(out, c);
        if (out.size() >= kFlushAt) {
            ok = ok && write_all(fd, out);
            out.clear();
        }
    };

    db.keys().for_each([&](const std::string& key, const Value& value) {
        if (!ok) return;
        const auto when = db.expire_time(key);
        if (when && *when <= now) return;
        if (const auto* text = std::get_if<std::string>(&value)) {
            emit({"SET", key, *text});
        } else if (const auto* list = std::get_if<List>(&value)) {
            command = {"RPUSH", key};
            for (const std::string& item : *list) {
                command.push_back(item);
                if (command.size() == kBatch + 2) {
                    emit(command);
                    command.resize(2);
                }
            }
            if (command.size() > 2) emit(command);
        } else if (const auto* hash = std::get_if<Hash>(&value)) {
            command = {"HSET", key};
            hash->for_each([&](const std::string& field, const std::string& v) {
                command.push_back(field);
                command.push_back(v);
                if (command.size() == 2 * kBatch + 2) {
                    emit(command);
                    command.resize(2);
                }
            });
            if (command.size() > 2) emit(command);
        } else if (const auto* zset = std::get_if<ZSet>(&value)) {
            command = {"ZADD", key};
            for (const SkipList::Node* node = zset->ordered().first(); node; node = node->next()) {
                command.push_back(format_score(node->score));
                command.emplace_back(node->member);
                if (command.size() == 2 * kBatch + 2) {
                    emit(command);
                    command.resize(2);
                }
            }
            if (command.size() > 2) emit(command);
        }
        if (when) emit({"PEXPIREAT", key, std::to_string(*when)});
    });
    ok = ok && write_all(fd, out);
    return ok && ::fsync(fd) == 0;
}

std::string AppendOnlyFile::start_rewrite(const Database& db) {
    if (fd_ < 0) return "ERR append-only file is disabled";
    if (child_ > 0) return "ERR Background append only file rewriting already in progress";

    // Buffered commands describe state already in memory, so they belong to the
    // snapshot; write them out now so the old file stays complete too.
    flush(now_ms());
    rewrite_buffer_.clear();

    const pid_t pid = ::fork();
    if (pid < 0) return "ERR cannot fork: " + errno_text();
    if (pid == 0) {
        // Child: the dataset is a copy-on-write snapshot of the parent's memory at fork.
        const std::string temp = temp_path(::getpid());
        const int out = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        const bool ok = out >= 0 && write_snapshot(db, out, now_ms());
        if (out >= 0) ::close(out);
        ::_exit(ok ? 0 : 1);
    }
    child_ = pid;
    rewrite_started_ms_ = now_ms();
    log::info("append-only file rewrite started by child process " + std::to_string(pid));
    return {};
}

void AppendOnlyFile::poll_rewrite() {
    if (child_ <= 0) return;
    int status = 0;
    const pid_t done = ::waitpid(child_, &status, WNOHANG);
    if (done == 0) return;
    const pid_t pid = child_;
    child_ = -1;
    if (done < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        log::error("append-only file rewrite failed in child process " + std::to_string(pid));
        ::unlink(temp_path(pid).c_str());
        rewrite_buffer_.clear();
        last_rewrite_ok_ = false;
        return;
    }
    finish_rewrite(pid);
}

void AppendOnlyFile::finish_rewrite(pid_t child) {
    const std::string temp = temp_path(child);
    const int fd = ::open(temp.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
    // Commands buffered since the fork are not in the snapshot; append them so the new
    // file reflects the current dataset before it replaces the old one.
    flush(now_ms());
    const bool ok = fd >= 0 && write_all(fd, rewrite_buffer_) && ::fsync(fd) == 0 &&
                    ::rename(temp.c_str(), path_.c_str()) == 0;
    if (!ok) {
        log::error("cannot install the rewritten append-only file: " + errno_text());
        if (fd >= 0) ::close(fd);
        ::unlink(temp.c_str());
        rewrite_buffer_.clear();
        last_rewrite_ok_ = false;
        return;
    }
    fsync_directory(path_);

    struct stat info {};
    ::fstat(fd, &info);
    const int old = fd_;
    fd_ = fd;
    jobs_.close(old);
    current_size_ = base_size_ = static_cast<std::uint64_t>(info.st_size);
    rewrite_buffer_.clear();
    last_rewrite_ok_ = true;
    ++rewrites_;
    log::info("append-only file rewritten: " + std::to_string(base_size_) + " bytes in " +
              std::to_string(now_ms() - rewrite_started_ms_) + " ms");
}

}  // namespace kv
