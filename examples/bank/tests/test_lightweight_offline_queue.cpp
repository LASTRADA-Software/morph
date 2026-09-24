// SPDX-License-Identifier: Apache-2.0
//
// Runs morph's own IOfflineQueue conformance suite against a queue morph does
// not ship: bank::offline::LightweightOfflineQueue, whose store is the
// Lightweight ORM.
//
// The suite is included from morph's tests/ directory **unedited**. That is the
// point of the exercise, not an incidental detail: a suite adjusted until a new
// implementation passes has stopped measuring anything, so if this file had
// needed the suite changed, the change would have been the finding.

#include <Lightweight/Lightweight.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <morph/offline/offline_queue.hpp>
#include <morph/offline/sync_worker.hpp>
#include <optional>
#include <stdexcept>
#include <string>

#include "bank/offline/lightweight_offline_queue.hpp"
#include "bank_test_support.hpp"
#include "offline_queue_conformance.hpp"

namespace {

using bank::offline::LightweightOfflineQueue;
using bank::offline::OfflineQueueRecord;

/// @brief Empties the queue table, so the next queue opened over it is fresh.
///
/// The conformance suite's `make` factory must hand back an *empty* store on
/// every call, and this process has exactly one database (its own private file
/// -- see unique_test_database.hpp). Truncating the one table is
/// what "a fresh store" means here.
void truncateQueueTable() {
    bank::testing::ensureDatabase();
    Lightweight::DataMapper mapper;
    mapper.Query<OfflineQueueRecord>().Delete();
}

/// @brief Factory handing the suite a queue over an emptied table.
/// @return A fresh, empty queue.
std::unique_ptr<morph::offline::IOfflineQueue> makeFreshQueue() {
    truncateQueueTable();
    return std::make_unique<LightweightOfflineQueue>();
}

/// @brief Factory handing the suite a queue over the table as it stands.
/// @return A queue reopened over the same store.
std::unique_ptr<morph::offline::IOfflineQueue> reopenQueue() {
    bank::testing::ensureDatabase();
    return std::make_unique<LightweightOfflineQueue>();
}

/// @brief Returns the pending item carrying @p itemId.
///
/// Throws rather than returning an optional so a caller reads the field
/// directly: Catch2 reports the exception as a test failure carrying the id
/// that was not found, which is what an optional plus a `REQUIRE` would have
/// said anyway.
///
/// @param queue  Queue to inspect.
/// @param itemId Id to look for.
/// @return The matching pending item.
/// @throws std::out_of_range if no pending item carries @p itemId.
morph::offline::QueueItem itemById(const morph::offline::IOfflineQueue& queue, std::uint64_t itemId) {
    for (const auto& item : queue.drain()) {
        if (item.id == itemId) {
            return item;
        }
    }
    throw std::out_of_range{"no pending item with id " + std::to_string(itemId)};
}

}  // namespace

TEST_CASE("LightweightOfflineQueue keeps the IOfflineQueue idempotency-key contract", "[offline][lightweight-queue]") {
    morph::test::checkIdempotencyKeyContract("LightweightOfflineQueue", morph::test::KeyDedup::onPendingItems,
                                             makeFreshQueue);
}

TEST_CASE("LightweightOfflineQueue keeps its dedup policy across a reopen", "[offline][lightweight-queue]") {
    truncateQueueTable();
    morph::test::checkIdempotencyKeyContractAcrossReopen("LightweightOfflineQueue",
                                                         morph::test::KeyDedup::onPendingItems, reopenQueue);
}

TEST_CASE("LightweightOfflineQueue round-trips NUL-bearing payloads and keys", "[offline][lightweight-queue]") {
    morph::test::checkNulPayloadRoundTrip("LightweightOfflineQueue", makeFreshQueue, reopenQueue);
}

TEST_CASE("LightweightOfflineQueue persists attempts across a reopen", "[offline][lightweight-queue]") {
    truncateQueueTable();
    std::uint64_t itemId{};
    {
        LightweightOfflineQueue queue;
        itemId = queue.enqueue("payload-A", "K1");
        REQUIRE(itemById(queue, itemId).attempts == 0);
        queue.setAttempts(itemId, 3);
    }
    {
        LightweightOfflineQueue queue;
        CHECK(itemById(queue, itemId).attempts == 3);
    }
}

TEST_CASE("LightweightOfflineQueue enforces maxDepth with reject-newest", "[offline][lightweight-queue]") {
    truncateQueueTable();
    LightweightOfflineQueue queue{std::optional<std::size_t>{2}};
    CHECK(queue.maxDepth() == std::optional<std::size_t>{2});
    (void)queue.enqueue("payload-A", "K1");
    (void)queue.enqueue("payload-B", "K2");
    REQUIRE(queue.size() == 2);

    CHECK_THROWS_AS((void)queue.enqueue("payload-C"), morph::offline::OfflineQueueFullError);

    // A dedup hit stores nothing, so a full queue must not reject it -- the
    // ordering docs/spec/offline/offline.md pins for FileOfflineQueue.
    const auto again = queue.enqueue("payload-B-corrected", "K2");
    CHECK(queue.size() == 2);
    CHECK(itemById(queue, again).payload == "payload-B");
}

TEST_CASE("LightweightOfflineQueue ids increase and are never reused", "[offline][lightweight-queue]") {
    // The property drain()'s `ORDER BY id` rests on, measured rather than
    // assumed. Lightweight's SQLite formatter emits `PRIMARY KEY AUTOINCREMENT`
    // for a ServerSideAutoIncrement key, not a bare rowid, so the id of a
    // removed row is not handed out again -- which is exactly what a bare
    // `INTEGER PRIMARY KEY` would do, and what would silently reorder a replay.
    truncateQueueTable();
    LightweightOfflineQueue queue;
    const auto first = queue.enqueue("payload-A");
    const auto second = queue.enqueue("payload-B");
    CHECK(second > first);

    queue.markDone(second);
    const auto third = queue.enqueue("payload-C");
    CHECK(third > second);
}

TEST_CASE("SyncWorker drains a Lightweight-backed queue", "[offline][lightweight-queue]") {
    truncateQueueTable();
    LightweightOfflineQueue queue;
    (void)queue.enqueue("one");
    (void)queue.enqueue("two");
    REQUIRE(queue.size() == 2);

    std::string replayed;
    morph::offline::SyncWorker worker{queue, [&](const std::string& payload) -> bool {
                                          replayed += payload;
                                          replayed += ';';
                                          return true;
                                      }};
    const auto result = worker.run();
    CHECK(result.successful == 2);
    CHECK(result.failed == 0);
    CHECK(replayed == "one;two;");
    CHECK(queue.size() == 0);
}
