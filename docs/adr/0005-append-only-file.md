# 0005. Persist with an append-only file and compact it in a forked child

- Status: Accepted
- Date: 2026-11-05

## Context

An in-memory store loses everything on restart unless it writes changes to disk. The persistence mechanism must let operators choose between durability and throughput, must not block request processing on slow disks, and must keep the file from growing without bound when the same keys are updated repeatedly.

## Decision

- Write commands that changed the dataset are appended to the file in RESP format, as recorded by the command implementations ([0004](0004-expiry.md)). Commands that changed nothing are not recorded.
- Commands are buffered during a loop iteration and written once before replies are sent ([0002](0002-single-threaded-event-loop.md)). The `appendfsync` policy then decides when data reaches the disk:
  - `always` calls `fdatasync` before replying, so an acknowledged write survives a power failure.
  - `everysec` schedules `fdatasync` at most once per second on a worker thread, so a power failure loses at most about a second of writes, and a slow disk never blocks the event loop on a sync.
  - `no` leaves flushing to the operating system.
- When writing to the file fails, for example because the disk is full, the commands stay buffered and the write is retried on every loop iteration. A write that failed part way is cut back to the last complete command first, so the retry cannot duplicate it. Clients whose writes are not yet in the file get no reply until the retry succeeds; other clients are served, but every write command is refused with a `MISCONF` error, as in Redis, so the buffer cannot grow while the disk is full. Under `everysec`, a failed background `fdatasync` also refuses writes until a later sync succeeds. Under `always`, a failed `fdatasync` stops the server without replying, as Redis does, because the writes cannot be acknowledged and it is unknown which of them reached the disk.
- A rewrite only starts when the buffer has been written, and when it installs the new file, commands still buffered for the old file are dropped from the buffer because the new file already holds them through the rewrite buffer. Neither file receives a command twice.
- On startup the file is replayed through the normal command implementations. A command cut off at the end of the file, the signature of a crash during a write, is discarded and the file is truncated to the last complete command. Damage anywhere else stops the server with the byte offset, because silently skipping data would hide loss.
- Compaction (`BGREWRITEAOF`, or automatically when the file has doubled since the last rewrite and exceeds 64 MiB) forks a child. The child writes the current dataset as a minimal sequence of commands from its copy-on-write snapshot. Meanwhile the parent keeps serving and records new writes both in the old file and in a rewrite buffer. When the child succeeds, the parent appends the buffer to the new file, syncs it, renames it over the old one and syncs the directory, so a crash at any point leaves either the complete old file or the complete new one. The old descriptor is closed on the worker thread, because freeing a large file's blocks can take a long time.

## Consequences

- Durability is a configuration choice with documented loss bounds.
- `fork` copies page tables, and pages the parent modifies during a rewrite are duplicated. Memory use can rise by up to the size of the dataset during a rewrite under a write-heavy load.
- Loading time is proportional to the file size. A point-in-time snapshot format would load faster but is not implemented.
- These properties are tested: restart after shutdown, recovery after `SIGKILL` with `always`, truncated tails, corruption in the middle of the file, writes made during a rewrite, automatic rewrites, and a write that fails part way because of a file size limit, which must not be acknowledged, must refuse further writes, and must be acknowledged and loaded once after the limit is lifted.
