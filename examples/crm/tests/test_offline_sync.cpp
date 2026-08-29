// SPDX-License-Identifier: Apache-2.0
//
// Offline field edits (README build order §8) — the rung's centrepiece.
//
// Same definition of done as lims's §7, restated for crm's own entity: two
// field reps edit the same opportunity offline; reconnect flags exactly the
// stale-base edit as a conflict. Both halves are load-bearing — *exactly*
// the stale one, and *flagged* rather than merged or dropped — and both are
// asserted below.
//
// The suite runs its whole conflict matrix twice where the queue matters:
// once against `InMemoryOfflineQueue` and once against the durable
// `SqliteOfflineQueue`, matching `examples/lims/tests/test_offline_capture.cpp`'s
// own two-legged structure exactly (its own comment explains why the durable
// leg is opt-in — MORPH_BUILD_OFFLINE_SQLITE, off by default).

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/journal/action_log.hpp>
#include <morph/offline/offline_queue.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <vector>

#include "crm/core/errors.hpp"
#include "crm/models/account_model.hpp"
#include "crm/models/opportunity_model.hpp"
#include "crm/offline/field_outbox.hpp"
#include "crm_test_support.hpp"
#include "testkit/db_fixture.hpp"

#ifdef MORPH_LADDER_HAVE_OFFLINE_SQLITE
#include <filesystem>
#include <morph/offline/sqlite_offline_queue.hpp>
#endif

using crm::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;
using morph::math::DecimalPlaces;
using morph::math::Rational;

namespace {

/// @brief An exact USD value at 2 decimal places.
/// @param cents The value, in whole cents (e.g. 500000 == $5,000.00).
/// @return The corresponding `Money`.
[[nodiscard]] crm::Money usd(std::int64_t cents) { return crm::Money{Rational{cents, DecimalPlaces{2}}}; }

/// @brief The deal, as far as §8 is concerned: one account, one opportunity,
///        both created under `alice`.
struct Deal {
    crm::AccountModel accounts;
    crm::OpportunityModel server;
    crm::AccountId accountId;
    crm::OpportunityView opportunity;

    Deal() {
        accountId = accounts.execute(crm::CreateAccount{.name = "Globex", .industry = "", .website = ""}).accountId;
        const auto created = server.execute(crm::CreateOpportunity{
            .account = crm::OpportunityAccountChoice{std::to_string(*accountId)},
            .primaryContact = {},
            .name = "Globex — renewal",
        });
        opportunity = server.execute(crm::GetOpportunity{.opportunityId = created.opportunityId});
    }
};

/// @brief Replays @p queue against a fresh handler, as @p principal.
///
/// A fresh `OpportunityModel` on purpose — same reasoning as
/// `test_offline_capture.cpp`'s own `reconnect()`: `Bridge::switchBackend`
/// reconstructs the model on the new backend and fires `onBackendChanged()`
/// on *that* instance (docs/spec/core/bridge.md), so a replay that only
/// worked on an already-attached instance would be testing the wrong thing.
/// Calls `onBackendChanged()` directly, from a thread with a session
/// installed — the same documented divergence from the real
/// `switchBackend`-posted path that `test_offline_capture.cpp`'s own
/// `reconnect()` doc comment names (morph#201): what this exercises is the
/// *classification* logic, the same code the supported path runs.
/// @param queue The queue to drain.
/// @param principal The reconnecting operator.
/// @param log Optional journal to attach before replaying.
void reconnect(const std::shared_ptr<morph::offline::IOfflineQueue>& queue, const std::string& principal,
               const std::shared_ptr<morph::journal::IActionLog>& log = nullptr) {
    const ScopedPrincipal session{principal};
    crm::OpportunityModel replaying;
    if (log) {
        replaying.attachActionLog(log, std::string{});
    }
    replaying.attachOfflineQueue(queue);
    replaying.onBackendChanged();
}

}  // namespace

// ── The write path: stamping and chaining ──────────────────────────────────

TEST_CASE("A field outbox stamps each update with the version it was prepared against", "[crm][offline]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    Deal deal;

    auto queue = std::make_shared<morph::offline::InMemoryOfflineQueue>();
    crm::offline::FieldOutbox outbox{queue, "fiona"};
    outbox.observe(deal.opportunity);

    const auto queued =
        outbox.enqueue(deal.opportunity.id, crm::OpportunityAccountChoice{std::to_string(*deal.accountId)},
                       crm::PrimaryContactChoice{}, "Globex — renewal (updated)", usd(500000));
    CHECK(queued.baseVersion == deal.opportunity.version);
    CHECK(queued.capturedBy == "fiona");
    CHECK_FALSE((*queued.operationKey).empty());
    CHECK(queue->size() == 1);

    // The payload really is what replay will decode, not a paraphrase of it.
    const auto items = queue->drain();
    REQUIRE(items.size() == 1);
    const auto decoded = morph::model::ActionTraits<crm::QueuedOpportunityUpdate>::fromJson(items[0].payload);
    CHECK(decoded.opportunityId == queued.opportunityId);
    CHECK(decoded.baseVersion == queued.baseVersion);
    CHECK(decoded.capturedBy == "fiona");
    CHECK(decoded.name == queued.name);
    // The same dedup token in both places: the queue's own slot and the payload.
    CHECK(items[0].idempotencyKey == *queued.operationKey);
}

TEST_CASE("A client's second offline edit chains onto its own first, not onto server state",
          "[crm][offline][self-conflict]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    Deal deal;
    const auto accountChoice = crm::OpportunityAccountChoice{std::to_string(*deal.accountId)};

    auto queue = std::make_shared<morph::offline::InMemoryOfflineQueue>();
    crm::offline::FieldOutbox outbox{queue, "fiona"};
    outbox.observe(deal.opportunity);

    const auto first =
        outbox.enqueue(deal.opportunity.id, accountChoice, crm::PrimaryContactChoice{}, "First edit", usd(100000));
    const auto second =
        outbox.enqueue(deal.opportunity.id, accountChoice, crm::PrimaryContactChoice{}, "Second edit", usd(200000));

    // The ODK trap, avoided: the second update's base is the version the
    // *first queued* update will produce, not the version the server last
    // showed this client.
    CHECK(first.baseVersion == deal.opportunity.version);
    CHECK(second.baseVersion == first.baseVersion + 1);
    CHECK(outbox.localVersion(deal.opportunity.id) == second.baseVersion + 1);

    reconnect(queue, "fiona");

    // Both landed; neither was flagged. A client must never conflict with itself.
    crm::OpportunityModel reader;
    CHECK(reader.execute(crm::ListConflicts{.opportunityId = deal.opportunity.id}).conflicts.empty());
    const auto view = reader.execute(crm::GetOpportunity{.opportunityId = deal.opportunity.id});
    CHECK(view.name == "Second edit");  // the *second* edit's value stands
    CHECK(view.version == deal.opportunity.version + 2);
}

TEST_CASE("A client that does not chain flags its own second edit as a conflict", "[crm][offline][self-conflict]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    Deal deal;
    const auto accountChoice = crm::OpportunityAccountChoice{std::to_string(*deal.accountId)};

    auto queue = std::make_shared<morph::offline::InMemoryOfflineQueue>();

    // The bug ODK hit, reproduced deliberately: stamp both edits with the
    // version the server last reported, because the client did not model its
    // own pending write. Two separate outboxes, each freshly observing the
    // same server state, is exactly that mistake.
    crm::offline::FieldOutbox naiveFirst{queue, "fiona"};
    naiveFirst.observe(deal.opportunity);
    const auto first =
        naiveFirst.enqueue(deal.opportunity.id, accountChoice, crm::PrimaryContactChoice{}, "First", usd(100000));

    crm::offline::FieldOutbox naiveSecond{queue, "fiona"};
    naiveSecond.observe(deal.opportunity);
    const auto second =
        naiveSecond.enqueue(deal.opportunity.id, accountChoice, crm::PrimaryContactChoice{}, "Second", usd(200000));
    REQUIRE(first.baseVersion == second.baseVersion);

    reconnect(queue, "fiona");

    crm::OpportunityModel reader;
    const auto conflicts = reader.execute(crm::ListConflicts{.opportunityId = deal.opportunity.id}).conflicts;
    REQUIRE(conflicts.size() == 1);
    CHECK(conflicts[0].reason == crm::ConflictReason::StaleBase);
    // The *second* edit is the one flagged; the first landed.
    CHECK(conflicts[0].baseVersion == second.baseVersion);
    CHECK(conflicts[0].serverVersion == second.baseVersion + 1);
    const auto view = reader.execute(crm::GetOpportunity{.opportunityId = deal.opportunity.id});
    CHECK(view.name == "First");
}

// ── The definition of done ─────────────────────────────────────────────────

TEST_CASE("Two field reps, one opportunity: reconnect flags exactly the stale-base update", "[crm][offline][dod]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    Deal deal;
    const auto accountChoice = crm::OpportunityAccountChoice{std::to_string(*deal.accountId)};

    // Two devices, two operators, two queues — both last saw the same version.
    auto fionaQueue = std::make_shared<morph::offline::InMemoryOfflineQueue>();
    auto gerardQueue = std::make_shared<morph::offline::InMemoryOfflineQueue>();
    crm::offline::FieldOutbox fiona{fionaQueue, "fiona"};
    crm::offline::FieldOutbox gerard{gerardQueue, "gerard"};
    fiona.observe(deal.opportunity);
    gerard.observe(deal.opportunity);

    const auto fionaUpdate =
        fiona.enqueue(deal.opportunity.id, accountChoice, crm::PrimaryContactChoice{}, "Fiona's edit", usd(100000));
    const auto gerardUpdate =
        gerard.enqueue(deal.opportunity.id, accountChoice, crm::PrimaryContactChoice{}, "Gerard's edit", usd(999900));
    REQUIRE(fionaUpdate.baseVersion == gerardUpdate.baseVersion);

    // Fiona reaches signal first.
    reconnect(fionaQueue, "fiona");
    reconnect(gerardQueue, "gerard");

    crm::OpportunityModel reader;

    // *Exactly* one conflict, and it is Gerard's.
    const auto conflicts = reader.execute(crm::ListConflicts{.opportunityId = deal.opportunity.id}).conflicts;
    REQUIRE(conflicts.size() == 1);
    CHECK(conflicts[0].detectedBy == "gerard");
    CHECK(conflicts[0].reason == crm::ConflictReason::StaleBase);
    CHECK(conflicts[0].status == crm::ConflictStatus::Open);
    CHECK(conflicts[0].baseVersion == deal.opportunity.version);
    CHECK(conflicts[0].serverVersion == deal.opportunity.version + 1);

    // Flagged, not merged and not dropped: the server still holds Fiona's
    // edit, and Gerard's is preserved verbatim inside the conflict.
    const auto view = reader.execute(crm::GetOpportunity{.opportunityId = deal.opportunity.id});
    CHECK(view.name == "Fiona's edit");

    const auto preserved = morph::model::ActionTraits<crm::QueuedOpportunityUpdate>::fromJson(conflicts[0].payload);
    CHECK(preserved.capturedBy == "gerard");
    CHECK(preserved.name == "Gerard's edit");
}

// ── Resolution ─────────────────────────────────────────────────────────────

TEST_CASE("Discarding a conflict closes it and leaves the server's value standing", "[crm][offline][resolution]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    Deal deal;
    const auto accountChoice = crm::OpportunityAccountChoice{std::to_string(*deal.accountId)};

    auto queue = std::make_shared<morph::offline::InMemoryOfflineQueue>();
    crm::offline::FieldOutbox gerard{queue, "gerard"};
    gerard.observe(deal.opportunity);
    gerard.enqueue(deal.opportunity.id, accountChoice, crm::PrimaryContactChoice{}, "Gerard's edit", usd(999900));

    // Somebody else moves the deal on first.
    crm::OpportunityModel bench;
    bench.execute(crm::UpdateOpportunity{
        .opportunityId = deal.opportunity.id,
        .account = accountChoice,
        .primaryContact = {},
        .name = "Bench edit",
        .expectedVersion = deal.opportunity.version,
    });

    reconnect(queue, "gerard");

    crm::OpportunityModel reader;
    const auto conflicts = reader.execute(crm::ListConflicts{.opportunityId = deal.opportunity.id}).conflicts;
    REQUIRE(conflicts.size() == 1);

    const auto resolved = reader.execute(crm::ResolveConflict{.conflictId = conflicts[0].id,
                                                              .resolution = crm::ConflictResolution::DiscardStale,
                                                              .note = "duplicate edit from the same rep"});
    CHECK(resolved.status == crm::ConflictStatus::Discarded);
    CHECK(resolved.resolvedBy == "alice");
    CHECK(resolved.resolutionNote == "duplicate edit from the same rep");

    // The bench's value stands.
    const auto view = reader.execute(crm::GetOpportunity{.opportunityId = deal.opportunity.id});
    CHECK(view.name == "Bench edit");

    // One-shot: a second decision about the same conflict is refused.
    CHECK_THROWS_AS(reader.execute(crm::ResolveConflict{.conflictId = conflicts[0].id,
                                                        .resolution = crm::ConflictResolution::ApplyAnyway,
                                                        .note = "changed my mind"}),
                    crm::Conflict);
}

TEST_CASE("Applying a conflict anyway rebases it onto the current version", "[crm][offline][resolution]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    Deal deal;
    const auto accountChoice = crm::OpportunityAccountChoice{std::to_string(*deal.accountId)};

    auto queue = std::make_shared<morph::offline::InMemoryOfflineQueue>();
    crm::offline::FieldOutbox gerard{queue, "gerard"};
    gerard.observe(deal.opportunity);
    gerard.enqueue(deal.opportunity.id, accountChoice, crm::PrimaryContactChoice{}, "Gerard's edit", usd(999900));

    crm::OpportunityModel bench;
    bench.execute(crm::UpdateOpportunity{
        .opportunityId = deal.opportunity.id,
        .account = accountChoice,
        .primaryContact = {},
        .name = "Bench edit",
        .expectedVersion = deal.opportunity.version,
    });

    reconnect(queue, "gerard");

    crm::OpportunityModel reader;
    const auto conflicts = reader.execute(crm::ListConflicts{.opportunityId = deal.opportunity.id}).conflicts;
    REQUIRE(conflicts.size() == 1);
    const auto versionBefore = reader.execute(crm::GetOpportunity{.opportunityId = deal.opportunity.id}).version;

    const auto resolved = reader.execute(crm::ResolveConflict{.conflictId = conflicts[0].id,
                                                              .resolution = crm::ConflictResolution::ApplyAnyway,
                                                              .note = "field edit is the authoritative one"});
    CHECK(resolved.status == crm::ConflictStatus::Applied);

    // Gerard's edit now stands, rebased onto the current version.
    const auto view = reader.execute(crm::GetOpportunity{.opportunityId = deal.opportunity.id});
    CHECK(view.name == "Gerard's edit");
    CHECK(view.version == versionBefore + 1);
    CHECK(resolved.resolvedBy == "alice");
}

TEST_CASE("Resolving needs a stated reason and a conflict that exists", "[crm][offline][resolution]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::OpportunityModel model;

    CHECK_THROWS_AS(model.execute(crm::ResolveConflict{.conflictId = crm::ConflictId{1}}), crm::ValidationError);
    CHECK_THROWS_AS(model.execute(crm::ResolveConflict{.note = "no id"}), crm::ValidationError);
    CHECK_THROWS_AS(
        model.execute(crm::ResolveConflict{.conflictId = crm::ConflictId{424242}, .note = "no such conflict"}),
        crm::NotFound);
}

// ── Replay's own guards ─────────────────────────────────────────────────────

TEST_CASE("A queued update may only be replayed by the operator who captured it", "[crm][offline][audit]") {
    DbFixture fixture;
    crm::OpportunityId opportunityId;
    crm::QueuedOpportunityUpdate queued;
    auto queue = std::make_shared<morph::offline::InMemoryOfflineQueue>();
    {
        const ScopedPrincipal alice{"alice"};
        Deal deal;
        opportunityId = deal.opportunity.id;
        crm::offline::FieldOutbox fiona{queue, "fiona"};
        fiona.observe(deal.opportunity);
        queued = fiona.enqueue(deal.opportunity.id, crm::OpportunityAccountChoice{std::to_string(*deal.accountId)},
                               crm::PrimaryContactChoice{}, "Fiona's edit", usd(100000));
    }

    // No session at all: refused before anything else is even looked at.
    {
        crm::OpportunityModel replaying;
        CHECK_THROWS_AS(replaying.execute(queued), crm::EmptyPrincipalError);
    }

    // Somebody else's session: refused.
    {
        const ScopedPrincipal mallory{"mallory"};
        crm::OpportunityModel replaying;
        CHECK_THROWS_AS(replaying.execute(queued), crm::Forbidden);
    }

    const ScopedPrincipal alice{"alice"};
    crm::OpportunityModel reader;
    CHECK(reader.execute(crm::ListConflicts{.opportunityId = opportunityId}).conflicts.empty());
}

TEST_CASE("Redelivering an operation is skipped, not applied twice", "[crm][offline][idempotency]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    Deal deal;
    const auto accountChoice = crm::OpportunityAccountChoice{std::to_string(*deal.accountId)};

    auto queue = std::make_shared<morph::offline::InMemoryOfflineQueue>();
    crm::offline::FieldOutbox fiona{queue, "fiona"};
    fiona.observe(deal.opportunity);
    const auto queued =
        fiona.enqueue(deal.opportunity.id, accountChoice, crm::PrimaryContactChoice{}, "Fiona's edit", usd(100000));

    const ScopedPrincipal asFiona{"fiona"};
    crm::OpportunityModel replaying;
    const auto first = replaying.execute(queued);
    CHECK(first.outcome == crm::ReplayOutcome::Applied);

    // The same operation delivered again — a queue that did not dedup at
    // enqueue time, a retried replay, a journal replay. Without the
    // consumer's own at-most-once check this would bump the version a second
    // time and then start flagging the client's later edits as stale.
    const auto again = replaying.execute(queued);
    CHECK(again.outcome == crm::ReplayOutcome::Skipped);

    crm::OpportunityModel reader;
    const auto view = reader.execute(crm::GetOpportunity{.opportunityId = deal.opportunity.id});
    CHECK(view.version == deal.opportunity.version + 1);
    CHECK(reader.execute(crm::ListConflicts{.opportunityId = deal.opportunity.id}).conflicts.empty());
}

TEST_CASE("A conflict is decided once: redelivering it does not raise a second flag", "[crm][offline][idempotency]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    Deal deal;
    const auto accountChoice = crm::OpportunityAccountChoice{std::to_string(*deal.accountId)};

    auto queue = std::make_shared<morph::offline::InMemoryOfflineQueue>();
    crm::offline::FieldOutbox gerard{queue, "gerard"};
    gerard.observe(deal.opportunity);
    const auto queued =
        gerard.enqueue(deal.opportunity.id, accountChoice, crm::PrimaryContactChoice{}, "Gerard's edit", usd(999900));

    crm::OpportunityModel bench;
    bench.execute(crm::UpdateOpportunity{
        .opportunityId = deal.opportunity.id,
        .account = accountChoice,
        .primaryContact = {},
        .name = "Bench edit",
        .expectedVersion = deal.opportunity.version,
    });

    const ScopedPrincipal asGerard{"gerard"};
    crm::OpportunityModel replaying;
    CHECK(replaying.execute(queued).outcome == crm::ReplayOutcome::Conflicted);
    CHECK(replaying.execute(queued).outcome == crm::ReplayOutcome::Skipped);

    const ScopedPrincipal asAlice{"alice"};
    crm::OpportunityModel reader;
    CHECK(reader.execute(crm::ListConflicts{.opportunityId = deal.opportunity.id}).conflicts.size() == 1);
}

TEST_CASE("An update for an opportunity that closed meanwhile is flagged, with the specific reason",
          "[crm][offline]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    Deal deal;
    const auto accountChoice = crm::OpportunityAccountChoice{std::to_string(*deal.accountId)};

    auto queue = std::make_shared<morph::offline::InMemoryOfflineQueue>();
    crm::offline::FieldOutbox fiona{queue, "fiona"};
    fiona.observe(deal.opportunity);
    fiona.enqueue(deal.opportunity.id, accountChoice, crm::PrimaryContactChoice{}, "Fiona's edit", usd(100000));

    // The desk closes the deal while Fiona is out of signal.
    crm::OpportunityModel bench;
    bench.execute(
        crm::MoveOpportunityStage{.opportunityId = deal.opportunity.id, .stage = crm::OpportunityStage::Won});

    reconnect(queue, "fiona");

    crm::OpportunityModel reader;
    const auto conflicts = reader.execute(crm::ListConflicts{.opportunityId = deal.opportunity.id}).conflicts;
    REQUIRE(conflicts.size() == 1);
    // Not StaleBase: "this deal closed while you were away" is the more
    // actionable thing to tell a human, and both are true.
    CHECK(conflicts[0].reason == crm::ConflictReason::LifecycleClosed);
    CHECK(reader.execute(crm::GetOpportunity{.opportunityId = deal.opportunity.id}).name != "Fiona's edit");
}

TEST_CASE("An undecodable queued payload is journaled and dropped, never left to block the queue",
          "[crm][offline][audit]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    Deal deal;
    const auto accountChoice = crm::OpportunityAccountChoice{std::to_string(*deal.accountId)};

    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    auto queue = std::make_shared<morph::offline::InMemoryOfflineQueue>();
    queue->enqueue("{ this is not json");

    crm::offline::FieldOutbox fiona{queue, "fiona"};
    fiona.observe(deal.opportunity);
    fiona.enqueue(deal.opportunity.id, accountChoice, crm::PrimaryContactChoice{}, "Fiona's edit", usd(100000));

    reconnect(queue, "fiona", log);

    // The queue is empty: the bad item did not block the good one behind it.
    CHECK(queue->size() == 0);

    // And it was not lost silently.
    bool sawRejection = false;
    for (const auto& entry : log->entries()) {
        if (entry.outcome == morph::journal::Outcome::Failed && entry.payload == "{ this is not json") {
            sawRejection = true;
            CHECK(entry.actionType == "QueuedOpportunityUpdate");
            CHECK_FALSE(entry.error.empty());
        }
    }
    CHECK(sawRejection);

    // The good item still landed.
    crm::OpportunityModel reader;
    CHECK(reader.execute(crm::GetOpportunity{.opportunityId = deal.opportunity.id}).name == "Fiona's edit");
}

TEST_CASE("A model with no queue attached replays nothing and does not crash", "[crm][offline]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::OpportunityModel model;
    model.onBackendChanged();
    SUCCEED("onBackendChanged() with no queue attached is a no-op");
}

TEST_CASE("A malformed queued envelope is rejected by validate(), not stored", "[crm][offline]") {
    DbFixture fixture;
    const ScopedPrincipal fiona{"fiona"};
    crm::OpportunityModel model;

    // No opportunity named.
    CHECK_THROWS_AS(
        model.execute(crm::QueuedOpportunityUpdate{.capturedBy = "fiona",
                                                   .operationKey = crm::OperationKey{"k"},
                                                   .account = crm::OpportunityAccountChoice{std::string{"1"}},
                                                   .name = "x"}),
        crm::ValidationError);
    // No dedup token: replay could not enforce at-most-once for it.
    CHECK_THROWS_AS(
        model.execute(crm::QueuedOpportunityUpdate{.opportunityId = crm::OpportunityId{1},
                                                   .capturedBy = "fiona",
                                                   .account = crm::OpportunityAccountChoice{std::string{"1"}},
                                                   .name = "x"}),
        crm::ValidationError);
}

#ifdef MORPH_LADDER_HAVE_OFFLINE_SQLITE

// ── The durable leg ────────────────────────────────────────────────────────
//
// Everything above runs against InMemoryOfflineQueue. A field rep does not:
// they use a queue that survives the device being switched off, which is the
// whole reason the queue exists. This block runs the same shape against the
// real durable one, including across a simulated restart.

namespace {

/// @brief A fresh on-disk queue path for one test case.
/// @param name Distinguishes concurrent cases in the same directory.
/// @return The path, with any previous file removed.
[[nodiscard]] std::filesystem::path freshQueuePath(const std::string& name) {
    auto path = std::filesystem::temp_directory_path() / ("crm_field_queue_" + name + ".sqlite");
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + "-wal");
    std::filesystem::remove(path.string() + "-shm");
    return path;
}

}  // namespace

TEST_CASE("The definition-of-done flow holds on the durable queue, across a restart", "[crm][offline][dod][sqlite]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    Deal deal;
    const auto accountChoice = crm::OpportunityAccountChoice{std::to_string(*deal.accountId)};

    const auto fionaPath = freshQueuePath("fiona");
    const auto gerardPath = freshQueuePath("gerard");

    {
        // Out in the field: two devices queue against the same version, then
        // both are switched off (the queue objects go out of scope).
        auto fionaQueue = std::make_shared<morph::offline::SqliteOfflineQueue>(fionaPath);
        auto gerardQueue = std::make_shared<morph::offline::SqliteOfflineQueue>(gerardPath);
        crm::offline::FieldOutbox fiona{fionaQueue, "fiona"};
        crm::offline::FieldOutbox gerard{gerardQueue, "gerard"};
        fiona.observe(deal.opportunity);
        gerard.observe(deal.opportunity);
        fiona.enqueue(deal.opportunity.id, accountChoice, crm::PrimaryContactChoice{}, "Fiona's edit", usd(100000));
        gerard.enqueue(deal.opportunity.id, accountChoice, crm::PrimaryContactChoice{}, "Gerard's edit", usd(999900));
        CHECK(fionaQueue->size() == 1);
        CHECK(gerardQueue->size() == 1);
    }

    // Back at the office, fresh queue objects over the same files.
    auto fionaQueue = std::make_shared<morph::offline::SqliteOfflineQueue>(fionaPath);
    auto gerardQueue = std::make_shared<morph::offline::SqliteOfflineQueue>(gerardPath);
    REQUIRE(fionaQueue->size() == 1);
    REQUIRE(gerardQueue->size() == 1);

    reconnect(fionaQueue, "fiona");
    reconnect(gerardQueue, "gerard");

    // Both queues are drained, exactly one conflict was flagged, and it is
    // Gerard's — the same outcome as the in-memory leg.
    CHECK(fionaQueue->size() == 0);
    CHECK(gerardQueue->size() == 0);

    crm::OpportunityModel reader;
    const auto conflicts = reader.execute(crm::ListConflicts{.opportunityId = deal.opportunity.id}).conflicts;
    REQUIRE(conflicts.size() == 1);
    CHECK(conflicts[0].detectedBy == "gerard");
    CHECK(conflicts[0].reason == crm::ConflictReason::StaleBase);

    const auto view = reader.execute(crm::GetOpportunity{.opportunityId = deal.opportunity.id});
    CHECK(view.name == "Fiona's edit");
}

#endif  // MORPH_LADDER_HAVE_OFFLINE_SQLITE
