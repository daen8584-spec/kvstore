# 0006. Verify behaviour against Redis, under sanitizers and with fuzzing

- Status: Accepted
- Date: 2026-11-05

## Context

A server written in C++ that parses untrusted network input can fail through memory errors and undefined behaviour as well as logic bugs. Claiming Redis compatibility also needs evidence: the reference behaviour is Redis itself, not a reading of its documentation.

## Decision

- Unit tests cover the hash table (against a reference map, through growth and shrinking, and SCAN across resizes), the parser (every split point of a request, pipelining, limits, malformed input), glob matching, expiry and command semantics.
- Integration tests in Python start the real binary and speak RESP over TCP with a minimal client, so they control exactly what is sent. The protocol and command tests also run against `redis-server` (`KVSTORE_TARGET=redis`). A behaviour that differs from Redis therefore fails a test, rather than depending on interpretation.
- All tests also run with AddressSanitizer and UndefinedBehaviorSanitizer.
- A libFuzzer target feeds arbitrary bytes through the parser and every command, split across two reads, and checks parser invariants and that every command produces exactly one well-formed reply. CI fuzzes for two minutes on every push, and the seed corpus runs as a regression test with any compiler.
- CI builds with GCC and Clang with warnings as errors, and checks formatting.

## Consequences

- A change that breaks compatibility, memory safety or protocol handling fails CI before it is merged.
- Tests that depend on implementation details (file format, log messages, configuration flags) are marked `kvstore_only` and skipped against Redis.
