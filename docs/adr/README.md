# Architecture Decision Records

Each record describes a significant design decision, its context and its consequences.

| Number | Decision |
| --- | --- |
| [0001](0001-record-architecture-decisions.md) | Record architecture decisions |
| [0002](0002-single-threaded-event-loop.md) | Serve all clients from one epoll event loop |
| [0003](0003-incremental-rehashing.md) | Resize the hash table incrementally and hash keys with SipHash |
| [0004](0004-expiry.md) | Store absolute expiry times and expire lazily and by sampling |
| [0005](0005-append-only-file.md) | Persist with an append-only file and compact it in a forked child |
| [0006](0006-verification.md) | Verify behaviour against Redis, under sanitizers and with fuzzing |
| [0007](0007-sorted-sets.md) | Implement sorted sets with a hash table and a ranked skip list |
