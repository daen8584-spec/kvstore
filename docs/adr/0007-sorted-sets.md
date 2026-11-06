# 0007. Implement sorted sets with a hash table and a ranked skip list

- Status: Accepted
- Date: 2026-11-05

## Context

Sorted sets hold unique members ordered by a floating-point score. They back leaderboards, priority queues, rate limiters and time-ordered indexes, and their commands need three kinds of access: by member (`ZSCORE`, and finding a member's old score on update), by score range (`ZRANGEBYSCORE`, `ZCOUNT`), and by rank (`ZRANK`, `ZRANGE` by index).

A balanced tree gives ordered access but not ranks without augmenting every node with subtree sizes, and is intricate to keep balanced. A sorted array gives ranks for free but O(n) inserts.

## Decision

- Store each sorted set as a hash table from member to score plus a skip list of (score, member) pairs, as Redis does. Ties in score are ordered by the member's bytes.
- Record on every skip list link the number of nodes it skips (its span). Summing spans along a search path gives a member's rank, and following spans finds the member at a rank, both in O(log n). `ZCOUNT` subtracts the ranks of the first and last members in range instead of walking the range.
- Promote a node to each next level with probability 1/4, up to 32 levels, as Redis does.
- Allocate a node and its links as one block, and let the node refer to the member string owned by the hash table entry, which keeps its address until it is erased. Each member is stored once.
- Change a score in place when the new score keeps the node between its neighbours, and otherwise unlink and relink the same node without reallocating.
- Look a member up in the hash table once per `ZADD` element, deciding the new score from the current one in the same lookup.
- Format scores with the shortest representation that parses back to the same value (`0.1`, `1`, `inf`), as Redis 7.2 and later do.

## Consequences

- Every sorted set command runs in O(log n) per element touched, with O(1) score lookups.
- The first implementation stored members twice, used two allocations per node and two hash lookups per update, and reached 0.70 of Redis's `ZADD` throughput (0.55 with pipelining). The last three decisions above brought it to 0.89 and 0.90 on the same benchmark.
- Redis encodes small sorted sets as compact listpacks to save memory. kvstore always uses the full structure, so small sorted sets use more memory than in Redis.
- `ZRANGE ... BYLEX` and the lexicographic commands are not implemented.
