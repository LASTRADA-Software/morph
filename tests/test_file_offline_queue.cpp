// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <morph/offline/file_offline_queue.hpp>
#include <string>
#include <vector>

namespace {

std::filesystem::path tempQueuePath() {
    static std::atomic<int> counter{0};
    auto const now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("morph_file_offline_queue_test_" + std::to_string(now) + "_" + std::to_string(++counter) + ".ndjson");
}

}  // namespace

TEST_CASE("morph::offline::FileOfflineQueue: enqueue/drain/markDone round-trip within one process", "[file_queue]") {
    auto path = tempQueuePath();
    std::filesystem::remove(path);
    {
        morph::offline::FileOfflineQueue queue{path};
        auto id1 = queue.enqueue("a");
        auto id2 = queue.enqueue("b");
        auto items = queue.drain();
        REQUIRE(items.size() == 2);
        REQUIRE(items[0].id == id1);
        REQUIRE(items[1].id == id2);
        queue.markDone(id1);
        REQUIRE(queue.drain().size() == 1);
    }
    std::filesystem::remove(path);
}

TEST_CASE("morph::offline::FileOfflineQueue: items and attempts survive destroying and reopening over the same file",
          "[file_queue]") {
    auto path = tempQueuePath();
    std::filesystem::remove(path);

    uint64_t id1 = 0;
    uint64_t id2 = 0;
    {
        morph::offline::FileOfflineQueue queue{path};
        id1 = queue.enqueue("payload-1", "key-1");
        id2 = queue.enqueue("payload-2");
        queue.setAttempts(id2, 3);
    }
    {
        morph::offline::FileOfflineQueue queue{path};
        auto items = queue.drain();
        REQUIRE(items.size() == 2);
        REQUIRE(items[0].id == id1);
        REQUIRE(items[0].payload == "payload-1");
        REQUIRE(items[0].idempotencyKey == "key-1");
        REQUIRE(items[1].id == id2);
        REQUIRE(items[1].attempts == 3);
    }
    std::filesystem::remove(path);
}

TEST_CASE("morph::offline::FileOfflineQueue: markDone persists across a reopen", "[file_queue]") {
    auto path = tempQueuePath();
    std::filesystem::remove(path);

    {
        morph::offline::FileOfflineQueue queue{path};
        auto id1 = queue.enqueue("gone");
        queue.enqueue("stays");
        queue.markDone(id1);
    }
    {
        morph::offline::FileOfflineQueue queue{path};
        auto items = queue.drain();
        REQUIRE(items.size() == 1);
        REQUIRE(items[0].payload == "stays");
    }
    std::filesystem::remove(path);
}

TEST_CASE(
    "morph::offline::FileOfflineQueue: new ids resume from the highest id ever seen, never colliding with a "
    "tombstoned id",
    "[file_queue]") {
    auto path = tempQueuePath();
    std::filesystem::remove(path);

    uint64_t id1 = 0;
    {
        morph::offline::FileOfflineQueue queue{path};
        id1 = queue.enqueue("first");
        queue.markDone(id1);  // tombstoned -- id1 must never be reused
    }
    {
        morph::offline::FileOfflineQueue queue{path};
        auto id2 = queue.enqueue("second");
        REQUIRE(id2 > id1);
    }
    std::filesystem::remove(path);
}

TEST_CASE("morph::offline::FileOfflineQueue: re-enqueue with the same idempotencyKey is deduplicated",
          "[file_queue]") {
    auto path = tempQueuePath();
    std::filesystem::remove(path);
    {
        morph::offline::FileOfflineQueue queue{path};

        auto id1 = queue.enqueue("first-payload", "op-1");
        auto id2 = queue.enqueue("second-payload", "op-1");

        REQUIRE(id1 == id2);
        REQUIRE(queue.drain().size() == 1);
    }  // close the queue's file handle before removing it -- required on Windows
    std::filesystem::remove(path);
}

TEST_CASE("morph::offline::FileOfflineQueue: empty idempotencyKey items are never deduplicated", "[file_queue]") {
    auto path = tempQueuePath();
    std::filesystem::remove(path);
    {
        morph::offline::FileOfflineQueue queue{path};

        queue.enqueue("a");
        queue.enqueue("b");

        REQUIRE(queue.drain().size() == 2);
    }  // close the queue's file handle before removing it -- required on Windows
    std::filesystem::remove(path);
}

TEST_CASE("morph::offline::FileOfflineQueue: tolerates a torn trailing line on open", "[file_queue]") {
    auto path = tempQueuePath();
    std::filesystem::remove(path);
    uint64_t id1 = 0;
    {
        morph::offline::FileOfflineQueue queue{path};
        id1 = queue.enqueue("intact");
    }
    // Manually append a torn (truncated, non-JSON) trailing line, simulating a
    // crash mid-write.
    {
        std::ofstream out{path, std::ios::app};
        out << R"({"op":"put","id":2,"payload":"cut-o)";  // no closing brace/newline
    }
    {
        morph::offline::FileOfflineQueue queue{path};
        auto items = queue.drain();
        REQUIRE(items.size() == 1);
        REQUIRE(items[0].id == id1);
        REQUIRE(items[0].payload == "intact");
    }
    std::filesystem::remove(path);
}

TEST_CASE("morph::offline::FileOfflineQueue: item survives a crash between drain() and markDone()", "[file_queue]") {
    auto path = tempQueuePath();
    std::filesystem::remove(path);
    uint64_t id1 = 0;
    {
        morph::offline::FileOfflineQueue queue{path};
        id1 = queue.enqueue("payload");
        auto items = queue.drain();
        REQUIRE(items.size() == 1);
        // Simulate a crash: no markDone() call before the queue is destroyed.
    }
    {
        morph::offline::FileOfflineQueue queue{path};
        REQUIRE(queue.drain().size() == 1);
        queue.markDone(id1);
        REQUIRE(queue.drain().empty());
    }
    std::filesystem::remove(path);
}
