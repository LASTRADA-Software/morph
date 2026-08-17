// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <morph/core/observability.hpp>
#include <morph/offline/offline_queue.hpp>
#include <optional>
#include <string>
#include <thread>
#include <vector>

TEST_CASE("morph::offline::InMemoryOfflineQueue: enqueue returns a unique id per item", "[queue]") {
    morph::offline::InMemoryOfflineQueue queue;
    auto id1 = queue.enqueue("first");
    auto id2 = queue.enqueue("second");
    REQUIRE(id1 != id2);
}

TEST_CASE("morph::offline::InMemoryOfflineQueue: drain returns items in enqueue order", "[queue]") {
    morph::offline::InMemoryOfflineQueue queue;
    queue.enqueue("a");
    queue.enqueue("b");
    queue.enqueue("c");

    auto items = queue.drain();
    REQUIRE(items.size() == 3);
    REQUIRE(items[0].payload == "a");
    REQUIRE(items[1].payload == "b");
    REQUIRE(items[2].payload == "c");
}

TEST_CASE("morph::offline::InMemoryOfflineQueue: drain on empty queue returns empty vector", "[queue]") {
    morph::offline::InMemoryOfflineQueue queue;
    REQUIRE(queue.drain().empty());
}

TEST_CASE("morph::offline::InMemoryOfflineQueue: markDone removes item from future drains", "[queue]") {
    morph::offline::InMemoryOfflineQueue queue;
    auto itemId = queue.enqueue("x");
    auto items = queue.drain();
    REQUIRE(items.size() == 1);

    queue.markDone(itemId);

    REQUIRE(queue.drain().empty());
}

TEST_CASE("morph::offline::InMemoryOfflineQueue: markDone on unknown id is a no-op", "[queue]") {
    morph::offline::InMemoryOfflineQueue queue;
    queue.enqueue("y");
    REQUIRE_NOTHROW(queue.markDone(9999));
    REQUIRE(queue.drain().size() == 1);
}

TEST_CASE("morph::offline::InMemoryOfflineQueue: drain does not remove items (items survive until markDone)",
          "[queue]") {
    morph::offline::InMemoryOfflineQueue queue;
    queue.enqueue("z");

    auto first = queue.drain();
    auto second = queue.drain();

    REQUIRE(first.size() == 1);
    REQUIRE(second.size() == 1);
    REQUIRE(first[0].payload == second[0].payload);
}

TEST_CASE("morph::offline::InMemoryOfflineQueue: concurrent enqueue from multiple threads is safe",
          "[queue][threading]") {
    morph::offline::InMemoryOfflineQueue queue;
    constexpr int nThreads = 8;
    constexpr int nPerThread = 100;

    std::vector<std::thread> threads;
    threads.reserve(nThreads);
    for (int i = 0; i < nThreads; ++i) {
        threads.emplace_back([&, i] {
            for (int j = 0; j < nPerThread; ++j) {
                queue.enqueue("t" + std::to_string(i) + "_" + std::to_string(j));
            }
        });
    }
    for (auto& thr : threads) {
        thr.join();
    }

    REQUIRE(queue.drain().size() == static_cast<std::size_t>(nThreads * nPerThread));
}

// ── Coverage: IOfflineQueue default two-arg enqueue + no-op setIdempotencyKey ──
// These target the base-class defaults in offline_queue.hpp that are not reached
// by the InMemoryOfflineQueue tests (which override the two-arg enqueue directly):
//   - L77-81  IOfflineQueue::enqueue(payload, key) default delegates to the
//             single-arg enqueue then stamps the key via setIdempotencyKey.
//   - L106-107 IOfflineQueue::setIdempotencyKey default is a no-op.

namespace {

/// Minimal IOfflineQueue implementation that does NOT override the two-arg
/// enqueue, so the base-class default (delegate + stamp) is exercised.
struct MinimalQueue : morph::offline::IOfflineQueue {
    uint64_t enqueue(std::string payload) override {
        const uint64_t itemId = ++nextId;
        items.push_back(morph::offline::QueueItem{.id = itemId, .payload = std::move(payload), .idempotencyKey = {}});
        return itemId;
    }
    std::vector<morph::offline::QueueItem> drain() const override { return items; }
    void markDone(uint64_t) override {}

    std::vector<morph::offline::QueueItem> items;
    uint64_t nextId{0};
};

}  // namespace

TEST_CASE("morph::offline::IOfflineQueue: default two-arg enqueue delegates and returns the single-arg id",
          "[queue]") {
    MinimalQueue queue;
    // Call through the base interface: the derived class does not override the
    // two-arg overload, so this resolves to IOfflineQueue's default, which
    // delegates to the single-arg enqueue and then stamps the key.
    morph::offline::IOfflineQueue& base = queue;
    auto id = base.enqueue("payload-with-key", "idem-key-1");
    REQUIRE(id == 1);
    REQUIRE(queue.items.size() == 1);
    REQUIRE(queue.items[0].payload == "payload-with-key");
}

TEST_CASE("morph::offline::IOfflineQueue: default setIdempotencyKey is a no-op (key dropped)", "[queue]") {
    MinimalQueue queue;
    // The default setIdempotencyKey drops the key; the item's idempotencyKey
    // stays empty because MinimalQueue has no per-item key storage.
    morph::offline::IOfflineQueue& base = queue;
    auto id = base.enqueue("p", "dropped-key");
    REQUIRE(queue.items[0].idempotencyKey.empty());
    REQUIRE(id == 1);
}

TEST_CASE("morph::offline::InMemoryOfflineQueue: two-arg enqueue stores the idempotency key", "[queue]") {
    morph::offline::InMemoryOfflineQueue queue;
    auto id = queue.enqueue("payload", "stable-key-123");
    auto items = queue.drain();
    REQUIRE(items.size() == 1);
    REQUIRE(items[0].id == id);
    REQUIRE(items[0].payload == "payload");
    REQUIRE(items[0].idempotencyKey == "stable-key-123");
}

// ── Coverage: QueueItem::attempts / IOfflineQueue::setAttempts ─────────────────
// These exercise the durable retry-count field and write-back hook that
// SyncWorker's cross-restart dead-lettering relies on (see sync_worker.hpp and
// docs/spec/offline/offline.md, "Retry counter is in-memory unless the queue
// opts into persisting it").

TEST_CASE("morph::offline::QueueItem: attempts defaults to 0 and round-trips through enqueue/drain",
          "[queue][attempts]") {
    morph::offline::InMemoryOfflineQueue queue;
    queue.enqueue("payload");

    auto items = queue.drain();
    REQUIRE(items.size() == 1);
    REQUIRE(items[0].attempts == 0);
}

TEST_CASE("morph::offline::InMemoryOfflineQueue: setAttempts updates the in-deque item, visible on next drain",
          "[queue][attempts]") {
    morph::offline::InMemoryOfflineQueue queue;
    auto id = queue.enqueue("payload");

    queue.setAttempts(id, 3);

    auto items = queue.drain();
    REQUIRE(items.size() == 1);
    REQUIRE(items[0].attempts == 3);
}

TEST_CASE("morph::offline::InMemoryOfflineQueue: setAttempts on an unknown id is a no-op", "[queue][attempts]") {
    morph::offline::InMemoryOfflineQueue queue;
    queue.enqueue("payload");

    REQUIRE_NOTHROW(queue.setAttempts(9999, 5));

    auto items = queue.drain();
    REQUIRE(items.size() == 1);
    REQUIRE(items[0].attempts == 0);
}

TEST_CASE("morph::offline::IOfflineQueue: default setAttempts is a no-op", "[queue][attempts]") {
    MinimalQueue queue;
    // MinimalQueue (defined above) does not override setAttempts, so this
    // resolves to IOfflineQueue's default no-op; the item's attempts field
    // never advances.
    morph::offline::IOfflineQueue& base = queue;
    auto id = base.enqueue("p");
    base.setAttempts(id, 7);
    REQUIRE(queue.items[0].attempts == 0);
}

// ── Coverage: maxDepth / overflow policy (morph#112) ───────────────────────

TEST_CASE("morph::offline::InMemoryOfflineQueue: enqueue below maxDepth succeeds", "[queue][overflow]") {
    morph::offline::InMemoryOfflineQueue queue{3};
    REQUIRE_NOTHROW(queue.enqueue("a"));
    REQUIRE_NOTHROW(queue.enqueue("b"));
    REQUIRE(queue.size() == 2);
    REQUIRE(queue.maxDepth() == std::optional<std::size_t>{3});
}

TEST_CASE("morph::offline::InMemoryOfflineQueue: enqueue at maxDepth throws OfflineQueueFullError",
          "[queue][overflow]") {
    morph::offline::InMemoryOfflineQueue queue{2};
    queue.enqueue("a");
    queue.enqueue("b");
    REQUIRE_THROWS_AS(queue.enqueue("c"), morph::offline::OfflineQueueFullError);
    REQUIRE(queue.size() == 2);  // the rejected item must not grow the queue
}

TEST_CASE("morph::offline::InMemoryOfflineQueue: markDone frees capacity for a subsequent enqueue",
          "[queue][overflow]") {
    morph::offline::InMemoryOfflineQueue queue{1};
    auto id = queue.enqueue("a");
    REQUIRE_THROWS_AS(queue.enqueue("b"), morph::offline::OfflineQueueFullError);

    queue.markDone(id);

    REQUIRE_NOTHROW(queue.enqueue("b"));
    REQUIRE(queue.size() == 1);
}

TEST_CASE("morph::offline::InMemoryOfflineQueue: default constructor is unbounded", "[queue][overflow]") {
    morph::offline::InMemoryOfflineQueue queue;
    REQUIRE(queue.maxDepth() == std::nullopt);
    for (int i = 0; i < 10000; ++i) {
        REQUIRE_NOTHROW(queue.enqueue("item" + std::to_string(i)));
    }
    REQUIRE(queue.size() == 10000);
}

TEST_CASE("morph::offline::IOfflineQueue: default size() delegates to drain().size()", "[queue][overflow]") {
    MinimalQueue queue;
    morph::offline::IOfflineQueue& base = queue;
    base.enqueue("a");
    base.enqueue("b");
    base.enqueue("c");
    // MinimalQueue does not override size(), so this resolves to
    // IOfflineQueue's default, which calls drain().size().
    REQUIRE(base.size() == 3);
    REQUIRE(base.maxDepth() == std::nullopt);
}

TEST_CASE("morph::offline::OfflineQueueFullError: carries maxDepth and currentSize", "[queue][overflow]") {
    morph::offline::OfflineQueueFullError const error{5, 5};
    REQUIRE(error.maxDepth == 5);
    REQUIRE(error.currentSize == 5);
    std::string const what = error.what();
    REQUIRE(what.find("5") != std::string::npos);
}

TEST_CASE("morph::offline::InMemoryOfflineQueue: enqueue at maxDepth emits queueOverflow metric",
          "[queue][overflow][observability]") {
    morph::observe::ScopedObserveOverride guard;
    morph::offline::InMemoryOfflineQueue queue{1};
    queue.enqueue("a");

    std::vector<double> samples;
    morph::observe::setMetricSink([&](const morph::observe::MetricEvent& evt) {
        if (evt.metric == morph::observe::Metric::queueOverflow) {
            samples.push_back(evt.value);
        }
    });

    REQUIRE_THROWS_AS(queue.enqueue("b"), morph::offline::OfflineQueueFullError);

    REQUIRE(samples.size() == 1);
    REQUIRE(samples[0] == 1.0);
}
