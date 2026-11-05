# 0003. Resize the hash table incrementally and hash keys with SipHash

- Status: Accepted
- Date: 2026-11-05

## Context

`std::unordered_map` rehashes every element in one call when it grows. With millions of keys that call takes long enough to stall the event loop, and every client sees the stall as a latency spike. A hash table keyed by client-chosen strings is also exposed to hash flooding: keys crafted to collide turn constant-time operations into linear scans.

## Decision

- Implement a chained hash table (`src/dict.hpp`) with two bucket arrays. When the load factor reaches 1 the table allocates an array of twice the size and moves one bucket on every lookup, insert and delete, plus batches during idle time from the server's periodic task. Lookups check both arrays while a resize is in progress. Tables shrink when they fall below one-eighth full.
- Provide cursor-based iteration (`SCAN`) using Redis's reverse-binary cursor. Because the cursor increments in bit-reversed order, buckets already visited map onto visited buckets in a table of any other power-of-two size, so every key present for the whole iteration is returned even if the table resizes between calls.
- Hash keys with SipHash-1-3 keyed with 128 random bits per process, as Redis does. An attacker who does not know the key cannot predict collisions.

## Consequences

- The cost of a resize is spread over many requests, and no single request pays for moving the whole table.
- During a resize both arrays are allocated, so memory peaks at about 1.5 times the steady state for the bucket arrays.
- `SCAN` can return a key more than once, which callers must tolerate, as with Redis.
- The table and its iteration guarantee are tested against a reference map through growth and shrinking, and with resizes between `SCAN` calls.
