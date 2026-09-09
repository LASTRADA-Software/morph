// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <morph/core/file_io_ops.hpp>
#include <morph/core/observability.hpp>
#include <morph/offline/file_offline_queue.hpp>
#include <optional>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>  // geteuid, for the permission-based fault-injection case below
#endif
#include "offline_queue_conformance.hpp"

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
        (void)queue.enqueue("stays");
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

        (void)queue.enqueue("a");
        (void)queue.enqueue("b");

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
        (void)queue.enqueue("keep-a");
        auto const gone = queue.enqueue("drop");
        (void)queue.enqueue("keep-b");
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

TEST_CASE(
    "morph::offline::FileOfflineQueue: setIdempotencyKey via the base IOfflineQueue default stamps an "
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

namespace {

/// @brief Test-only subclass that retires each item the instant it is
///        enqueued, so the base `IOfflineQueue::enqueue(payload, key)`
///        default's *second* virtual call (`setIdempotencyKey`) is
///        deterministically driven against an id that is already gone —
///        exercising its "not found" branch with no race and no timing
///        dependency. See task-11 audit finding #3
///        (file_offline_queue.hpp) and offline_queue.hpp's contract note on
///        the base default's non-atomicity.
struct SelfRetiringQueue : morph::offline::FileOfflineQueue {
    using FileOfflineQueue::FileOfflineQueue;
    // Unhide the inherited 2-arg enqueue(payload, idempotencyKey) override --
    // without this, overriding only the 1-arg enqueue() below hides the
    // 2-arg one from lookup on this type, which GCC's -Woverloaded-virtual
    // (Werror in the gcc-debug CI job) rejects even though the test below
    // only ever reaches the 2-arg overload via an explicit
    // IOfflineQueue::enqueue(...) qualified call.
    using FileOfflineQueue::enqueue;
    uint64_t enqueue(std::string payload) override {
        auto const id = FileOfflineQueue::enqueue(std::move(payload));
        markDone(id);
        return id;
    }
};

}  // namespace

TEST_CASE("morph::offline::FileOfflineQueue: setIdempotencyKey via the base default is a no-op on an unknown id",
          "[file_queue]") {
    // NOTE (Task 12, finding #3): the previous version of this test called
    // base.IOfflineQueue::enqueue("other-payload", "orphan-key") directly --
    // but that default itself calls the single-arg enqueue() *first*, which
    // inserts a brand-new item and returns its own fresh id, and only *then*
    // calls setIdempotencyKey with that same, just-inserted id. The id was
    // therefore always found; this test never exercised the "not found"
    // branch it was named for. SelfRetiringQueue overrides the single-arg
    // enqueue to markDone() the id before returning it, so the id handed to
    // setIdempotencyKey is guaranteed already-erased -- deterministic, no
    // race, single-threaded.
    auto path = tempQueuePath();
    std::filesystem::remove(path);
    {
        SelfRetiringQueue queue{path};
        auto id1 = queue.enqueue("payload");  // retired immediately by the override above
        REQUIRE(queue.drain().empty());

        morph::offline::IOfflineQueue& base = queue;
        // enqueue(payload) inserts+immediately retires a *second* item (id1's
        // successor), then setIdempotencyKey is called against that
        // already-retired id -- guaranteed not found.
        REQUIRE_NOTHROW(base.IOfflineQueue::enqueue("other-payload", "orphan-key"));

        auto items = queue.drain();
        REQUIRE(items.empty());
        (void)id1;
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

TEST_CASE(
    "morph::offline::FileOfflineQueue: load() rethrows on a malformed line that is NOT the last "
    "(genuine corruption)",
    "[file_queue]") {
    auto path = tempQueuePath();
    std::filesystem::remove(path);
    {
        morph::offline::FileOfflineQueue queue{path};
        (void)queue.enqueue("first");
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
    // NOTE (Task 12, finding #1): an earlier version of this test made
    // ioOps.fopen unconditionally return nullptr, but compact() (called
    // *before* the append-mode fopen() this test claims to cover) makes its
    // own, earlier fopen(tmp, "w") call for its temp file -- that one fails
    // first with the same unconditional-null lambda, so the constructor
    // threw from compact()'s failure, never reaching the append-mode open at
    // all. Gating on mode == "a" lets compact()'s "w"-mode open succeed for
    // real and only fails the genuine append-mode open this test is named
    // for.
    auto path = tempQueuePath();
    std::filesystem::remove(path);
    morph::core::FileIoOps ioOps;
    ioOps.fopen = [](const std::string& p, const char* mode) -> std::FILE* {
        return std::string{mode} == "a" ? nullptr : std::fopen(p.c_str(), mode);
    };
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

TEST_CASE("morph::offline::FileOfflineQueue: a failing fflush() during construction-time compaction throws",
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

// ── Coverage: maxDepth / overflow policy (morph#112) ───────────────────────

TEST_CASE("morph::offline::FileOfflineQueue: enqueue at maxDepth throws OfflineQueueFullError",
          "[file_queue][overflow]") {
    auto path = tempQueuePath();
    std::filesystem::remove(path);
    {
        morph::offline::FileOfflineQueue queue{path, morph::core::FileIoOps{}, 2};
        REQUIRE(queue.maxDepth() == std::optional<std::size_t>{2});
        (void)queue.enqueue("a");
        (void)queue.enqueue("b");
        REQUIRE_THROWS_AS(queue.enqueue("c"), morph::offline::OfflineQueueFullError);
        REQUIRE(queue.drain().size() == 2);
    }
    std::filesystem::remove(path);
}

TEST_CASE("morph::offline::FileOfflineQueue: maxDepth() is std::nullopt when unbounded", "[file_queue][overflow]") {
    auto path = tempQueuePath();
    std::filesystem::remove(path);
    {
        morph::offline::FileOfflineQueue queue{path};
        REQUIRE(queue.maxDepth() == std::nullopt);
    }
    std::filesystem::remove(path);
}

TEST_CASE("morph::offline::FileOfflineQueue: maxDepth survives destroying and reopening over the same file",
          "[file_queue][overflow]") {
    auto path = tempQueuePath();
    std::filesystem::remove(path);
    {
        morph::offline::FileOfflineQueue queue{path, morph::core::FileIoOps{}, 1};
        (void)queue.enqueue("a");
        REQUIRE_THROWS_AS(queue.enqueue("b"), morph::offline::OfflineQueueFullError);
    }
    {
        // Reopened with the same maxDepth argument -- still enforced. maxDepth
        // is a per-construction parameter, not persisted in the file itself.
        morph::offline::FileOfflineQueue queue{path, morph::core::FileIoOps{}, 1};
        REQUIRE(queue.drain().size() == 1);
        REQUIRE_THROWS_AS(queue.enqueue("b"), morph::offline::OfflineQueueFullError);
    }
    std::filesystem::remove(path);
}

TEST_CASE("morph::offline::FileOfflineQueue: a dedup hit on a full queue does not throw", "[file_queue][overflow]") {
    auto path = tempQueuePath();
    std::filesystem::remove(path);
    {
        morph::offline::FileOfflineQueue queue{path, morph::core::FileIoOps{}, 1};
        auto id1 = queue.enqueue("first-payload", "op-1");
        // The dedup scan runs before the capacity check, so a repeat of the
        // same idempotencyKey on a full queue returns the existing id instead
        // of throwing.
        auto id2 = queue.enqueue("second-payload", "op-1");
        REQUIRE(id1 == id2);
        REQUIRE(queue.drain().size() == 1);
    }
    std::filesystem::remove(path);
}

TEST_CASE("morph::offline::FileOfflineQueue: size() reflects live pending count", "[file_queue][overflow]") {
    auto path = tempQueuePath();
    std::filesystem::remove(path);
    {
        morph::offline::FileOfflineQueue queue{path};
        REQUIRE(queue.size() == 0);
        auto id1 = queue.enqueue("a");
        (void)queue.enqueue("b");
        REQUIRE(queue.size() == 2);
        queue.markDone(id1);
        REQUIRE(queue.size() == 1);
    }
    std::filesystem::remove(path);
}

TEST_CASE("morph::offline::FileOfflineQueue: enqueue at maxDepth emits queueOverflow metric",
          "[file_queue][overflow][observability]") {
    morph::observe::ScopedObserveOverride guard;
    auto path = tempQueuePath();
    std::filesystem::remove(path);
    {
        morph::offline::FileOfflineQueue queue{path, morph::core::FileIoOps{}, 1};
        (void)queue.enqueue("a");

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
    std::filesystem::remove(path);
}

// ── IOfflineQueue conformance ─────────────────────────────────────────────────
//
// `FileOfflineQueue` deduplicates a non-empty idempotency key already carried by
// a pending item (linear scan). That is a permitted strengthening of the
// `IOfflineQueue` contract, declared here so the shared suite asserts it rather
// than tolerating either behaviour.

TEST_CASE("morph::offline::FileOfflineQueue: IOfflineQueue idempotency-key conformance", "[file_queue]") {
    std::vector<std::filesystem::path> created;
    morph::test::checkIdempotencyKeyContract("FileOfflineQueue", morph::test::KeyDedup::onPendingItems, [&created] {
        auto path = tempQueuePath();
        std::filesystem::remove(path);
        created.push_back(path);
        return std::make_unique<morph::offline::FileOfflineQueue>(path);
    });
    for (auto const& path : created) {
        std::filesystem::remove(path);
    }
}

TEST_CASE("morph::offline::FileOfflineQueue: the idempotency-key contract survives a reopen", "[file_queue]") {
    auto path = tempQueuePath();
    std::filesystem::remove(path);
    morph::test::checkIdempotencyKeyContractAcrossReopen(
        "FileOfflineQueue", morph::test::KeyDedup::onPendingItems,
        [&path] { return std::make_unique<morph::offline::FileOfflineQueue>(path); });
    std::filesystem::remove(path);
}

// ── An unreadable queue file must not be committed away (morph#494) ──
//
// load() read with an unchecked ifstream and the constructor calls compact()
// straight after, so a failed read produced an empty `_items` that compact()
// then wrote over the real file. Measured before the fix: 3 pending items (306
// bytes) went to 0, with the constructor returning normally and the queue
// reporting an empty backlog. Unlike FileActionLog's sibling defect this needed
// no fault injection at all -- load() bypasses the FileIoOps seam entirely.
//
// POSIX-only and non-root, for the same reasons as the FileActionLog case.
#ifndef _WIN32
TEST_CASE("FileOfflineQueue: an unreadable queue file is not silently compacted away",
          "[offline][file][fault-injection]") {
    if (::geteuid() == 0) {
        SUCCEED("running as root: permission bits are not enforced");
        return;
    }
    const auto path = std::filesystem::temp_directory_path() / "morph_test_offline_unreadable.ndjson";
    std::filesystem::remove(path);
    std::uintmax_t sizeBefore = 0;
    {
        morph::offline::FileOfflineQueue queue{path};
        (void)queue.enqueue(R"({"op":"transfer","amount":100})");
        (void)queue.enqueue(R"({"op":"transfer","amount":250})");
        (void)queue.enqueue(R"({"op":"transfer","amount":375})");
        REQUIRE(queue.drain().size() == 3);
        sizeBefore = std::filesystem::file_size(path);
        REQUIRE(sizeBefore > 0);
    }

    std::filesystem::permissions(path, std::filesystem::perms::owner_write);
    // Must throw rather than hand back a queue that reports no pending work.
    REQUIRE_THROWS_AS(morph::offline::FileOfflineQueue{path}, std::runtime_error);

    std::filesystem::permissions(path, std::filesystem::perms::owner_all);
    REQUIRE(std::filesystem::file_size(path) == sizeBefore);
    morph::offline::FileOfflineQueue reopened{path};
    REQUIRE(reopened.drain().size() == 3);
    std::filesystem::remove(path);
}
#endif  // _WIN32
