// SPDX-License-Identifier: Apache-2.0
//
// Offline field capture (README build order §7) — the rung's centrepiece.
//
// The definition of done, verbatim: "two field clients update the same sample
// offline; reconnect flags exactly the stale-base update as a conflict." Both
// halves of that sentence are load-bearing — *exactly* the stale one, and
// *flagged* rather than merged or dropped — and both are asserted below.
//
// The suite runs its whole conflict matrix twice where the queue matters:
// once against `InMemoryOfflineQueue` and once against the durable
// `SqliteOfflineQueue`, which is the queue a real field client would use. The
// second leg only compiles when the build enabled MORPH_BUILD_OFFLINE_SQLITE
// (see the MORPH_LADDER_HAVE_OFFLINE_SQLITE block at the bottom of this file);
// it is off in a default configure, so the durable leg is opt-in.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/journal/action_log.hpp>
#include <morph/offline/offline_queue.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <vector>

#include "lims/core/errors.hpp"
#include "lims/models/analysis_catalog_model.hpp"
#include "lims/models/sample_model.hpp"
#include "lims/offline/field_outbox.hpp"
#include "lims_test_support.hpp"
#include "testkit/db_fixture.hpp"

#ifdef MORPH_LADDER_HAVE_OFFLINE_SQLITE
#include <filesystem>
#include <morph/offline/sqlite_offline_queue.hpp>
#endif

using lims::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;
using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

namespace {

/// @brief An exact rational at @p places decimal places.
/// @param num Numerator.
/// @param den Denominator.
/// @param places Decimal-precision tag.
/// @return The canonical rational.
[[nodiscard]] Rational exact(std::int64_t num, std::int64_t den, std::uint32_t places) {
    return Rational{Numerator{num}, Denominator{den}, DecimalPlaces{places}};
}

/// @brief A measured concentration capture against @p versionId.
/// @param versionId The analysis version captured under.
/// @param num Numerator of the reading, over 1000 (i.e. millis of mg/L).
/// @return The capture action.
[[nodiscard]] lims::CaptureConcentration reading(lims::AnalysisVersionId versionId, std::int64_t num) {
    return lims::CaptureConcentration{.analysisVersionId = versionId,
                                      .value = lims::Concentration{exact(num, 1000, 3)}};
}

/// @brief The lab, as far as §7 is concerned: one analysis, one sample at
///        `InProgress`, both created under `alice`.
struct Lab {
    lims::AnalysisCatalogModel catalog;
    lims::SampleModel server;
    lims::AnalysisVersionId versionId;
    lims::SampleView sample;

    Lab() {
        versionId =
            catalog.execute(lims::DefineAnalysis{.name = "Nitrate", .canonicalUnit = "mg_per_L", .decimalPlaces = 3})
                .versionId;
        const auto client = server.execute(lims::RegisterClient{.name = "Waterworks Ltd"});
        server.execute(lims::RegisterSample{.clientId = client.clientId, .reference = "WW-1"});
        server.execute(lims::ReceiveSample{});
        sample = server.execute(lims::StartWork{});
    }
};

/// @brief Replays @p queue against a fresh handler, as @p principal.
///
/// A fresh `SampleModel` on purpose: `Bridge::switchBackend` reconstructs the
/// model on the new backend and fires `onBackendChanged()` on *that* instance
/// (docs/spec/core/bridge.md), so a replay that only worked on the instance
/// that happened to be attached already would be testing the wrong thing.
///
/// @warning This calls `onBackendChanged()` **directly**, from a thread that
/// has a session installed. `switchBackend` does not: it posts the call onto
/// the model's own strand, where `session::current()` is null, and every item
/// is then refused for want of a principal. That is pinned separately, in
/// `test_backend_matrix.cpp`'s "onBackendChanged fires on switchBackend, and
/// fails closed with no session", and filed as morph#201. What this
/// helper exercises is the *classification* logic — base-version comparison,
/// conflict flagging, at-most-once — which is the same code the supported
/// re-dispatch path runs, and which `test_backend_matrix.cpp` also drives
/// through a real `Bridge` in all three deployment modes.
/// @param queue The queue to drain.
/// @param principal The reconnecting operator.
/// @param log Optional journal to attach before replaying.
void reconnect(const std::shared_ptr<morph::offline::IOfflineQueue>& queue, const std::string& principal,
               const std::shared_ptr<morph::journal::IActionLog>& log = nullptr) {
    const ScopedPrincipal session{principal};
    lims::SampleModel replaying;
    if (log) {
        replaying.attachActionLog(log, std::string{});
    }
    replaying.attachOfflineQueue(queue);
    replaying.onBackendChanged();
}

/// @brief Opens a handler onto @p sampleId so its results/conflicts can be read.
/// @param model The handler to attach.
/// @param sampleId The sample to attach to.
void open(lims::SampleModel& model, lims::SampleId sampleId) { model.execute(lims::OpenSample{.sampleId = sampleId}); }

}  // namespace

// ── The write path: stamping and chaining ──────────────────────────────────

TEST_CASE("A field outbox stamps each update with the version it was prepared against", "[lims][offline]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    Lab lab;

    auto queue = std::make_shared<morph::offline::InMemoryOfflineQueue>();
    lims::offline::FieldOutbox outbox{queue, "fiona"};
    outbox.observe(lab.sample);

    const auto queued = outbox.enqueue(lab.sample.id, reading(lab.versionId, 2400));
    CHECK(*queued.baseVersion == *lab.sample.version);
    CHECK(queued.capturedBy == "fiona");
    CHECK_FALSE((*queued.operationKey).empty());
    CHECK(queue->size() == 1);

    // The payload really is what replay will decode, not a paraphrase of it.
    const auto items = queue->drain();
    REQUIRE(items.size() == 1);
    const auto decoded = morph::model::ActionTraits<lims::QueuedCapture>::fromJson(items[0].payload);
    CHECK(decoded.sampleId == queued.sampleId);
    CHECK(*decoded.baseVersion == *queued.baseVersion);
    CHECK(decoded.capturedBy == "fiona");
    CHECK(decoded.capture.value == queued.capture.value);
    // The same dedup token in both places: the queue's own slot and the payload.
    CHECK(items[0].idempotencyKey == *queued.operationKey);
}

TEST_CASE("A client's second offline edit chains onto its own first, not onto server state",
          "[lims][offline][self-conflict]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    Lab lab;

    auto queue = std::make_shared<morph::offline::InMemoryOfflineQueue>();
    lims::offline::FieldOutbox outbox{queue, "fiona"};
    outbox.observe(lab.sample);

    const auto first = outbox.enqueue(lab.sample.id, reading(lab.versionId, 2400));
    const auto second = outbox.enqueue(lab.sample.id, reading(lab.versionId, 2500));

    // This is the ODK trap, and the assertion that it is avoided: the second
    // update's base is the version the *first queued* update will produce, not
    // the version the server last showed this client.
    CHECK(*first.baseVersion == *lab.sample.version);
    CHECK(*second.baseVersion == *first.baseVersion + 1);
    CHECK(*outbox.localVersion(lab.sample.id) == *second.baseVersion + 1);

    reconnect(queue, "fiona");

    // Both landed; neither was flagged. A client must never conflict with itself.
    lims::SampleModel reader;
    open(reader, lab.sample.id);
    CHECK(reader.execute(lims::ListConflicts{}).conflicts.empty());
    const auto results = reader.execute(lims::ListResults{});
    REQUIRE(results.results.size() == 1);
    CHECK((*results.results[0].value).numerator == 5);  // 2500/1000 == 5/2, the *second* edit
    CHECK((*results.results[0].value).denominator == 2);
    CHECK(*reader.execute(lims::GetSample{}).version == *lab.sample.version + 2);
}

TEST_CASE("A client that does not chain flags its own second edit as a conflict", "[lims][offline][self-conflict]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    Lab lab;

    auto queue = std::make_shared<morph::offline::InMemoryOfflineQueue>();

    // The bug ODK hit, reproduced deliberately: stamp both edits with the
    // version the server last reported, because the client did not model its
    // own pending write. Two separate outboxes, each freshly observing the
    // same server state, is exactly that mistake.
    lims::offline::FieldOutbox naiveFirst{queue, "fiona"};
    naiveFirst.observe(lab.sample);
    const auto first = naiveFirst.enqueue(lab.sample.id, reading(lab.versionId, 2400));

    lims::offline::FieldOutbox naiveSecond{queue, "fiona"};
    naiveSecond.observe(lab.sample);
    const auto second = naiveSecond.enqueue(lab.sample.id, reading(lab.versionId, 2500));
    REQUIRE(*first.baseVersion == *second.baseVersion);

    reconnect(queue, "fiona");

    lims::SampleModel reader;
    open(reader, lab.sample.id);
    const auto conflicts = reader.execute(lims::ListConflicts{}).conflicts;
    REQUIRE(conflicts.size() == 1);
    CHECK(conflicts[0].reason == lims::ConflictReason::StaleBase);
    // The *second* edit is the one flagged; the first landed.
    CHECK(*conflicts[0].baseVersion == *second.baseVersion);
    CHECK(*conflicts[0].serverVersion == *second.baseVersion + 1);
    const auto results = reader.execute(lims::ListResults{});
    REQUIRE(results.results.size() == 1);
    CHECK((*results.results[0].value).numerator == 12);  // 2400/1000 == 12/5, the first edit
    CHECK((*results.results[0].value).denominator == 5);
}

// ── The definition of done ─────────────────────────────────────────────────

TEST_CASE("Two field clients, one sample: reconnect flags exactly the stale-base update", "[lims][offline][dod]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    Lab lab;

    // Two devices, two operators, two queues — both last saw the same version.
    auto fionaQueue = std::make_shared<morph::offline::InMemoryOfflineQueue>();
    auto gerardQueue = std::make_shared<morph::offline::InMemoryOfflineQueue>();
    lims::offline::FieldOutbox fiona{fionaQueue, "fiona"};
    lims::offline::FieldOutbox gerard{gerardQueue, "gerard"};
    fiona.observe(lab.sample);
    gerard.observe(lab.sample);

    const auto fionaUpdate = fiona.enqueue(lab.sample.id, reading(lab.versionId, 2400));
    const auto gerardUpdate = gerard.enqueue(lab.sample.id, reading(lab.versionId, 9900));
    REQUIRE(*fionaUpdate.baseVersion == *gerardUpdate.baseVersion);

    // Fiona reaches signal first.
    reconnect(fionaQueue, "fiona");
    reconnect(gerardQueue, "gerard");

    lims::SampleModel reader;
    open(reader, lab.sample.id);

    // *Exactly* one conflict, and it is Gerard's.
    const auto conflicts = reader.execute(lims::ListConflicts{}).conflicts;
    REQUIRE(conflicts.size() == 1);
    CHECK(conflicts[0].detectedBy == "gerard");
    CHECK(conflicts[0].reason == lims::ConflictReason::StaleBase);
    CHECK(conflicts[0].status == lims::ConflictStatus::Open);
    CHECK(*conflicts[0].baseVersion == *lab.sample.version);
    CHECK(*conflicts[0].serverVersion == *lab.sample.version + 1);

    // Flagged, not merged and not dropped: the server still holds Fiona's
    // reading, and Gerard's is preserved verbatim inside the conflict.
    const auto results = reader.execute(lims::ListResults{});
    REQUIRE(results.results.size() == 1);
    CHECK(results.results[0].capturedBy == "fiona");
    CHECK((*results.results[0].value).numerator == 12);
    CHECK((*results.results[0].value).denominator == 5);

    const auto preserved = morph::model::ActionTraits<lims::QueuedCapture>::fromJson(conflicts[0].payload);
    CHECK(preserved.capturedBy == "gerard");
    CHECK(preserved.capture.value == gerardUpdate.capture.value);
}

// ── Resolution ─────────────────────────────────────────────────────────────

TEST_CASE("Discarding a conflict closes it and leaves the server's value standing", "[lims][offline][resolution]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    Lab lab;

    auto queue = std::make_shared<morph::offline::InMemoryOfflineQueue>();
    lims::offline::FieldOutbox gerard{queue, "gerard"};
    gerard.observe(lab.sample);
    gerard.enqueue(lab.sample.id, reading(lab.versionId, 9900));

    // Somebody else moves the sample on first.
    lims::SampleModel bench;
    open(bench, lab.sample.id);
    bench.execute(reading(lab.versionId, 2400));

    reconnect(queue, "gerard");

    lims::SampleModel reader;
    open(reader, lab.sample.id);
    const auto conflicts = reader.execute(lims::ListConflicts{}).conflicts;
    REQUIRE(conflicts.size() == 1);

    const auto resolved = reader.execute(lims::ResolveConflict{.conflictId = conflicts[0].id,
                                                               .resolution = lims::ConflictResolution::DiscardStale,
                                                               .note = "duplicate sampled from the same bottle"});
    CHECK(resolved.status == lims::ConflictStatus::Discarded);
    CHECK(resolved.resolvedBy == "alice");
    CHECK(resolved.resolutionNote == "duplicate sampled from the same bottle");

    // The bench's value stands.
    const auto results = reader.execute(lims::ListResults{});
    REQUIRE(results.results.size() == 1);
    CHECK((*results.results[0].value).numerator == 12);

    // One-shot: a second decision about the same conflict is refused.
    CHECK_THROWS_AS(reader.execute(lims::ResolveConflict{.conflictId = conflicts[0].id,
                                                         .resolution = lims::ConflictResolution::ApplyAnyway,
                                                         .note = "changed my mind"}),
                    lims::Conflict);
}

TEST_CASE("Applying a conflict anyway rebases it onto the current version", "[lims][offline][resolution]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    Lab lab;

    auto queue = std::make_shared<morph::offline::InMemoryOfflineQueue>();
    lims::offline::FieldOutbox gerard{queue, "gerard"};
    gerard.observe(lab.sample);
    gerard.enqueue(lab.sample.id, reading(lab.versionId, 9900));

    lims::SampleModel bench;
    open(bench, lab.sample.id);
    bench.execute(reading(lab.versionId, 2400));

    reconnect(queue, "gerard");

    lims::SampleModel reader;
    open(reader, lab.sample.id);
    const auto conflicts = reader.execute(lims::ListConflicts{}).conflicts;
    REQUIRE(conflicts.size() == 1);
    const auto versionBefore = *reader.execute(lims::GetSample{}).version;

    const auto resolved = reader.execute(lims::ResolveConflict{.conflictId = conflicts[0].id,
                                                               .resolution = lims::ConflictResolution::ApplyAnyway,
                                                               .note = "field reading is the authoritative one"});
    CHECK(resolved.status == lims::ConflictStatus::Applied);

    // Gerard's reading now stands — and it is still attributed to Gerard, who
    // took it, not to Alice, who decided to keep it. The resolver is recorded
    // separately, which is what makes the trail answer both questions.
    const auto results = reader.execute(lims::ListResults{});
    REQUIRE(results.results.size() == 1);
    CHECK(results.results[0].capturedBy == "gerard");
    CHECK((*results.results[0].value).numerator == 99);
    CHECK((*results.results[0].value).denominator == 10);
    CHECK(resolved.resolvedBy == "alice");
    CHECK(*reader.execute(lims::GetSample{}).version == versionBefore + 1);
}

TEST_CASE("Resolving needs a stated reason and a conflict that exists", "[lims][offline][resolution]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::SampleModel model;

    CHECK_THROWS_AS(model.execute(lims::ResolveConflict{.conflictId = lims::ConflictId{1}}), lims::ValidationError);
    CHECK_THROWS_AS(model.execute(lims::ResolveConflict{.note = "no id"}), lims::ValidationError);
    CHECK_THROWS_AS(
        model.execute(lims::ResolveConflict{.conflictId = lims::ConflictId{424242}, .note = "no such conflict"}),
        lims::NotFound);
}

// ── Replay's own guards ────────────────────────────────────────────────────

TEST_CASE("A queued update may only be replayed by the operator who captured it", "[lims][offline][audit]") {
    DbFixture fixture;
    lims::SampleId sampleId;
    lims::QueuedCapture queued;
    auto queue = std::make_shared<morph::offline::InMemoryOfflineQueue>();
    {
        // Scoped so the session ends with it: the "no principal at all" case
        // below is only meaningful once alice's context is gone.
        const ScopedPrincipal alice{"alice"};
        Lab lab;
        sampleId = lab.sample.id;
        lims::offline::FieldOutbox fiona{queue, "fiona"};
        fiona.observe(lab.sample);
        queued = fiona.enqueue(lab.sample.id, reading(lab.versionId, 2400));
    }

    // No session at all: refused before anything else is even looked at.
    {
        lims::SampleModel replaying;
        CHECK_THROWS_AS(replaying.execute(queued), lims::EmptyPrincipalError);
    }

    // Somebody else's session: refused. A lab result filed under a colleague's
    // name is the failure a 21 CFR-style trail exists to prevent.
    {
        const ScopedPrincipal mallory{"mallory"};
        lims::SampleModel replaying;
        CHECK_THROWS_AS(replaying.execute(queued), lims::Forbidden);
    }

    const ScopedPrincipal alice{"alice"};
    lims::SampleModel reader;
    open(reader, sampleId);
    CHECK(reader.execute(lims::ListResults{}).results.empty());
    // Refusal is not a conflict: nothing was flagged for a human either.
    CHECK(reader.execute(lims::ListConflicts{}).conflicts.empty());
}

TEST_CASE("Redelivering an operation is skipped, not applied twice", "[lims][offline][idempotency]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    Lab lab;

    auto queue = std::make_shared<morph::offline::InMemoryOfflineQueue>();
    lims::offline::FieldOutbox fiona{queue, "fiona"};
    fiona.observe(lab.sample);
    const auto queued = fiona.enqueue(lab.sample.id, reading(lab.versionId, 2400));

    const ScopedPrincipal asFiona{"fiona"};
    lims::SampleModel replaying;
    const auto first = replaying.execute(queued);
    CHECK(first.outcome == lims::ReplayOutcome::Applied);

    // The same operation delivered again — a queue that did not dedup at
    // enqueue time, a retried replay, a journal replay. Without the consumer's
    // own at-most-once check this would bump the version a second time and
    // then start flagging the client's later updates as stale.
    const auto again = replaying.execute(queued);
    CHECK(again.outcome == lims::ReplayOutcome::Skipped);

    lims::SampleModel reader;
    open(reader, lab.sample.id);
    CHECK(reader.execute(lims::ListResults{}).results.size() == 1);
    CHECK(*reader.execute(lims::GetSample{}).version == *lab.sample.version + 1);
    CHECK(reader.execute(lims::ListConflicts{}).conflicts.empty());
}

TEST_CASE("A conflict is decided once: redelivering it does not raise a second flag", "[lims][offline][idempotency]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    Lab lab;

    auto queue = std::make_shared<morph::offline::InMemoryOfflineQueue>();
    lims::offline::FieldOutbox gerard{queue, "gerard"};
    gerard.observe(lab.sample);
    const auto queued = gerard.enqueue(lab.sample.id, reading(lab.versionId, 9900));

    lims::SampleModel bench;
    open(bench, lab.sample.id);
    bench.execute(reading(lab.versionId, 2400));

    const ScopedPrincipal asGerard{"gerard"};
    lims::SampleModel replaying;
    CHECK(replaying.execute(queued).outcome == lims::ReplayOutcome::Conflicted);
    CHECK(replaying.execute(queued).outcome == lims::ReplayOutcome::Skipped);

    lims::SampleModel reader;
    const ScopedPrincipal asAlice{"alice"};
    open(reader, lab.sample.id);
    CHECK(reader.execute(lims::ListConflicts{}).conflicts.size() == 1);
}

TEST_CASE("An update for a sample that left the bench is flagged, with the specific reason", "[lims][offline]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    Lab lab;

    auto queue = std::make_shared<morph::offline::InMemoryOfflineQueue>();
    lims::offline::FieldOutbox fiona{queue, "fiona"};
    fiona.observe(lab.sample);
    fiona.enqueue(lab.sample.id, reading(lab.versionId, 2400));

    // The lab submits the sample for verification while Fiona is out of signal.
    lims::SampleModel bench;
    open(bench, lab.sample.id);
    bench.execute(lims::SubmitForVerification{});

    reconnect(queue, "fiona");

    lims::SampleModel reader;
    open(reader, lab.sample.id);
    const auto conflicts = reader.execute(lims::ListConflicts{}).conflicts;
    REQUIRE(conflicts.size() == 1);
    // Not StaleBase: "somebody submitted this for verification while you were
    // away" is the more actionable thing to tell a human, and both are true.
    CHECK(conflicts[0].reason == lims::ConflictReason::LifecycleClosed);
    CHECK(reader.execute(lims::ListResults{}).results.empty());
}

TEST_CASE("An undecodable queued payload is journaled and dropped, never left to block the queue",
          "[lims][offline][audit]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    Lab lab;

    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    auto queue = std::make_shared<morph::offline::InMemoryOfflineQueue>();
    queue->enqueue("{ this is not json");

    lims::offline::FieldOutbox fiona{queue, "fiona"};
    fiona.observe(lab.sample);
    fiona.enqueue(lab.sample.id, reading(lab.versionId, 2400));

    reconnect(queue, "fiona", log);

    // The queue is empty: the bad item did not block the good one behind it.
    CHECK(queue->size() == 0);

    // And it was not lost silently — the payload this build could not read is
    // in the journal verbatim, which is the whole point of recording it.
    bool sawRejection = false;
    for (const auto& entry : log->entries()) {
        if (entry.outcome == morph::journal::Outcome::Failed && entry.payload == "{ this is not json") {
            sawRejection = true;
            CHECK(entry.actionType == "QueuedCapture");
            CHECK_FALSE(entry.error.empty());
        }
    }
    CHECK(sawRejection);

    // The good item still landed.
    lims::SampleModel reader;
    open(reader, lab.sample.id);
    CHECK(reader.execute(lims::ListResults{}).results.size() == 1);
}

TEST_CASE("A model with no queue attached replays nothing and does not crash", "[lims][offline]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::SampleModel model;
    model.onBackendChanged();
    SUCCEED("onBackendChanged() with no queue attached is a no-op");
}

TEST_CASE("A malformed queued envelope is rejected by validate(), not stored", "[lims][offline]") {
    DbFixture fixture;
    const ScopedPrincipal fiona{"fiona"};
    lims::SampleModel model;

    // No sample named.
    CHECK_THROWS_AS(model.execute(lims::QueuedCapture{.capturedBy = "fiona",
                                                      .operationKey = lims::OperationKey{"k"},
                                                      .capture = reading(lims::AnalysisVersionId{1}, 1000)}),
                    lims::ValidationError);
    // No dedup token: replay could not enforce at-most-once for it.
    CHECK_THROWS_AS(model.execute(lims::QueuedCapture{.sampleId = lims::SampleId{1},
                                                      .capturedBy = "fiona",
                                                      .capture = reading(lims::AnalysisVersionId{1}, 1000)}),
                    lims::ValidationError);
    // Both halves of the sum engaged: the capture itself is invalid.
    CHECK_THROWS_AS(
        model.execute(lims::QueuedCapture{
            .sampleId = lims::SampleId{1},
            .capturedBy = "fiona",
            .operationKey = lims::OperationKey{"k"},
            .capture = lims::CaptureConcentration{.analysisVersionId = lims::AnalysisVersionId{1},
                                                  .value = lims::Concentration{exact(1, 2, 3)},
                                                  .qualifier =
                                                      lims::QualifierChoice{std::string{lims::kQualifierBelowLod}}}}),
        lims::ValidationError);
}

TEST_CASE("Replay is journaled: applied and flagged updates both name their operator", "[lims][offline][audit]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    Lab lab;

    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    auto fionaQueue = std::make_shared<morph::offline::InMemoryOfflineQueue>();
    auto gerardQueue = std::make_shared<morph::offline::InMemoryOfflineQueue>();
    lims::offline::FieldOutbox fiona{fionaQueue, "fiona"};
    lims::offline::FieldOutbox gerard{gerardQueue, "gerard"};
    fiona.observe(lab.sample);
    gerard.observe(lab.sample);
    fiona.enqueue(lab.sample.id, reading(lab.versionId, 2400));
    gerard.enqueue(lab.sample.id, reading(lab.versionId, 9900));

    reconnect(fionaQueue, "fiona", log);
    reconnect(gerardQueue, "gerard", log);

    std::vector<std::string> principals;
    std::vector<std::string> results;
    for (const auto& entry : log->entries()) {
        if (entry.actionType == "QueuedCapture") {
            principals.push_back(entry.principal);
            results.push_back(entry.result);
        }
    }
    REQUIRE(principals.size() == 2);
    CHECK(principals[0] == "fiona");
    CHECK(principals[1] == "gerard");
    CHECK(results[0].find("\"Applied\"") != std::string::npos);
    CHECK(results[1].find("\"Conflicted\"") != std::string::npos);
    CHECK(results[1].find("\"StaleBase\"") != std::string::npos);
}

#ifdef MORPH_LADDER_HAVE_OFFLINE_SQLITE

// ── The durable leg ────────────────────────────────────────────────────────
//
// Everything above runs against InMemoryOfflineQueue. A field client does not:
// it uses a queue that survives the device being switched off, which is the
// whole reason the queue exists. This block runs the same shape against the
// real durable one, including across a simulated restart.

namespace {

/// @brief A fresh on-disk queue path for one test case.
/// @param name Distinguishes concurrent cases in the same directory.
/// @return The path, with any previous file removed.
[[nodiscard]] std::filesystem::path freshQueuePath(const std::string& name) {
    auto path = std::filesystem::temp_directory_path() / ("lims_field_queue_" + name + ".sqlite");
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + "-wal");
    std::filesystem::remove(path.string() + "-shm");
    return path;
}

}  // namespace

TEST_CASE("The definition-of-done flow holds on the durable queue, across a restart", "[lims][offline][dod][sqlite]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    Lab lab;

    const auto fionaPath = freshQueuePath("fiona");
    const auto gerardPath = freshQueuePath("gerard");

    {
        // Out in the field: two devices queue against the same version, then
        // both are switched off (the queue objects go out of scope).
        auto fionaQueue = std::make_shared<morph::offline::SqliteOfflineQueue>(fionaPath);
        auto gerardQueue = std::make_shared<morph::offline::SqliteOfflineQueue>(gerardPath);
        lims::offline::FieldOutbox fiona{fionaQueue, "fiona"};
        lims::offline::FieldOutbox gerard{gerardQueue, "gerard"};
        fiona.observe(lab.sample);
        gerard.observe(lab.sample);
        fiona.enqueue(lab.sample.id, reading(lab.versionId, 2400));
        gerard.enqueue(lab.sample.id, reading(lab.versionId, 9900));
        CHECK(fionaQueue->size() == 1);
        CHECK(gerardQueue->size() == 1);
    }

    // Back at the lab, fresh queue objects over the same files.
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

    lims::SampleModel reader;
    open(reader, lab.sample.id);
    const auto conflicts = reader.execute(lims::ListConflicts{}).conflicts;
    REQUIRE(conflicts.size() == 1);
    CHECK(conflicts[0].detectedBy == "gerard");
    CHECK(conflicts[0].reason == lims::ConflictReason::StaleBase);

    const auto results = reader.execute(lims::ListResults{});
    REQUIRE(results.results.size() == 1);
    CHECK(results.results[0].capturedBy == "fiona");
}

TEST_CASE("The durable queue dedups a re-enqueued operation where the in-memory one does not",
          "[lims][offline][sqlite][finding]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    Lab lab;

    // Same payload, same dedup token, enqueued twice — a field client that
    // retried a local write, say. `IOfflineQueue`'s documented contract is
    // that the queue "never interprets, requires, or enforces uniqueness" on
    // the key, but the two shipped implementations do not agree about that.
    // See morph#175; this test pins the divergence so a future change
    // to either one is noticed here rather than in a lab.
    const auto path = freshQueuePath("dedup");
    morph::offline::SqliteOfflineQueue durable{path};
    morph::offline::InMemoryOfflineQueue volatileQueue;
    CHECK(durable.enqueue("{}", "op-1") == durable.enqueue("{}", "op-1"));
    CHECK(durable.size() == 1);
    CHECK(volatileQueue.enqueue("{}", "op-1") != volatileQueue.enqueue("{}", "op-1"));
    CHECK(volatileQueue.size() == 2);

    // Which is exactly why replay enforces at-most-once itself. The same
    // logical update delivered twice through the queue that does *not* dedup
    // still lands once.
    auto queue = std::make_shared<morph::offline::InMemoryOfflineQueue>();
    lims::offline::FieldOutbox fiona{queue, "fiona"};
    fiona.observe(lab.sample);
    const auto queued = fiona.enqueue(lab.sample.id, reading(lab.versionId, 2400));
    queue->enqueue(morph::model::ActionTraits<lims::QueuedCapture>::toJson(queued), *queued.operationKey);
    REQUIRE(queue->size() == 2);

    reconnect(queue, "fiona");

    lims::SampleModel reader;
    open(reader, lab.sample.id);
    CHECK(reader.execute(lims::ListResults{}).results.size() == 1);
    CHECK(*reader.execute(lims::GetSample{}).version == *lab.sample.version + 1);
    CHECK(reader.execute(lims::ListConflicts{}).conflicts.empty());
}

#endif  // MORPH_LADDER_HAVE_OFFLINE_SQLITE
