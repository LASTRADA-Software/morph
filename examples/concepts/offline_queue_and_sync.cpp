// SPDX-License-Identifier: Apache-2.0
//
// Concept: the offline queue (morph::offline) — durable storage for actions
// that could not be delivered while the app was offline, plus SyncWorker,
// which replays them on reconnect.
//
// Three small demonstrations:
//   1. The basic IOfflineQueue lifecycle: enqueue while offline, drain to see
//      what's pending, markDone once a replay actually succeeds.
//   2. The NEW retry-budget + dead-letter story: SyncWorker retries a failing
//      item a bounded number of times (QueueItem::attempts), then hands it to
//      an optional DeadLetterSink instead of dropping it silently forever.
//   3. The NEW FileOfflineQueue: unlike InMemoryOfflineQueue, an enqueued item
//      survives destroying and reconstructing the queue object over the same
//      file path — the durability an in-memory queue cannot give you.
//
// Full design reference: docs/spec/offline/offline.md.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <morph/offline/file_offline_queue.hpp>
#include <morph/offline/offline_queue.hpp>
#include <morph/offline/sync_worker.hpp>
#include <string>
#include <vector>

using morph::offline::FileOfflineQueue;
using morph::offline::InMemoryOfflineQueue;
using morph::offline::QueueItem;
using morph::offline::SyncWorker;

// ── 1. Basic enqueue -> drain -> markDone lifecycle ─────────────────────────
//
// Reach for this whenever an action can't be sent right now (no network, a
// backend call failed) but must not be lost: park it in the queue instead of
// discarding it, and replay it later once connectivity is back. In a real
// client, enqueue() is called from wherever a `Bridge`/`BridgeHandler` send
// attempt fails (a caught connection error, not shown here) rather than
// standing alone as it does in this example.

TEST_CASE("offline queue: enqueue -> drain -> markDone", "[concepts][offline]") {
    InMemoryOfflineQueue queue;

    auto id = queue.enqueue(R"({"action":"Deposit","amount":10})");

    // drain() is read-only: the item survives being inspected, so a crash
    // between drain() and markDone() never loses it.
    auto pending = queue.drain();
    REQUIRE(pending.size() == 1);
    REQUIRE(pending[0].id == id);
    REQUIRE(pending[0].payload == R"({"action":"Deposit","amount":10})");

    // Only once the replay has actually succeeded does the item leave the queue.
    queue.markDone(id);
    REQUIRE(queue.drain().empty());
}

// ── 2. Retry budget + DeadLetterSink ────────────────────────────────────────
//
// Reach for this when a queued item might be permanently unreplayable (e.g. a
// stale action referencing data that no longer exists): SyncWorker retries up
// to a fixed cumulative attempt count (5) before giving up, and — if you
// install one — hands the poisoned item to a DeadLetterSink instead of just
// logging and silently dropping it, so the host can park it somewhere a human
// can look at it later.

TEST_CASE("offline queue: SyncWorker dead-letters an item that always fails to replay", "[concepts][offline][sync]") {
    InMemoryOfflineQueue queue;
    queue.enqueue("poison-payload", "op-1");  // "op-1" is this item's idempotencyKey

    std::vector<QueueItem> deadLettered;
    SyncWorker worker{
        queue,
        [](const std::string&) { return false; },  // a replay function that always fails
        [&](const QueueItem& item) { deadLettered.push_back(item); },
    };

    // run() processes one attempt per pending item per call; the retry cap is
    // 5 cumulative attempts, so 5 calls are needed to exhaust the budget.
    morph::offline::SyncResult result;
    for (int i = 0; i < 5; ++i) {
        result = worker.run();
    }

    REQUIRE(result.deadLettered == 1);
    REQUIRE(deadLettered.size() == 1);
    REQUIRE(deadLettered[0].payload == "poison-payload");
    REQUIRE(deadLettered[0].idempotencyKey == "op-1");
    REQUIRE(queue.drain().empty());  // the poisoned item is gone from the queue either way
}

// ── 3. FileOfflineQueue: durability across a process restart ───────────────
//
// Reach for this when the queue itself must survive a crash or restart, not
// just an in-process retry loop — InMemoryOfflineQueue loses everything the
// moment the process exits. FileOfflineQueue is a zero-dependency,
// NDJSON-backed IOfflineQueue that fsyncs every mutation.

TEST_CASE("offline queue: FileOfflineQueue survives destroying and reopening the queue object",
          "[concepts][offline][file]") {
    // uniqueTag's address (not the path's own) makes the filename unique per
    // test run without the initializer referring to itself.
    int uniqueTag = 0;
    auto path =
        std::filesystem::temp_directory_path() /
        ("morph_concepts_offline_queue_" + std::to_string(reinterpret_cast<std::uintptr_t>(&uniqueTag)) + ".ndjson");
    std::filesystem::remove(path);  // start from a clean slate even if a previous run left this behind

    QueueItem survivor;
    {
        FileOfflineQueue queue{path};
        auto id = queue.enqueue("payload-that-must-survive-a-restart");
        survivor = queue.drain().at(0);
        REQUIRE(survivor.id == id);
    }  // "process exits" here: the FileOfflineQueue and its file handle are gone

    {
        // "process restarts": a fresh FileOfflineQueue over the same path
        // replays what's on disk instead of starting empty.
        FileOfflineQueue reopened{path};
        auto pending = reopened.drain();
        REQUIRE(pending.size() == 1);
        REQUIRE(pending[0].id == survivor.id);
        REQUIRE(pending[0].payload == "payload-that-must-survive-a-restart");
    }

    std::filesystem::remove(path);
}
