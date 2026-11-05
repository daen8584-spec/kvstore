# 0002. Serve all clients from one epoll event loop

- Status: Accepted
- Date: 2026-11-05

## Context

An in-memory store spends most of each request on system calls and memory access, not computation. A thread per connection adds context switches and requires locking the keyspace for every command, and a thread pool sharing one keyspace needs fine-grained locking that makes every operation, including multi-key commands and expiry, harder to get right.

## Decision

- One thread runs a level-triggered `epoll` loop that owns every connection and the whole dataset. Commands execute one at a time, so each command is atomic without locks.
- Requests are read in chunks of up to 1 MiB per readiness event, so one fast writer cannot starve other clients. Every complete command in the buffer is executed, which makes pipelining a matter of batching system calls.
- Replies are not written while commands execute. They are collected and written at the end of the loop iteration, after the iteration's write commands have been appended to the append-only file ([0005](0005-append-only-file.md)). A client therefore never receives a success for a write that has not reached the file.
- When a client's unsent replies exceed 4 MiB, the server stops reading from it until it drains. TCP flow control then slows the client down, instead of the server buffering replies without bound.
- Periodic work (active expiry, idle rehashing, rewrite completion) runs on a `timerfd` at `hz` times per second. `SIGINT` and `SIGTERM` arrive through a `signalfd` and trigger a clean shutdown that flushes and syncs the append-only file.
- Blocking file operations run elsewhere: `fsync` and closing replaced files on a worker thread, and compaction in a forked child.

## Consequences

- Throughput is bounded by one core. On the reference machine it is 0.92 to 1.18 times that of Redis, which uses the same model, without pipelining.
- A slow command (for example `KEYS` on a large keyspace) delays every client. `SCAN` exists for incremental iteration.
- Linux-specific system calls (`epoll`, `timerfd`, `signalfd`, `accept4`) limit portability to Linux and Android. A kqueue backend would be needed for BSD and macOS.
