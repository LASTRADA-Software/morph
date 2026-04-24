// SPDX-License-Identifier: Apache-2.0

#include <morph/offline_queue.hpp>
#include <morph/sync_worker.hpp>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
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

TEST_CASE("morph::offline::SyncWorker: replay exception is caught  -  item stays in queue, run continues", "[sync][exception]") {
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

TEST_CASE("morph::offline::SyncWorker: concurrent run() calls are serialised  -  second waits for first", "[sync][threading]") {
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
