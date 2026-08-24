// SPDX-License-Identifier: Apache-2.0

/// @file
/// @brief Shared `IOfflineQueue` conformance checks, run against every
///        implementation morph ships.
///
/// `SqliteOfflineQueue` lives behind `MORPH_BUILD_OFFLINE_SQLITE` in a separate
/// test target, so these checks are a header rather than a test file: each
/// target instantiates them for the implementations it can link. A new
/// `IOfflineQueue` implementation gets the whole suite by adding one call.
///
/// The checks pin the interface-level guarantees documented on `IOfflineQueue`
/// (`include/morph/offline/offline_queue.hpp`) and in `docs/spec/offline/offline.md`.
/// Dedup of a repeated non-empty `idempotencyKey` is a *permitted
/// strengthening*, not a requirement, so each call site declares which policy
/// its implementation has and the suite asserts that declaration holds. That
/// declaration is what makes the suite bite: a change that silently gives
/// `InMemoryOfflineQueue` dedup, or takes it away from `FileOfflineQueue`,
/// fails here rather than being absorbed as "either behaviour is fine".

#pragma once

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/offline/offline_queue.hpp>
#include <optional>
#include <string>
#include <vector>

namespace morph::test {

/// @brief Whether an implementation deduplicates a repeated non-empty
///        `idempotencyKey` against its pending items.
enum class KeyDedup : std::uint8_t {
    /// @brief Never deduplicates; every `enqueue` yields a distinct item.
    never,
    /// @brief Deduplicates a non-empty key that a pending item already carries.
    onPendingItems,
};

/// @brief Makes a fresh, empty queue. Each call must yield an independent store.
using QueueFactory = std::function<std::unique_ptr<morph::offline::IOfflineQueue>()>;

namespace detail {

/// @brief Returns the pending item carrying @p itemId, or `std::nullopt`.
/// @param queue  Queue to inspect.
/// @param itemId Id to look for.
/// @return The matching item, or `std::nullopt` if no pending item has that id.
inline std::optional<morph::offline::QueueItem> itemById(const morph::offline::IOfflineQueue& queue,
                                                         std::uint64_t itemId) {
    for (auto const& item : queue.drain()) {
        if (item.id == itemId) {
            return item;
        }
    }
    return std::nullopt;
}

}  // namespace detail

/// @brief Asserts the `IOfflineQueue` idempotency-key contract on one implementation.
///
/// @param name     Implementation name, reported on failure.
/// @param declared The dedup policy this implementation is documented to have;
///                 asserted, not assumed.
/// @param make     Factory producing a fresh, empty queue.
inline void checkIdempotencyKeyContract(const std::string& name, KeyDedup declared, const QueueFactory& make) {
    INFO("implementation under test: " << name);

    // ── The declared dedup policy is the one the implementation actually has ──
    {
        auto queue = make();
        auto const first = queue->enqueue("payload-A", "K1");
        auto const second = queue->enqueue("payload-B", "K1");

        if (declared == KeyDedup::never) {
            INFO("declared KeyDedup::never, so the same key twice must produce two items");
            CHECK(first != second);
            CHECK(queue->size() == 2);
            auto const firstItem = detail::itemById(*queue, first);
            auto const secondItem = detail::itemById(*queue, second);
            REQUIRE(firstItem.has_value());
            REQUIRE(secondItem.has_value());
            CHECK(firstItem->payload == "payload-A");
            CHECK(secondItem->payload == "payload-B");
        } else {
            INFO("declared KeyDedup::onPendingItems, so the second enqueue must be a dedup hit");
            // A hit returns the *existing* item's id and stores nothing new.
            CHECK(second == first);
            CHECK(queue->size() == 1);
            auto const item = detail::itemById(*queue, first);
            REQUIRE(item.has_value());
            // First-write-wins: the second payload is discarded, with no error
            // and no signal to the caller. Callers must not re-enqueue a
            // corrected payload under an unchanged key.
            CHECK(item->payload == "payload-A");
        }
    }

    // ── An empty key is never a dedup token, in any implementation ──
    {
        auto queue = make();
        auto const first = queue->enqueue("payload-A", "");
        auto const second = queue->enqueue("payload-B", "");
        INFO("an empty idempotencyKey must never deduplicate");
        CHECK(first != second);
        CHECK(queue->size() == 2);
    }

    // ── The one-arg enqueue is equivalent to an empty key, never deduped ──
    {
        auto queue = make();
        auto const first = queue->enqueue("payload-A");
        auto const second = queue->enqueue("payload-A");
        INFO("the one-arg enqueue carries no key and must never deduplicate");
        CHECK(first != second);
        CHECK(queue->size() == 2);
    }

    // ── A returned id always resolves to a pending item carrying the key ──
    {
        auto queue = make();
        auto const itemId = queue->enqueue("payload-A", "K1");
        auto const item = detail::itemById(*queue, itemId);
        INFO("enqueue's returned id must name a pending item, with the key stored on it");
        REQUIRE(item.has_value());
        CHECK(item->payload == "payload-A");
        CHECK(item->idempotencyKey == "K1");
    }

    // ── markDone releases the key for re-enqueue ──
    {
        auto queue = make();
        auto const first = queue->enqueue("payload-A", "K1");
        queue->markDone(first);
        REQUIRE(queue->size() == 0);
        auto const second = queue->enqueue("payload-B", "K1");
        INFO("after markDone, the key is free and the new payload must be stored");
        CHECK(queue->size() == 1);
        auto const item = detail::itemById(*queue, second);
        REQUIRE(item.has_value());
        CHECK(item->payload == "payload-B");
    }

    // ── Pending items are presented in enqueue order ──
    {
        auto queue = make();
        queue->enqueue("payload-A", "K1");
        queue->enqueue("payload-B", "K2");
        queue->enqueue("payload-C", "K3");
        auto const items = queue->drain();
        INFO("drain must present pending items in enqueue order");
        REQUIRE(items.size() == 3);
        CHECK(items[0].payload == "payload-A");
        CHECK(items[1].payload == "payload-B");
        CHECK(items[2].payload == "payload-C");
    }
}

/// @brief Asserts that a durable implementation's key contract survives a reopen.
///
/// @param name     Implementation name, reported on failure.
/// @param declared The dedup policy this implementation is documented to have.
/// @param reopen   Factory that reopens the **same** store on every call.
inline void checkIdempotencyKeyContractAcrossReopen(const std::string& name, KeyDedup declared,
                                                    const QueueFactory& reopen) {
    INFO("implementation under test (across reopen): " << name);

    std::uint64_t firstId{};
    {
        auto queue = reopen();
        firstId = queue->enqueue("payload-A", "K1");
    }
    {
        auto queue = reopen();
        REQUIRE(queue->size() == 1);
        auto const second = queue->enqueue("payload-B", "K1");
        if (declared == KeyDedup::never) {
            CHECK(queue->size() == 2);
        } else {
            INFO("a durable dedup must still recognise the key after a restart");
            CHECK(queue->size() == 1);
            CHECK(second == firstId);
            auto const item = detail::itemById(*queue, firstId);
            REQUIRE(item.has_value());
            CHECK(item->payload == "payload-A");
        }
    }
}

}  // namespace morph::test
