---
id: 007
title: The three shipped IOfflineQueue implementations disagree about what a repeated idempotencyKey means, and the spec says they agree
subsystem: offline
severity: major
source: lims rung 6, build order §7
disposition: open
test: examples/lims/tests/test_offline_capture.cpp (case "The durable queue dedups a re-enqueued operation where the in-memory one does not", tag [sqlite])
---

`IOfflineQueue`'s documented contract is that the queue does **not** dedup:

> The queue **stores the key verbatim and never interprets, requires, or
> enforces uniqueness on it** — enforcement is the replay consumer's job.
> — `docs/spec/offline/offline.md`, "`idempotencyKey`: deduping against the
> journal" (the same sentence appears on `IOfflineQueue::setIdempotencyKey`
> in `include/morph/offline/offline_queue.hpp`)

Two of the three shipped implementations enforce it anyway, and the spec's own
`SqliteOfflineQueue` section asserts a parity that does not exist:

> The partial unique index gives insert-time dedup for a non-empty
> `idempotencyKey` — a re-enqueue of the same key is a no-op that returns the
> existing row's id; empty keys are exempt and are never deduplicated,
> **matching `InMemoryOfflineQueue`**.

`InMemoryOfflineQueue` does not dedup a *non-empty* key at all — its
`enqueue(payload, key)` unconditionally `push_back`s (`offline_queue.hpp`).
`FileOfflineQueue` does ("A keyed `enqueue`'s dedup is a linear scan over
pending items").

## Repro

```cpp
#include <morph/offline/offline_queue.hpp>
#include <morph/offline/file_offline_queue.hpp>
#include <morph/offline/sqlite_offline_queue.hpp>
#include <cstdio>

template <class Q> void probe(const char* name, Q& q) {
    const auto a = q.enqueue("{\"n\":1}", "op-42");
    const auto b = q.enqueue("{\"n\":1}", "op-42");
    std::printf("%-22s first id=%llu second id=%llu  size=%zu\n", name,
                (unsigned long long)a, (unsigned long long)b, q.size());
}

int main() {
    morph::offline::InMemoryOfflineQueue mem;   probe("InMemoryOfflineQueue", mem);
    morph::offline::FileOfflineQueue file{"q.ndjson"}; probe("FileOfflineQueue", file);
    morph::offline::SqliteOfflineQueue sql{"q.sqlite"}; probe("SqliteOfflineQueue", sql);
}
```

Built with Homebrew clang 22.1.8, `-std=c++23`:

```
InMemoryOfflineQueue   first id=1 second id=2  size=2
FileOfflineQueue       first id=1 second id=1  size=1
SqliteOfflineQueue     first id=1 second id=1  size=1
```

## Why it matters

The queue implementation is a deployment choice, not an application-visible
one — `IOfflineQueue` exists precisely so a host can swap it. An application
developed and tested against `InMemoryOfflineQueue` (which is what
`docs/spec/offline/offline.md` recommends for testing) and shipped on
`SqliteOfflineQueue` gets a different queue depth for the same sequence of
calls, and the difference is invisible until a replay produces a different
number of writes than the test suite ever saw.

The divergence is *silent* in both directions. A consumer that relies on the
interface contract and dedups itself is correct everywhere but does redundant
work on two of three queues. A consumer that relies on the queue deduping is
correct on two of three and double-applies on the third.

## What should happen

Pick one and make all three do it:

1. **Enforce nowhere** (what the interface contract says today) — drop the
   partial unique index from `SqliteOfflineQueue` and the linear scan from
   `FileOfflineQueue`, and correct their spec sections. Consumers dedup.
2. **Enforce everywhere** — add the dedup to `InMemoryOfflineQueue` and
   rewrite the `IOfflineQueue` contract sentence to promise it. This is the
   smaller code change and matches what two of three already do.

Either way, the "matching `InMemoryOfflineQueue`" clause in the
`SqliteOfflineQueue` section is currently false and should be corrected even
if the behavior is left alone.

`lims` works on any of the three: `SampleModel::execute(QueuedCapture)`
enforces at-most-once itself against a durable `lims_replayed_ops` table,
which is what the interface contract asks the consumer to do.
