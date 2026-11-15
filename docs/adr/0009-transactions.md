# 0009. Run transactions in the event loop, track WATCH with key positions, and filter lookups

- Status: Accepted
- Date: 2026-11-15

## Context

`MULTI` and `EXEC` let a client run several commands with no other client's command in between. `WATCH` adds optimistic locking: `EXEC` fails with a null reply if a watched key changed after it was watched. Clients use this for check-and-set without server-side locks.

Three questions follow:

1. How to run a transaction atomically.
2. How to know which keys a write changed, so watched keys can be invalidated, and how much that costs every write.
3. How to keep the append-only file consistent when a crash happens in the middle of a transaction.

The behaviour to match is Redis's. It was recorded from Redis 7.0.15 before implementation, and it includes a few rules that are easy to get wrong:

- Errors while queueing (an unknown command, wrong arity) abort the transaction with `EXECABORT`. Errors at run time (`WRONGTYPE`) do not: the other commands still run, and there is no rollback.
- Nested `MULTI` and `WATCH` inside `MULTI` reply with an error but do not abort the transaction. `QUIT` runs immediately and discards it. `UNWATCH` is queued.
- A watched key is invalidated only by a write that changes it. `SADD` of an existing member or `DEL` of a missing key does not count. `SET` to the same value does.
- The `STORE` commands invalidate only their destination, not their sources.
- A watched key that expires before `EXEC` makes it fail.
- `FLUSHALL` invalidates only watched keys that existed.
- `EXEC` and `DISCARD` remove all of the client's watches, whether or not the transaction ran.

## Decision

- **Atomicity from the event loop.** The server runs one command at a time ([0002](0002-single-threaded-event-loop.md)), so `EXEC` runs the queued commands one after another with no locks.
- **Transaction state in a `Session`**, passed to commands through the execution context. The server keeps one per client. The append-only file loader and the fuzz target each keep one too, so all three exercise the same code.
- **Key positions in the command table.** Each write command declares which arguments are keys it modifies (`first_key`, `last_key`, `key_step`, as in Redis's command table). After a write that changed the dataset, every such key is "touched", marking its watchers. Commands whose handler reported no change touch nothing, which gives the Redis rules above without per-command code. `FLUSHALL` touches every watched key that exists, before it runs.
- **Expiry counts as a change.** Each watch records whether the key existed. `EXEC` fails if such a key no longer exists, whatever removed it.
- **Logged between `MULTI` and `EXEC`.** A transaction's writes are appended to the file as one block. Read-only transactions are not logged. The loader queues commands after `MULTI` and applies them at `EXEC`. If the file ends inside a block, it is truncated to where the block started, so a crash during a transaction leaves either all of its writes or none.
- **A filter in front of the watch registry.** With no watched keys, a write pays one emptiness check. Otherwise a key's hash selects a bit in a 2^18-bit (32 KiB) array, and only keys whose bit is set are looked up in the map of watched keys. Counters per bit, read only by `WATCH` and `UNWATCH`, decide when a bit can be cleared.

## How the filter was arrived at

Measured with `redis-benchmark -t set -P 16 -r 100000`, the server on one CPU and the client on three, with 10 idle clients watching 1,000 keys each from the benchmark's key range. Both cases ran in the same server process, alternating, median of 7 runs.

| Lookup on every write | SET throughput with 10,000 watched keys, relative to none |
| --- | ---: |
| `std::unordered_map`, key copied to a `std::string` | 0.81 to 0.86 |
| Same map, key looked up without a copy | 0.81 to 0.85 |
| 2^16 32-bit counters in front of the map (256 KiB) | 0.85 |
| 2^18-bit array in front of the map (32 KiB), counters kept separately | 0.89 to 0.97 (median 0.94) |
| No lookup (a diagnostic build, not correct) | 0.96 to 1.08 |

Redis 7.0.15 showed no drop in a single comparison run on the same setup, which was not repeated.

Timed in isolation, a lookup in the map cost 42 ns. Inside the server it averaged 266 ns, measured with an instrumented build, including about 40 ns for the timing itself. The rest of the server's memory traffic evicts the map, and the first filter, from the CPU cache, so each lookup waits on main memory. The 32 KiB bit array is read on every write and stays cached. The remaining cost comes mostly from the 10% of writes in this benchmark that really hit a watched key and have to look up the map.

Writing the tests also exposed a quadratic `WATCH`: checking for a duplicate by scanning the session's own watched keys made a unit test that watches 150,000 keys run for more than ten minutes under the sanitizers before it was stopped. The check now searches the key's watchers instead, which are few.

## Consequences

- Transactions need no locks and no copy of the dataset.
- Adding a write command requires declaring its key positions. The default (the first argument) is right for most commands; `MSET`, `DEL`, `UNLINK` and `SMOVE` declare theirs, and `FLUSHALL` is handled separately.
- A crash during a transaction can no longer leave half of it in the dataset after a restart.
- With watched keys, writes pay a hash of the key and one cached bit read. About 6% of pipelined `SET` throughput was lost with 10,000 watched keys in the benchmark above, within its run-to-run variation.
- There is no rollback of commands that fail at run time, as in Redis.
