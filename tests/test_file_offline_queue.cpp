// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <morph/core/file_io_ops.hpp>
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

TEST_CASE("morph::offline::FileOfflineQueue: ids are never reissued across repeated restarts", "[file_queue]") {
    // compact() keeps only surviving "put" lines, and load() derives _nextId
    // from the ids it reads, so dropping every tombstone used to let the mark
    // regress -- but only from the *second* restart onward, since the first
    // still reads the original tombstone. The id of a completed, acknowledged
    // item was then handed to a brand-new one.
    auto const path = tempQueuePath();
    std::uint64_t firstId = 0;
    std::uint64_t doneId = 0;
    {
        morph::offline::FileOfflineQueue queue{path};
        firstId = queue.enqueue("one");
        doneId = queue.enqueue("two");
        queue.markDone(doneId);
    }
    REQUIRE(firstId != doneId);

    {
        morph::offline::FileOfflineQueue queue{path};  // first restart: compacts away the tombstone
        REQUIRE(queue.drain().size() == 1);
    }

    std::uint64_t reissued = 0;
    {
        morph::offline::FileOfflineQueue queue{path};  // second restart: the mark must have survived
        reissued = queue.enqueue("three");
    }
    CHECK(reissued != doneId);
    CHECK(reissued != firstId);
    CHECK(reissued > doneId);

    std::filesystem::remove(path);
}

TEST_CASE("morph::offline::FileOfflineQueue: the id high-water mark survives an empty queue", "[file_queue]") {
    // With nothing surviving, compaction writes no "put" lines at all, so the
    // mark has nowhere to hide unless it is recorded explicitly.
    auto const path = tempQueuePath();
    std::uint64_t lastId = 0;
    {
        morph::offline::FileOfflineQueue queue{path};
        lastId = queue.enqueue("only");
        queue.markDone(lastId);
    }
    {
        morph::offline::FileOfflineQueue const queue{path};
    }  // restart 1: compacts to empty
    {
        morph::offline::FileOfflineQueue queue{path};  // restart 2
        REQUIRE(queue.drain().empty());
        CHECK(queue.enqueue("next") > lastId);
    }
    std::filesystem::remove(path);
}

TEST_CASE("morph::offline::FileOfflineQueue: surviving items are intact after repeated restarts", "[file_queue]") {
    // The high-water marker is written as a "done" record, so it must never
    // collide with a surviving id and delete it on the next load.
    auto const path = tempQueuePath();
    {
        morph::offline::FileOfflineQueue queue{path};
        queue.enqueue("keep-a");
        auto const gone = queue.enqueue("drop");
        queue.enqueue("keep-b");
        queue.markDone(gone);
    }
    for (int restart = 0; restart < 3; ++restart) {
        morph::offline::FileOfflineQueue queue{path};
        auto const pending = queue.drain();
        REQUIRE(pending.size() == 2);
        CHECK(pending.at(0).payload == "keep-a");
        CHECK(pending.at(1).payload == "keep-b");
    }
    std::filesystem::remove(path);
}

TEST_CASE("morph::offline::FileOfflineQueue: a non-matching idempotencyKey enqueues a new item, not a dedup hit",
          "[file_queue]") {
    // The dedup scan in enqueue() must walk past a pending item with a
    // *different* non-empty key without matching it -- covering the loop's
    // no-match arm, not just the single-item, first-iteration match the
    // existing dedup test exercises.
    auto path = tempQueuePath();
    std::filesystem::remove(path);
    {
        morph::offline::FileOfflineQueue queue{path};
        auto id1 = queue.enqueue("first-payload", "key-a");
        auto id2 = queue.enqueue("second-payload", "key-b");

        REQUIRE(id2 != id1);
        REQUIRE(queue.drain().size() == 2);
    }
    std::filesystem::remove(path);
}

TEST_CASE("morph::offline::FileOfflineQueue: markDone on an unknown id is a no-op", "[file_queue]") {
    auto path = tempQueuePath();
    std::filesystem::remove(path);
    {
        morph::offline::FileOfflineQueue queue{path};
        auto id1 = queue.enqueue("payload");
        REQUIRE_NOTHROW(queue.markDone(id1 + 1000));  // never issued -- erase() finds nothing
        REQUIRE(queue.drain().size() == 1);
    }
    std::filesystem::remove(path);
}

TEST_CASE("morph::offline::FileOfflineQueue: setAttempts on an unknown id is a no-op", "[file_queue]") {
    auto path = tempQueuePath();
    std::filesystem::remove(path);
    {
        morph::offline::FileOfflineQueue queue{path};
        auto id1 = queue.enqueue("payload");
        REQUIRE_NOTHROW(queue.setAttempts(id1 + 1000, 7));  // never issued -- find() misses
        auto items = queue.drain();
        REQUIRE(items.size() == 1);
        CHECK(items[0].attempts == 0);  // untouched
    }
    std::filesystem::remove(path);
}

TEST_CASE("morph::offline::FileOfflineQueue: setIdempotencyKey via the base IOfflineQueue default stamps an "
          "already-enqueued item",
          "[file_queue]") {
    // FileOfflineQueue overrides the two-arg enqueue(payload, key) itself, so
    // an ordinary call -- through any reference type -- always resolves to
    // that override, never to IOfflineQueue's default (which delegates to the
    // single-arg enqueue and then stamps the key via the protected
    // setIdempotencyKey hook). The explicit scope-qualified call below is the
    // only way to invoke that base default over a FileOfflineQueue, mirroring
    // the equivalent coverage test for IOfflineQueue's default in
    // test_offline_queue.cpp -- it reaches FileOfflineQueue's own
    // setIdempotencyKey override, which is otherwise never invoked by any
    // ordinary call path.
    auto path = tempQueuePath();
    std::filesystem::remove(path);
    {
        morph::offline::FileOfflineQueue queue{path};
        morph::offline::IOfflineQueue& base = queue;
        auto id = base.IOfflineQueue::enqueue("payload", "stamped-key");

        auto items = queue.drain();
        REQUIRE(items.size() == 1);
        CHECK(items[0].id == id);
        CHECK(items[0].idempotencyKey == "stamped-key");
    }
    std::filesystem::remove(path);
}

TEST_CASE("morph::offline::FileOfflineQueue: setIdempotencyKey via the base default is a no-op on an unknown id",
          "[file_queue]") {
    auto path = tempQueuePath();
    std::filesystem::remove(path);
    {
        morph::offline::FileOfflineQueue queue{path};
        auto id1 = queue.enqueue("payload");

        // Stamp a key onto an id that was never enqueued -- setIdempotencyKey's
        // find() misses, so this must not throw or touch any existing item.
        morph::offline::IOfflineQueue& base = queue;
        REQUIRE_NOTHROW(base.IOfflineQueue::enqueue("other-payload", "orphan-key"));

        auto items = queue.drain();
        REQUIRE(items.size() == 2);
        CHECK(items[0].id == id1);
        CHECK(items[0].idempotencyKey.empty());
    }
    std::filesystem::remove(path);
}

TEST_CASE("morph::offline::FileOfflineQueue: load() skips a blank line in the NDJSON file", "[file_queue]") {
    auto path = tempQueuePath();
    std::filesystem::remove(path);
    uint64_t id1 = 0;
    {
        morph::offline::FileOfflineQueue queue{path};
        id1 = queue.enqueue("first");
    }
    // A blank line can't be produced by FileOfflineQueue itself (every write
    // ends in exactly one '\n' with no other blank lines), but a hand-edited
    // or externally-appended file could have one -- load() must skip it
    // rather than try to decode it as JSON.
    {
        std::ofstream out{path, std::ios::app};
        out << "\n";
    }
    {
        morph::offline::FileOfflineQueue queue{path};
        auto items = queue.drain();
        REQUIRE(items.size() == 1);
        REQUIRE(items[0].id == id1);
        REQUIRE(items[0].payload == "first");
        // The queue is still fully usable afterwards.
        auto id2 = queue.enqueue("second");
        REQUIRE(id2 > id1);
    }
    std::filesystem::remove(path);
}

TEST_CASE("morph::offline::FileOfflineQueue: load() rethrows on a malformed line that is NOT the last "
          "(genuine corruption)",
          "[file_queue]") {
    auto path = tempQueuePath();
    std::filesystem::remove(path);
    {
        morph::offline::FileOfflineQueue queue{path};
        queue.enqueue("first");
    }
    {
        // Insert a complete-but-malformed line, then a well-formed line after
        // it -- the malformed line is no longer trailing, so it must be
        // reported as genuine corruption, not tolerated like a torn tail.
        std::ofstream out{path, std::ios::app};
        out << "not json at all\n";
        out << R"({"op":"put","id":99,"payload":"after-corruption","idempotencyKey":"","attempts":0})" << "\n";
    }
    REQUIRE_THROWS_AS(morph::offline::FileOfflineQueue(path), morph::offline::FileOfflineQueueError);
    std::filesystem::remove(path);
}

TEST_CASE("morph::offline::FileOfflineQueue: construction throws if the compaction temp file cannot be opened",
          "[file_queue]") {
    // load() no-ops when the path does not exist, so compact() is the first
    // thing to touch disk: its own fopen(path + ".compact-tmp", "w") fails
    // when the parent directory does not exist, throwing before the
    // append-mode _file handle is ever opened.
    auto const path = std::filesystem::path{"/no/such/directory/at/all/queue.ndjson"};
    REQUIRE_THROWS_AS(morph::offline::FileOfflineQueue(path), std::runtime_error);
}

// ── FileIoOps fault injection (LASTRADA-Software/morph#97) ─────────────────
//
// Same seam FileActionLog's own fault-injection tests use (morph/core/
// file_io_ops.hpp) -- FileOfflineQueue has the identical class of gap:
// several branches only run when a real OS-level file-I/O call fails
// partway through an otherwise-successful operation.

TEST_CASE("morph::offline::FileOfflineQueue: the constructor's own append-mode fopen() failing throws",
          "[file_queue][fault-injection]") {
    auto path = tempQueuePath();
    std::filesystem::remove(path);
    morph::core::FileIoOps ioOps;
    ioOps.fopen = [](const std::string&, const char*) -> std::FILE* { return nullptr; };
    REQUIRE_THROWS_AS(morph::offline::FileOfflineQueue(path, ioOps), std::runtime_error);
    std::filesystem::remove(path);
}

TEST_CASE("morph::offline::FileOfflineQueue::enqueue: a short fwrite() to the append-mode file throws",
          "[file_queue][fault-injection]") {
    auto path = tempQueuePath();
    std::filesystem::remove(path);
    auto shouldFail = std::make_shared<bool>(false);
    morph::core::FileIoOps ioOps;
    ioOps.fwrite = [shouldFail](const void* buffer, std::size_t size, std::FILE* file) {
        return *shouldFail ? size - 1 : std::fwrite(buffer, 1, size, file);
    };

    {
        morph::offline::FileOfflineQueue queue{path, ioOps};
        *shouldFail = true;
        REQUIRE_THROWS_AS(queue.enqueue("payload"), std::runtime_error);
    }  // queue's own file handle must close before remove() -- Windows cannot delete an open file
    std::filesystem::remove(path);
}

TEST_CASE("morph::offline::FileOfflineQueue::enqueue: a failing fflush() on the append-mode file throws",
          "[file_queue][fault-injection]") {
    auto path = tempQueuePath();
    std::filesystem::remove(path);
    auto shouldFail = std::make_shared<bool>(false);
    morph::core::FileIoOps ioOps;
    ioOps.fflush = [shouldFail](std::FILE* file) { return *shouldFail ? -1 : std::fflush(file); };

    {
        morph::offline::FileOfflineQueue queue{path, ioOps};
        *shouldFail = true;
        REQUIRE_THROWS_AS(queue.enqueue("payload"), std::runtime_error);
    }
    std::filesystem::remove(path);
}

TEST_CASE("morph::offline::FileOfflineQueue::enqueue: a failing fsync() on the append-mode file throws",
          "[file_queue][fault-injection]") {
    auto path = tempQueuePath();
    std::filesystem::remove(path);
    auto shouldFail = std::make_shared<bool>(false);
    morph::core::FileIoOps ioOps;
    morph::core::FileIoOps const realOps;
    ioOps.fsync = [shouldFail, realOps](std::FILE* file) { return *shouldFail ? -1 : realOps.fsync(file); };

    {
        morph::offline::FileOfflineQueue queue{path, ioOps};
        *shouldFail = true;
        REQUIRE_THROWS_AS(queue.enqueue("payload"), std::runtime_error);
    }
    std::filesystem::remove(path);
}

TEST_CASE(
    "morph::offline::FileOfflineQueue: a short fwrite() during construction-time compaction throws before the "
    "append-mode file is ever opened",
    "[file_queue][fault-injection]") {
    auto path = tempQueuePath();
    {
        // Seed one surviving item so compact() has at least one "put" line to
        // write -- an empty queue's compact() writes nothing and never calls
        // fwrite at all.
        std::ofstream out{path};
        out << R"({"op":"put","id":1,"payload":"seed","idempotencyKey":"","attempts":0})" << "\n";
    }
    morph::core::FileIoOps ioOps;
    ioOps.fwrite = [](const void*, std::size_t size, std::FILE*) { return size - 1; };
    REQUIRE_THROWS_AS(morph::offline::FileOfflineQueue(path, ioOps), std::runtime_error);
    std::filesystem::remove(path);
}

TEST_CASE(
    "morph::offline::FileOfflineQueue: a failing fflush() during construction-time compaction throws",
    "[file_queue][fault-injection]") {
    auto path = tempQueuePath();
    {
        std::ofstream out{path};
        out << R"({"op":"put","id":1,"payload":"seed","idempotencyKey":"","attempts":0})" << "\n";
    }
    morph::core::FileIoOps ioOps;
    ioOps.fflush = [](std::FILE*) { return -1; };
    REQUIRE_THROWS_AS(morph::offline::FileOfflineQueue(path, ioOps), std::runtime_error);
    std::filesystem::remove(path);
}
