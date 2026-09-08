// SPDX-License-Identifier: Apache-2.0
//
// morph::offline::IReplayLedger (morph#226): the op-id/exactly-once replay
// ledger promoted out of seven near-identical hand-written copies across five
// example rungs. See include/morph/offline/replay_ledger.hpp for the
// interface's own rationale and docs/spec/offline/offline.md for the
// disposition.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/offline/replay_ledger.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "replay_ledger_conformance.hpp"

namespace {

// A ledger that answers "never seen, never recorded" to everything -- the
// negative control invariant 7 asks every gate in this repository to carry:
// a check that cannot fail is not a check. See the [!shouldfail] case below.
class NoOpReplayLedger : public morph::offline::IReplayLedger {
protected:
    [[nodiscard]] std::optional<std::string> doLookup(std::string_view, std::string_view) const override {
        return std::nullopt;
    }
    void doRecord(std::string_view, std::string_view, std::string) override {}
};

}  // namespace

TEST_CASE("morph::offline::InMemoryReplayLedger: IReplayLedger conformance", "[replay_ledger]") {
    morph::test::checkReplayLedgerContract("InMemoryReplayLedger",
                                           [] { return std::make_unique<morph::offline::InMemoryReplayLedger>(); });
}

// Invariant 7's negative control: a ledger whose lookup always reports
// "not decided" and whose record does nothing must make the conformance suite
// fail, not silently look conformant. Tagged [!shouldfail] -- Catch2 reports
// this case as PASSING the overall run precisely because its assertions fail,
// and would report the run as failing if this stub ever started satisfying
// the suite (the suite itself weakened into checking nothing).
TEST_CASE("morph::test::checkReplayLedgerContract rejects a no-op ledger", "[replay_ledger][!shouldfail]") {
    morph::test::checkReplayLedgerContract("NoOpReplayLedger", [] { return std::make_unique<NoOpReplayLedger>(); });
}

TEST_CASE("InMemoryReplayLedger: two independent instances do not share state", "[replay_ledger]") {
    morph::offline::InMemoryReplayLedger first;
    morph::offline::InMemoryReplayLedger second;
    first.record("scope", "k", "payload");
    CHECK(first.lookup("scope", "k").has_value());
    CHECK_FALSE(second.lookup("scope", "k").has_value());
}

TEST_CASE("InMemoryReplayLedger: distinct opIds under the same scope do not collide", "[replay_ledger]") {
    morph::offline::InMemoryReplayLedger ledger;
    ledger.record("scope", "a", "payload-a");
    ledger.record("scope", "b", "payload-b");
    REQUIRE(ledger.lookup("scope", "a").has_value());
    REQUIRE(ledger.lookup("scope", "b").has_value());
    CHECK(*ledger.lookup("scope", "a") == "payload-a");
    CHECK(*ledger.lookup("scope", "b") == "payload-b");
}
