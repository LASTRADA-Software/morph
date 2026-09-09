// SPDX-License-Identifier: Apache-2.0

/// @file
/// @brief Shared `IReplayLedger` conformance checks, run against every
///        implementation morph ships (morph#226).
///
/// Mirrors `tests/offline_queue_conformance.hpp`'s shape: a header of plain
/// functions rather than `TEST_CASE`s, so a new implementation — including one
/// living in an example rung, outside this repository's own test binary —
/// gets the whole suite by adding one call. Pins the interface-level
/// guarantees documented on `IReplayLedger`
/// (`include/morph/offline/replay_ledger.hpp`): the round trip, scope
/// isolation, the empty-`opId` floor, and first-write-wins.
///
/// This suite is a *negative* control target too: `tests/test_replay_ledger.cpp`
/// runs it against a stub whose `lookup`/`record` do nothing, tagged
/// `[!shouldfail]`, so a future edit that weakens these checks into something
/// a no-op ledger can pass is caught here rather than discovered later against
/// a real implementation.

#pragma once

#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <memory>
#include <morph/offline/replay_ledger.hpp>
#include <string>

namespace morph::test {

/// @brief Makes a fresh, empty ledger. Each call must yield an independent store.
using ReplayLedgerFactory = std::function<std::unique_ptr<morph::offline::IReplayLedger>()>;

/// @brief Asserts the full `IReplayLedger` contract on one implementation.
///
/// @param name Implementation name, reported on failure.
/// @param make Factory producing a fresh, empty ledger.
inline void checkReplayLedgerContract(const std::string& name, const ReplayLedgerFactory& make) {
    INFO("implementation under test: " << name);

    // ── An id never recorded is never reported as decided ──────────────────
    {
        auto ledger = make();
        CHECK_FALSE(ledger->lookup("scope", "never-seen").has_value());
    }

    // ── Round trip: record, then look the same (scope, opId) back up ───────
    {
        auto ledger = make();
        ledger->record("scope", "k", "payload");
        auto const hit = ledger->lookup("scope", "k");
        REQUIRE(hit.has_value());
        CHECK(*hit == "payload");
    }

    // ── Scope isolation: the same opId under a different scope is a miss ───
    {
        auto ledger = make();
        ledger->record("scope", "k", "payload");
        CHECK_FALSE(ledger->lookup("otherScope", "k").has_value());
    }

    // ── The empty scope is a valid, distinct partition, not "no scope" ─────
    {
        auto ledger = make();
        ledger->record("", "k", "bare-key payload");
        auto const bareHit = ledger->lookup("", "k");
        REQUIRE(bareHit.has_value());
        CHECK(*bareHit == "bare-key payload");
        // Recording under a real scope with the same id must not collide with
        // the empty-scope entry above -- confirms the empty scope is compared
        // like any other string, not treated as a wildcard.
        CHECK_FALSE(ledger->lookup("scope", "k").has_value());
    }

    // ── An empty opId is never reported as decided, even after record() ────
    //
    // The interface's own floor: an empty id is not an identity, so recording
    // one must not make a later lookup for the same (scope, "") report a hit.
    {
        auto ledger = make();
        CHECK_FALSE(ledger->lookup("scope", "").has_value());
        ledger->record("scope", "", "should never be stored");
        CHECK_FALSE(ledger->lookup("scope", "").has_value());
    }

    // ── A skip-only caller's empty payload round-trips as a hit, not a miss ─
    //
    // `lookup()`'s own doc comment: the returned value is engaged (a hit) even
    // when the payload itself is empty -- a skip-only consumer's whole point
    // is "was this seen", never looking at the string. `has_value()` is the
    // one bit that must never be conflated with "the string happens to be
    // empty".
    {
        auto ledger = make();
        ledger->record("scope", "k", "");
        auto const hit = ledger->lookup("scope", "k");
        REQUIRE(hit.has_value());
        CHECK(hit->empty());
    }

    // ── record() is idempotent: first-write-wins, not last-write-wins ──────
    {
        auto ledger = make();
        ledger->record("scope", "k", "first");
        ledger->record("scope", "k", "second");
        auto const hit = ledger->lookup("scope", "k");
        REQUIRE(hit.has_value());
        CHECK(*hit == "first");
    }
}

}  // namespace morph::test
