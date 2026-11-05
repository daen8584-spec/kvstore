# 0001. Record architecture decisions

- Status: Accepted
- Date: 2026-11-05

## Context

A storage server makes trade-offs between latency, throughput, durability and simplicity that are not visible in the code. Without a record, the reasons behind a choice are lost.

## Decision

Significant decisions are recorded in `docs/adr` with their context, the decision and its consequences. An accepted record is not rewritten; a later record supersedes it.

## Consequences

A change in assumptions, such as a need for multiple cores or stronger durability, points directly to the decisions that should be revisited.
