---
id: 016
title: FileOfflineQueue keyed enqueue is a linear scan (no depth bound)
subsystem: offline
severity: minor
source: examples/LADDER.md; include/morph/offline/file_offline_queue.hpp:105
disposition: documented-limitation
test: spec-cited
---

`FileOfflineQueue` performs keyed `enqueue()` (idempotency-key deduplication) as a linear scan over pending items — O(n) per call. This is intentional and documented in `docs/spec/offline/offline.md:215-216` as acceptable for modest queue depths, with `SqliteOfflineQueue` provided as an index-backed alternative for high-volume keyed enqueues.

**Scope.** The reference NDJSON implementation (`FileOfflineQueue`) is by design simple and dependency-free; it targets use cases where queue depth stays bounded (tens of items, not thousands). Apps requiring high-concurrency dedup should use `SqliteOfflineQueue` instead, whose foreign-key dedup is index-backed and scales.
