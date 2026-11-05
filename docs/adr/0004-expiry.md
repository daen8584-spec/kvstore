# 0004. Store absolute expiry times and expire lazily and by sampling

- Status: Accepted
- Date: 2026-11-05

## Context

Keys with a time to live must stop being visible when they expire, and their memory must eventually be reclaimed even if nobody reads them again. Scanning every key with a TTL on a timer costs time proportional to the number of such keys, which stalls the event loop.

## Decision

- Expiry times are stored as absolute Unix milliseconds in a second hash table that holds only keys with a TTL.
- Every access checks the expiry and deletes an expired key before using it (lazy expiry), so an expired key is never visible.
- The periodic task samples 20 keys from the expiry table, deletes the expired ones, and repeats while more than a quarter of a sample was expired, within a quarter of the task period (active expiry). This is Redis's algorithm: the fraction of expired keys left in memory stays bounded at roughly a quarter, at a cost that adapts to how many keys are expiring.
- Relative forms are recorded in the append-only file as absolute times: `SET ... EX` becomes `SET ... PXAT`, and `EXPIRE` becomes `PEXPIREAT`, or `DEL` if the time is already past.

## Consequences

- Replaying the append-only file after a restart, at any later time, gives every key the same expiry it had, and keys that expired while the server was down are removed on load or first access.
- Expiry depends on the wall clock. Moving the system clock forward expires keys early, as with Redis.
- Memory held by expired keys that are never read is reclaimed gradually rather than instantly.
