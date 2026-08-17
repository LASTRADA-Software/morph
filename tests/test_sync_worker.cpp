// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <morph/core/logger.hpp>
#include <morph/core/observability.hpp>
#include <morph/offline/offline_queue.hpp>
#include <morph/offline/sync_worker.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

TEST_CASE("morph::offline::SyncWorker: run on empty queue returns zero successful and zero failed", "[sync]") {
    morph::offline::InMemoryOfflineQueue queue;
    morph::offline::SyncWorker worker{queue, [](const std::string&) { return true; }};
    auto result = worker.run();
    REQUIRE(result.successful == 0);
    REQUIRE(result.failed == 0);
}

TEST_CASE("morph::offline::SyncWorker: successful replay removes items from queue", "[sync]") {
    morph::offline::InMemoryOfflineQueue queue;
    queue.enqueue("item1");
    queue.enqueue("item2");

    morph::offline::SyncWorker worker{queue, [](const std::string&) { return true; }};
    auto result = worker.run();

    REQUIRE(result.successful == 2);
    REQUIRE(result.failed == 0);
    REQUIRE(queue.drain().empty());
}

TEST_CASE("morph::offline::SyncWorker: failed replay leaves items in queue", "[sync]") {
    morph::offline::InMemoryOfflineQueue queue;
    queue.enqueue("item1");
    queue.enqueue("item2");

    morph::offline::SyncWorker worker{queue, [](const std::string&) { return false; }};
    auto result = worker.run();

    REQUIRE(result.successful == 0);
    REQUIRE(result.failed == 2);
    REQUIRE(queue.drain().size() == 2);
}

TEST_CASE("morph::offline::SyncWorker: partial replay  -  first succeeds, second fails", "[sync]") {
    morph::offline::InMemoryOfflineQueue queue;
    queue.enqueue("good");
    queue.enqueue("bad");

    int call = 0;
    morph::offline::SyncWorker worker{queue, [&](const std::string&) {
                                          return ++call == 1;  // first call succeeds, second fails
                                      }};
    auto result = worker.run();

    REQUIRE(result.successful == 1);
    REQUIRE(result.failed == 1);
    auto remaining = queue.drain();
    REQUIRE(remaining.size() == 1);
    REQUIRE(remaining[0].payload == "bad");
}

TEST_CASE("morph::offline::SyncWorker: replay function receives the correct payload", "[sync]") {
    morph::offline::InMemoryOfflineQueue queue;
    queue.enqueue("hello");
    queue.enqueue("world");

    std::vector<std::string> received;
    morph::offline::SyncWorker worker{queue, [&](const std::string& payload) {
                                          received.push_back(payload);
                                          return true;
                                      }};
    worker.run();

    REQUIRE(received.size() == 2);
    REQUIRE(received[0] == "hello");
    REQUIRE(received[1] == "world");
}

TEST_CASE("morph::offline::SyncWorker: stop() aborts run() before processing items", "[sync][stop]") {
    morph::offline::InMemoryOfflineQueue queue;
    for (int i = 0; i < 10; ++i) {
        queue.enqueue("item" + std::to_string(i));
    }

    morph::offline::SyncWorker worker{queue, [](const std::string&) { return true; }};

    // Signal stop before calling run().
    worker.stop();
    auto result = worker.run();

    // stop was set, so run() sees _stopped immediately and processes zero items.
    REQUIRE(result.successful == 0);
    REQUIRE(queue.drain().size() == 10);
}

TEST_CASE("morph::offline::SyncWorker: replay exception is caught  -  item stays in queue, run continues",
          "[sync][exception]") {
    morph::offline::InMemoryOfflineQueue queue;
    queue.enqueue("throws");
    queue.enqueue("ok");

    morph::offline::SyncWorker worker{queue, [](const std::string& payload) -> bool {
                                          if (payload == "throws") {
                                              throw std::runtime_error("boom");
                                          }
                                          return true;
                                      }};
    auto result = worker.run();

    REQUIRE(result.successful == 1);
    REQUIRE(result.failed == 1);
    // "throws" item remains; "ok" item was removed.
    auto remaining = queue.drain();
    REQUIRE(remaining.size() == 1);
    REQUIRE(remaining[0].payload == "throws");
}

TEST_CASE("morph::offline::SyncWorker: concurrent run() calls are serialised  -  second waits for first",
          "[sync][threading]") {
    morph::offline::InMemoryOfflineQueue queue;
    for (int i = 0; i < 4; ++i) {
        queue.enqueue("item" + std::to_string(i));
    }

    std::atomic<int> replayCount{0};
    morph::offline::SyncWorker worker{queue, [&](const std::string&) {
                                          ++replayCount;
                                          std::this_thread::sleep_for(std::chrono::milliseconds(20));
                                          return true;
                                      }};

    morph::offline::SyncResult result1;
    morph::offline::SyncResult result2;
    std::thread thr1{[&] { result1 = worker.run(); }};
    std::thread thr2{[&] { result2 = worker.run(); }};
    thr1.join();
    thr2.join();

    // One run drains all 4 items; the other finds the queue empty.
    REQUIRE(result1.successful + result2.successful == 4);
    REQUIRE(queue.drain().empty());
}

TEST_CASE("morph::offline::SyncWorker: stop resets after run  -  next run proceeds normally", "[sync][stop]") {
    morph::offline::InMemoryOfflineQueue queue;
    queue.enqueue("a");
    queue.enqueue("b");

    morph::offline::SyncWorker worker{queue, [](const std::string&) { return true; }};

    // First run is aborted immediately (stop before run).
    worker.stop();
    worker.run();

    // Items remain because run() was aborted.
    // (If by chance 0 items were enqueued, re-enqueue to guarantee the next run has work.)
    if (queue.drain().empty()) {
        queue.enqueue("c");
    }

    // Second run must not inherit the stop flag  -  processes all remaining items.
    auto result = worker.run();
    REQUIRE(result.successful > 0);
    REQUIRE(queue.drain().empty());
}

// ── Coverage: durable attempts + DeadLetterSink (see docs/spec/offline/offline.md,
// "Retry counter is in-memory unless the queue opts into persisting it" and
// "Dead-lettering has an optional recovery hook") ───────────────────────────

TEST_CASE("morph::offline::SyncWorker: no DeadLetterSink set  -  default log-and-drop path fires unchanged",
          "[sync][attempts][logger]") {
    morph::log::ScopedLoggerOverride guard;
    std::vector<std::string> logged;
    morph::log::setLogger([&](morph::log::LogLevel, std::string_view msg) { logged.emplace_back(msg); });

    morph::offline::InMemoryOfflineQueue queue;
    queue.enqueue("poison-payload");
    morph::offline::SyncWorker worker{queue, [](const std::string&) { return false; }};

    morph::offline::SyncResult result;
    for (int i = 0; i < 5; ++i) {
        result = worker.run();
    }
    REQUIRE(result.deadLettered == 1);

    bool foundDropLine = false;
    for (const auto& line : logged) {
        if (line.find("dropping payload after 5 failed attempts") != std::string::npos &&
            line.find("poison-payload") != std::string::npos) {
            foundDropLine = true;
        }
    }
    REQUIRE(foundDropLine);
}

TEST_CASE(
    "morph::offline::SyncWorker: DeadLetterSink set  -  receives the exhausted item before removal and suppresses "
    "the default log line",
    "[sync][attempts]") {
    morph::log::ScopedLoggerOverride guard;
    std::vector<std::string> logged;
    morph::log::setLogger([&](morph::log::LogLevel, std::string_view msg) { logged.emplace_back(msg); });

    morph::offline::InMemoryOfflineQueue queue;
    auto id = queue.enqueue("poison-payload", "idem-key-1");

    std::vector<morph::offline::QueueItem> sunk;
    std::size_t queueSizeDuringSink = 0;
    morph::offline::SyncWorker worker{queue, [](const std::string&) { return false; },
                                      [&](const morph::offline::QueueItem& poisoned) {
                                          // The sink must run before markDone: capture the queue
                                          // size mid-callback (asserted after run() returns -- a
                                          // failed REQUIRE here would be swallowed by run()'s own
                                          // try/catch around the sink call).
                                          queueSizeDuringSink = queue.drain().size();
                                          sunk.push_back(poisoned);
                                      }};

    morph::offline::SyncResult result;
    for (int i = 0; i < 5; ++i) {
        result = worker.run();
    }

    REQUIRE(result.deadLettered == 1);
    REQUIRE(queueSizeDuringSink == 1);
    REQUIRE(sunk.size() == 1);
    REQUIRE(sunk[0].id == id);
    REQUIRE(sunk[0].payload == "poison-payload");
    REQUIRE(sunk[0].idempotencyKey == "idem-key-1");
    REQUIRE(sunk[0].attempts == 5);
    REQUIRE(queue.drain().empty());

    for (const auto& line : logged) {
        REQUIRE(line.find("dropping payload after 5 failed attempts") == std::string::npos);
    }
}

TEST_CASE("morph::offline::SyncWorker: a throwing DeadLetterSink is caught  -  item is still removed",
          "[sync][attempts][exception]") {
    morph::log::ScopedLoggerOverride guard;
    std::vector<std::string> logged;
    morph::log::setLogger([&](morph::log::LogLevel, std::string_view msg) { logged.emplace_back(msg); });

    morph::offline::InMemoryOfflineQueue queue;
    queue.enqueue("poison");

    morph::offline::SyncWorker worker{
        queue, [](const std::string&) { return false; },
        [](const morph::offline::QueueItem&) -> void { throw std::runtime_error("sink boom"); }};

    morph::offline::SyncResult result;
    for (int i = 0; i < 5; ++i) {
        result = worker.run();
    }

    REQUIRE(result.deadLettered == 1);
    REQUIRE(queue.drain().empty());

    bool foundThrowLine = false;
    for (const auto& line : logged) {
        if (line.find("dead-letter sink threw") != std::string::npos) {
            foundThrowLine = true;
        }
    }
    REQUIRE(foundThrowLine);
}

TEST_CASE(
    "morph::offline::SyncWorker: a queue that persists attempts dead-letters cumulatively across a simulated "
    "restart",
    "[sync][attempts][durable]") {
    morph::log::ScopedLoggerOverride guard;  // silence the "dropping payload" error log (no sink set here)
    morph::log::setLogger([](morph::log::LogLevel, std::string_view) {});

    morph::offline::InMemoryOfflineQueue queue;  // overrides setAttempts -> persists across "restarts"
    queue.enqueue("poison");

    morph::offline::SyncResult result;
    for (int i = 0; i < 5; ++i) {
        // A fresh SyncWorker each iteration simulates a process restart: its
        // in-memory _attempts map starts empty every time, so only the count
        // persisted on the queue's QueueItem::attempts carries over.
        morph::offline::SyncWorker worker{queue, [](const std::string&) { return false; }};
        result = worker.run();
    }

    REQUIRE(result.deadLettered == 1);
    REQUIRE(queue.drain().empty());
}

namespace {
/// IOfflineQueue that does NOT override setAttempts, so QueueItem::attempts
/// never advances -- used to prove SyncWorker falls back to a purely
/// in-memory count (today's original behavior) when the queue does not
/// persist attempts.
struct NonDurableQueue : morph::offline::IOfflineQueue {
    uint64_t enqueue(std::string payload) override {
        uint64_t const itemId = ++nextId;
        items.push_back(morph::offline::QueueItem{.id = itemId, .payload = std::move(payload), .idempotencyKey = {}});
        return itemId;
    }
    std::vector<morph::offline::QueueItem> drain() const override { return items; }
    void markDone(uint64_t itemId) override {
        std::erase_if(items, [itemId](const morph::offline::QueueItem& item) { return item.id == itemId; });
    }
    // setAttempts intentionally NOT overridden: inherits IOfflineQueue's no-op.

    std::vector<morph::offline::QueueItem> items;
    uint64_t nextId{0};
};
}  // namespace

TEST_CASE(
    "morph::offline::SyncWorker: a queue that does not override setAttempts resets the count on a new SyncWorker",
    "[sync][attempts]") {
    NonDurableQueue queue;
    queue.enqueue("poison");

    {
        morph::offline::SyncWorker worker{queue, [](const std::string&) { return false; }};
        // 4 failures with the SAME worker instance: not exhausted yet (cap is 5).
        for (int i = 0; i < 4; ++i) {
            auto result = worker.run();
            REQUIRE(result.failed == 1);
        }
    }

    // "Restart": a fresh SyncWorker over the same (non-persisting) queue. Its
    // in-memory count starts at 0, and QueueItem::attempts was never written
    // back (setAttempts is the inherited no-op), so this failure is attempt
    // #1, not #5 -- today's in-memory-only behavior, unchanged.
    morph::offline::SyncWorker worker2{queue, [](const std::string&) { return false; }};
    auto result = worker2.run();
    REQUIRE(result.failed == 1);
    REQUIRE(result.deadLettered == 0);
    REQUIRE(queue.drain().size() == 1);
}

TEST_CASE("morph::offline::SyncWorker: run() emits queueDepth with the pending count at drain",
          "[sync][observability]") {
    morph::observe::ScopedObserveOverride guard;
    morph::offline::InMemoryOfflineQueue queue;
    queue.enqueue("item1");
    queue.enqueue("item2");
    queue.enqueue("item3");

    std::vector<double> samples;
    morph::observe::setMetricSink([&](const morph::observe::MetricEvent& evt) {
        if (evt.metric == morph::observe::Metric::queueDepth) {
            samples.push_back(evt.value);
        }
    });

    morph::offline::SyncWorker worker{queue, [](const std::string&) { return true; }};
    worker.run();

    REQUIRE(samples.size() == 1);
    REQUIRE(samples[0] == 3.0);
}

TEST_CASE("morph::offline::SyncWorker: run() over a queue at maxDepth still drains and replays normally",
          "[sync][overflow]") {
    // maxDepth bounds enqueue(); it has no bearing on drain()/replay -- a
    // full queue still drains and replays every pending item exactly as an
    // unbounded one would.
    morph::offline::InMemoryOfflineQueue queue{3};
    queue.enqueue("a");
    queue.enqueue("b");
    queue.enqueue("c");
    REQUIRE_THROWS_AS(queue.enqueue("d"), morph::offline::OfflineQueueFullError);

    morph::offline::SyncWorker worker{queue, [](const std::string&) { return true; }};
    auto result = worker.run();

    REQUIRE(result.successful == 3);
    REQUIRE(result.failed == 0);
    REQUIRE(queue.drain().empty());

    // Capacity freed up by the successful replay -- enqueue succeeds again.
    REQUIRE_NOTHROW(queue.enqueue("e"));
}
