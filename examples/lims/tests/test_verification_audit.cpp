// SPDX-License-Identifier: Apache-2.0
//
// Verification and audit (README build order §6).
//
// Two claims, tested separately because they fail separately:
//
//   1. Four eyes. The verifier must hold the role *and* must not be the
//      analyst who captured the reading. The first half is checkable before
//      dispatch and is enforced twice (authorizer at the edge, model on every
//      path); the second half `IAuthorizer` structurally cannot express, so it
//      lives only in the model.
//   2. "Every state a sample was ever in is reconstructible from the journal
//      alone." Asserted the only way that claim can honestly be asserted: by
//      deleting every row the sample has and reconstructing anyway.

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <algorithm>

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/journal/action_log.hpp>
#include <morph/session/session_auth.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <vector>

#include "lims/auth/lims_authorizer.hpp"
#include "lims/core/errors.hpp"
#include "lims/db/lims_entity.hpp"
#include "lims/models/analysis_catalog_model.hpp"
#include "lims/models/sample_model.hpp"
#include "lims_test_support.hpp"
#include "testkit/db_fixture.hpp"

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

/// @brief A sample with one captured reading, sitting at `ToBeVerified`.
struct ReadyToVerify {
    lims::AnalysisCatalogModel catalog;
    lims::SampleModel bench;
    lims::SampleView sample;
    lims::ResultView result;

    /// @param log Optional journal attached before anything is recorded.
    explicit ReadyToVerify(const std::shared_ptr<morph::journal::IActionLog>& log = nullptr) {
        if (log) {
            bench.attachActionLog(log, std::string{});
        }
        const auto nitrate =
            catalog.execute(lims::DefineAnalysis{.name = "Nitrate", .canonicalUnit = "mg_per_L", .decimalPlaces = 3});
        const auto client = bench.execute(lims::RegisterClient{.name = "Waterworks Ltd"});
        sample = bench.execute(lims::RegisterSample{.clientId = client.clientId, .reference = "WW-1"});
        bench.execute(lims::ReceiveSample{});
        bench.execute(lims::StartWork{});
        result = bench.execute(lims::CaptureConcentration{.analysisVersionId = nitrate.versionId,
                                                          .value = lims::Concentration{exact(12, 5, 3)}});
        sample = bench.execute(lims::SubmitForVerification{});
    }
};

/// @brief A signed token for @p principal, valid for an hour.
/// @param issuer The issuer to mint with.
/// @param principal The principal to name in the claims.
/// @return The token string.
[[nodiscard]] std::string tokenFor(const morph::session::TokenIssuer& issuer, const std::string& principal) {
    return issuer.issue(morph::session::SessionToken{
        .principal = principal, .issuedAtMs = 0, .expiresAtMs = 4'102'444'800'000, .roles = {}});
}

}  // namespace

// ── Roles ──────────────────────────────────────────────────────────────────

TEST_CASE("The first grant bootstraps a lab with no supervisor -- later ones need one", "[lims][audit][roles]") {
    DbFixture fixture;
    lims::SampleModel model;

    {
        // A lab with no supervisor has no way to appoint its first one, so the
        // first grant is allowed whoever makes it.
        const ScopedPrincipal alice{"alice"};
        const auto granted = model.execute(lims::GrantRole{.principal = "alice", .role = lims::LimsRole::Supervisor});
        CHECK(granted.principal == "alice");
        REQUIRE(granted.roles.size() == 1);
        CHECK(granted.roles[0] == lims::LimsRole::Supervisor);
    }
    {
        // And the carve-out closes the instant a supervisor exists.
        const ScopedPrincipal mallory{"mallory"};
        CHECK_THROWS_AS(model.execute(lims::GrantRole{.principal = "mallory", .role = lims::LimsRole::Supervisor}),
                        lims::Forbidden);
    }
    {
        const ScopedPrincipal alice{"alice"};
        const auto granted = model.execute(lims::GrantRole{.principal = "bob", .role = lims::LimsRole::Verifier});
        REQUIRE(granted.roles.size() == 1);
        CHECK(granted.roles[0] == lims::LimsRole::Verifier);

        // Idempotent: re-granting is not an error and does not duplicate.
        const auto again = model.execute(lims::GrantRole{.principal = "bob", .role = lims::LimsRole::Verifier});
        CHECK(again.roles.size() == 1);

        CHECK_THROWS_AS(model.execute(lims::GrantRole{.role = lims::LimsRole::Verifier}), lims::ValidationError);
    }
    // And with no session at all, before any other check runs.
    CHECK_THROWS_AS(model.execute(lims::GrantRole{.principal = "bob", .role = lims::LimsRole::Verifier}),
                    lims::EmptyPrincipalError);
}

TEST_CASE("Roles are not hierarchical", "[lims][audit][roles]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::SampleModel model;

    model.execute(lims::GrantRole{.principal = "alice", .role = lims::LimsRole::Supervisor});
    // A supervisor is not implicitly a verifier. An implicit hierarchy is
    // exactly how somebody ends up being the second pair of eyes on their own
    // work without anyone deciding they should be.
    const auto held = lims::SampleModel::rolesOf("alice");
    REQUIRE(held.size() == 1);
    CHECK(held[0] == lims::LimsRole::Supervisor);
}

// ── Four eyes ──────────────────────────────────────────────────────────────

TEST_CASE("Verifying needs the verifier role", "[lims][audit][verification]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    ReadyToVerify lab;

    {
        // Bob has no role at all yet.
        const ScopedPrincipal bob{"bob"};
        lims::SampleModel verifier;
        CHECK_THROWS_AS(verifier.execute(lims::VerifyResult{.resultId = lab.result.id}), lims::Forbidden);
    }

    lab.bench.execute(lims::GrantRole{.principal = "bob", .role = lims::LimsRole::Analyst});
    {
        // ...and an analyst is still not a verifier.
        const ScopedPrincipal bob{"bob"};
        lims::SampleModel verifier;
        CHECK_THROWS_AS(verifier.execute(lims::VerifyResult{.resultId = lab.result.id}), lims::Forbidden);
    }

    lab.bench.execute(lims::GrantRole{.principal = "bob", .role = lims::LimsRole::Verifier});
    {
        const ScopedPrincipal bob{"bob"};
        lims::SampleModel verifier;
        const auto recorded = verifier.execute(lims::VerifyResult{.resultId = lab.result.id});
        CHECK(recorded.verifiedBy == "bob");
        CHECK(recorded.capturedBy == "alice");
        CHECK(recorded.resultId == lab.result.id);
    }
}

TEST_CASE("A verifier cannot verify their own reading, however senior", "[lims][audit][verification]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    ReadyToVerify lab;

    // Alice captured the reading. Give her every role in the system — the
    // second pair of eyes still has to belong to a second person, and no role
    // grants an exemption.
    lab.bench.execute(lims::GrantRole{.principal = "alice", .role = lims::LimsRole::Supervisor});
    lab.bench.execute(lims::GrantRole{.principal = "alice", .role = lims::LimsRole::Verifier});

    CHECK_THROWS_AS(lab.bench.execute(lims::VerifyResult{.resultId = lab.result.id}), lims::Forbidden);
    CHECK(lab.bench.execute(lims::ListVerifications{}).verifications.empty());
}

TEST_CASE("A result is verified once, while the sample is awaiting verification", "[lims][audit][verification]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    ReadyToVerify lab;
    lab.bench.execute(lims::GrantRole{.principal = "bob", .role = lims::LimsRole::Verifier});

    const ScopedPrincipal bob{"bob"};
    lims::SampleModel verifier;
    verifier.execute(lims::OpenSample{.sampleId = lab.sample.id});
    verifier.execute(lims::VerifyResult{.resultId = lab.result.id});

    // Twice is a conflict, not a second signature.
    CHECK_THROWS_AS(verifier.execute(lims::VerifyResult{.resultId = lab.result.id}), lims::Conflict);
    CHECK(verifier.execute(lims::ListVerifications{}).verifications.size() == 1);

    CHECK_THROWS_AS(verifier.execute(lims::VerifyResult{.resultId = lims::ResultId{424242}}), lims::NotFound);
    CHECK_THROWS_AS(verifier.execute(lims::VerifyResult{}), lims::ValidationError);
}

TEST_CASE("A sample cannot be published while any of its results is unverified", "[lims][audit][verification]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    ReadyToVerify lab;

    // This is what makes the four-eyes step load-bearing rather than
    // decorative: publishing is where the lab tells the client a number is
    // true, so it is where a second pair of eyes is insisted on.
    CHECK_THROWS_AS(lab.bench.execute(lims::PublishSample{}), lims::IllegalTransition);
    CHECK(lab.bench.execute(lims::GetSample{}).state == lims::SampleState::ToBeVerified);

    lab.bench.execute(lims::GrantRole{.principal = "bob", .role = lims::LimsRole::Verifier});
    {
        const ScopedPrincipal bob{"bob"};
        lims::SampleModel verifier;
        verifier.execute(lims::VerifyResult{.resultId = lab.result.id});
    }
    CHECK(lab.bench.execute(lims::PublishSample{}).state == lims::SampleState::Published);
}

// ── The authorizer: the type-level half, at the edge ───────────────────────

TEST_CASE("The authorizer gates the role-requiring actions and nothing else", "[lims][audit][authorizer]") {
    const std::string secret = "shared-lab-secret";
    const morph::session::TokenIssuer issuer{secret, morph::session::hmacSha256};

    // The role source the authorizer consults. In production this is the same
    // `lims_operators` query the model uses; here it is a fixed table, so the
    // policy is tested without a schema.
    const lims::auth::RoleLookup roles = [](std::string_view principal) -> std::vector<lims::LimsRole> {
        if (principal == "bob") {
            return {lims::LimsRole::Verifier};
        }
        if (principal == "alice") {
            return {lims::LimsRole::Supervisor};
        }
        return {};
    };
    const lims::auth::LimsAuthorizer authorizer{secret, roles};

    morph::session::Context bob;
    bob.token = tokenFor(issuer, "bob");
    morph::session::Context alice;
    alice.token = tokenFor(issuer, "alice");
    morph::session::Context carol;
    carol.token = tokenFor(issuer, "carol");

    // Verifier-gated.
    CHECK(authorizer.authorize(bob, "SampleModel", "VerifyResult"));
    CHECK_FALSE(authorizer.authorize(alice, "SampleModel", "VerifyResult"));
    CHECK_FALSE(authorizer.authorize(carol, "SampleModel", "VerifyResult"));

    // Supervisor-gated.
    CHECK(authorizer.authorize(alice, "SampleModel", "GrantRole"));
    CHECK_FALSE(authorizer.authorize(bob, "SampleModel", "GrantRole"));

    // Everything else needs only a valid token — capturing a reading is an
    // everyday act, not a privileged one.
    CHECK(authorizer.authorize(carol, "SampleModel", "CaptureConcentration"));
    CHECK(authorizer.authorize(carol, "SampleModel", "GetSample"));

    // An unsigned or forged session is refused outright, before any role
    // question is asked.
    morph::session::Context anonymous;
    CHECK_FALSE(authorizer.authorize(anonymous, "SampleModel", "GetSample"));
    morph::session::Context forged;
    forged.token = tokenFor(issuer, "bob") + "x";
    CHECK_FALSE(authorizer.authorize(forged, "SampleModel", "VerifyResult"));

    // The principal the role check keys on is the *token's*, not the client's
    // claim: carol cannot become bob by saying so.
    morph::session::Context impostor;
    impostor.token = tokenFor(issuer, "carol");
    impostor.principal = "bob";
    CHECK_FALSE(authorizer.authorize(impostor, "SampleModel", "VerifyResult"));
}

TEST_CASE("An authorizer with no role source fails closed", "[lims][audit][authorizer]") {
    const std::string secret = "shared-lab-secret";
    const morph::session::TokenIssuer issuer{secret, morph::session::hmacSha256};
    const lims::auth::LimsAuthorizer authorizer{secret, nullptr};

    morph::session::Context bob;
    bob.token = tokenFor(issuer, "bob");
    CHECK_FALSE(authorizer.authorize(bob, "SampleModel", "VerifyResult"));
    CHECK_FALSE(authorizer.authorize(bob, "SampleModel", "GrantRole"));
    // Ungated actions are unaffected: failing closed means refusing the things
    // that need a role, not refusing everything.
    CHECK(authorizer.authorize(bob, "SampleModel", "GetSample"));
}

TEST_CASE("The model re-checks the role the authorizer already checked", "[lims][audit][authorizer]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    ReadyToVerify lab;

    // The authorizer would have allowed this — bob holds the role as far as a
    // token-carrying edge is concerned. The point is that no authorizer runs
    // on the Local dispatch path at all, so a model that trusted the edge
    // would be unguarded exactly where the ladder's own tests exercise it.
    CHECK(lims::SampleModel::rolesOf("bob").empty());
    const ScopedPrincipal bob{"bob"};
    lims::SampleModel verifier;
    CHECK_THROWS_AS(verifier.execute(lims::VerifyResult{.resultId = lab.result.id}), lims::Forbidden);
}

// ── The audit trail ────────────────────────────────────────────────────────

TEST_CASE("Every state the sample was ever in is reconstructible from the journal alone", "[lims][audit][dod]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    ReadyToVerify lab{log};

    lab.bench.execute(lims::GrantRole{.principal = "bob", .role = lims::LimsRole::Verifier});
    {
        const ScopedPrincipal bob{"bob"};
        lims::SampleModel verifier;
        verifier.execute(lims::VerifyResult{.resultId = lab.result.id});
    }
    lab.bench.execute(lims::PublishSample{});

    // Now delete every row this sample has. If the reconstruction consulted
    // the store at all, everything below breaks — which is the only honest way
    // to assert "from the journal alone".
    {
        Lightweight::DataMapper mapper;
        for (auto& row : mapper.Query<lims::db::VerificationRecord>().All()) {
            mapper.Delete(row);
        }
        for (auto& row : mapper.Query<lims::db::ResultRecord>().All()) {
            mapper.Delete(row);
        }
        for (auto& row : mapper.Query<lims::db::SampleRecord>().All()) {
            mapper.Delete(row);
        }
        CHECK(mapper.Query<lims::db::SampleRecord>().All().empty());
    }

    const auto trail = lab.bench.execute(lims::GetAuditTrail{});

    // The sample's whole lifecycle, in order.
    const std::vector<lims::SampleState> expected{
        lims::SampleState::Registered,   lims::SampleState::Received,  lims::SampleState::InProgress,
        lims::SampleState::ToBeVerified, lims::SampleState::Published,
    };
    CHECK(trail.states == expected);

    // And the non-lifecycle steps are there too, each naming its author.
    bool sawCapture = false;
    bool sawGrant = false;
    for (const auto& step : trail.steps) {
        CHECK(step.kind != lims::AuditStepKind::Unreadable);
        CHECK_FALSE(step.principal.empty());
        if (step.kind == lims::AuditStepKind::ResultCaptured) {
            sawCapture = true;
            CHECK(step.principal == "alice");
        }
        if (step.kind == lims::AuditStepKind::RoleGranted) {
            sawGrant = true;
        }
    }
    CHECK(sawCapture);
    CHECK(sawGrant);
}

TEST_CASE("A refused attempt appears in the trail, marked refused", "[lims][audit]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    ReadyToVerify lab{log};

    CHECK_THROWS_AS(lab.bench.execute(lims::PublishSample{}), lims::IllegalTransition);

    const auto trail = lab.bench.execute(lims::GetAuditTrail{});
    bool sawRefusal = false;
    for (const auto& step : trail.steps) {
        if (step.action == "PublishSample") {
            sawRefusal = true;
            CHECK(step.outcome == lims::AuditOutcome::Refused);
            CHECK(step.principal == "alice");
            CHECK(step.detail.find("has not been verified") != std::string::npos);
        }
    }
    CHECK(sawRefusal);

    // A refused transition contributes no state: the sample never was published.
    for (const auto& state : trail.states) {
        CHECK(state != lims::SampleState::Published);
    }
}

TEST_CASE("An entry this build cannot interpret is surfaced, never silently dropped", "[lims][audit][finding]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    ReadyToVerify lab{log};

    const auto entityKey = std::to_string(*lab.sample.id);

    // Two shapes a future (or past) build's journal can take, injected
    // directly: an action id this binary has never heard of, and a known
    // action whose recorded result does not decode.
    {
        morph::journal::LogEntry fromTheFuture;
        fromTheFuture.modelType = "SampleModel";
        fromTheFuture.entityKey = entityKey;
        fromTheFuture.actionType = "FreezeSample";
        fromTheFuture.payload = R"({"sampleId":1})";
        fromTheFuture.result = R"({"state":"Frozen"})";
        fromTheFuture.principal = "alice";
        log->append(fromTheFuture);

        morph::journal::LogEntry corrupt;
        corrupt.modelType = "SampleModel";
        corrupt.entityKey = entityKey;
        corrupt.actionType = "ReceiveSample";
        corrupt.payload = "{}";
        corrupt.result = "{ not json";
        corrupt.principal = "alice";
        log->append(corrupt);
    }

    const auto trail = lab.bench.execute(lims::GetAuditTrail{});

    std::vector<std::string> unreadable;
    for (const auto& step : trail.steps) {
        if (step.kind == lims::AuditStepKind::Unreadable) {
            unreadable.push_back(step.action);
            CHECK_FALSE(step.detail.empty());
        }
    }
    REQUIRE(unreadable.size() == 2);
    CHECK(unreadable[0] == "FreezeSample");
    CHECK(unreadable[1] == "ReceiveSample");

    // The gap is admitted rather than hidden: the step count still covers every
    // entry in the journal, so a reader can see that two of them could not be
    // interpreted instead of quietly getting a shorter, plausible history.
    CHECK(trail.steps.size() == log->entries(entityKey).size());
}

TEST_CASE("A renamed payload field decodes to a default, silently -- the payload-evolution gap",
          "[lims][audit][finding]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    ReadyToVerify lab{log};

    // A journal line written by an older build, in which the field now called
    // `reference` was called `ref`. `ActionTraits::fromJson` reads leniently
    // (`error_on_unknown_keys = false`), so the unknown key is dropped and the
    // missing one takes its default.
    const auto older = R"({"clientId":1,"ref":"WW-1"})";
    const auto decoded = morph::model::ActionTraits<lims::RegisterSample>::fromJson(older);

    // This is the finding, stated as an assertion: the payload said "WW-1" and
    // the decode says "". Nothing anywhere reports that a field was lost -- not
    // the codec, not the entry, not the reconstruction. Whatever the trail
    // renders from this entry will be confidently wrong rather than visibly
    // incomplete, which is what makes the definition of done's "reconstructible
    // from the journal alone" false across a rename.
    CHECK(decoded.reference.empty());
    CHECK(decoded.clientId == lims::ClientId{1});

    // The result half is what the trail actually reads, and it fails the same
    // way: a `SampleView` whose `state` key was renamed decodes to the default
    // state rather than being rejected.
    const auto olderResult = R"({"id":1,"clientId":1,"reference":"WW-1","sampleState":"Published","version":9})";
    const auto view = morph::model::ActionTraits<lims::ReceiveSample>::resultFromJson(olderResult);
    CHECK(view.state == lims::SampleState::Registered);  // NOT Published
    CHECK(*view.version == 9);

    // The `Unreadable` machinery above cannot catch this: the payload *does*
    // decode, it just decodes to something else. Only a per-entry payload
    // version could, and this rung does not have one -- see docs/findings/010.
}

TEST_CASE("A verification appears in the sample's own audit trail", "[lims][audit]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    ReadyToVerify lab{log};
    lab.bench.execute(lims::GrantRole{.principal = "bob", .role = lims::LimsRole::Verifier});

    {
        // Bob's handler is attached to nothing — he acts on a result id. The
        // verification must still be recorded against the sample, or the
        // sample's trail omits the second pair of eyes entirely.
        const ScopedPrincipal bob{"bob"};
        lims::SampleModel verifier;
        verifier.attachActionLog(log, std::string{});
        verifier.execute(lims::VerifyResult{.resultId = lab.result.id});
    }

    const auto trail = lab.bench.execute(lims::GetAuditTrail{});
    bool sawVerification = false;
    for (const auto& step : trail.steps) {
        if (step.kind == lims::AuditStepKind::ResultVerified) {
            sawVerification = true;
            CHECK(step.principal == "bob");
            CHECK(step.outcome == lims::AuditOutcome::Succeeded);
            // The trail names both halves of four eyes: who verified, and
            // whose reading they verified.
            CHECK(step.detail == "alice");
        }
    }
    CHECK(sawVerification);
}

TEST_CASE("A refused verification appears in the trail, marked refused", "[lims][audit]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    ReadyToVerify lab{log};
    lab.bench.execute(lims::GrantRole{.principal = "alice", .role = lims::LimsRole::Verifier});

    // Alice captured the reading, so she cannot verify it. The attempt is
    // audit-worthy precisely because it was refused.
    CHECK_THROWS_AS(lab.bench.execute(lims::VerifyResult{.resultId = lab.result.id}), lims::Forbidden);

    const auto trail = lab.bench.execute(lims::GetAuditTrail{});
    bool sawRefusal = false;
    for (const auto& step : trail.steps) {
        if (step.action == "VerifyResult") {
            sawRefusal = true;
            CHECK(step.kind == lims::AuditStepKind::ResultVerified);
            CHECK(step.outcome == lims::AuditOutcome::Refused);
            CHECK(step.detail.find("cannot also verify") != std::string::npos);
        }
    }
    CHECK(sawRefusal);
}

TEST_CASE("Offline replay and conflict resolution both reach the sample's trail", "[lims][audit]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();

    lims::AnalysisCatalogModel catalog;
    const auto nitrate = catalog.execute(
        lims::DefineAnalysis{.name = "Nitrate", .canonicalUnit = "mg_per_L", .decimalPlaces = 3});
    lims::SampleModel bench;
    bench.attachActionLog(log, std::string{});
    const auto client = bench.execute(lims::RegisterClient{.name = "Waterworks Ltd"});
    const auto sample = bench.execute(lims::RegisterSample{.clientId = client.clientId, .reference = "WW-1"});
    bench.execute(lims::ReceiveSample{});
    const auto atWork = bench.execute(lims::StartWork{});

    // A field update prepared against the version before the bench moved on,
    // so replay flags it.
    lims::QueuedCapture stale{
        .sampleId = sample.id,
        .baseVersion = lims::SampleVersion{*atWork.version - 1},
        .capturedBy = "alice",
        .operationKey = lims::OperationKey{"op-audit-1"},
        .capture = lims::CaptureConcentration{.analysisVersionId = nitrate.versionId,
                                              .value = lims::Concentration{exact(12, 5, 3)}},
    };
    const auto replayed = bench.execute(stale);
    REQUIRE(replayed.outcome == lims::ReplayOutcome::Conflicted);

    {
        // Resolved from a handler attached to *nothing* — a supervisor
        // working a conflicts queue acts on a conflict id, not on a sample.
        // The resolution must still land in that sample's trail, which is
        // what the rekey inside `execute(ResolveConflict)` is for. Resolving
        // through `bench` instead would prove nothing: it is already keyed to
        // this sample, so the rekey would be a no-op.
        lims::SampleModel supervisor;
        supervisor.attachActionLog(log, std::string{});
        supervisor.execute(lims::ResolveConflict{.conflictId = replayed.conflictId,
                                                 .resolution = lims::ConflictResolution::DiscardStale,
                                                 .note = "prepared against a superseded version"});
    }

    const auto trail = bench.execute(lims::GetAuditTrail{});
    bool sawReplay = false;
    bool sawResolution = false;
    for (const auto& step : trail.steps) {
        if (step.kind == lims::AuditStepKind::OfflineReplay) {
            sawReplay = true;
            // The trail says which way replay went, not merely that it ran.
            CHECK(step.detail == "conflicted");
        }
        if (step.kind == lims::AuditStepKind::ConflictResolved) {
            sawResolution = true;
            CHECK(step.principal == "alice");
        }
    }
    CHECK(sawReplay);
    CHECK(sawResolution);
}

TEST_CASE("A verification is refused unless the sample is awaiting verification", "[lims][audit]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    ReadyToVerify lab;
    lab.bench.execute(lims::GrantRole{.principal = "bob", .role = lims::LimsRole::Verifier});

    // Back to the bench: the reading is being reworked, so it is not up for
    // verification any more.
    lab.bench.execute(lims::ReturnForRework{.reason = "duplicate out of tolerance"});

    const ScopedPrincipal bob{"bob"};
    lims::SampleModel verifier;
    CHECK_THROWS_AS(verifier.execute(lims::VerifyResult{.resultId = lab.result.id}), lims::IllegalTransition);
}

TEST_CASE("The audit and verification listings need an attached handler", "[lims][audit]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::SampleModel model;

    CHECK_THROWS_AS(model.execute(lims::GetAuditTrail{}), lims::NotFound);
    CHECK_THROWS_AS(model.execute(lims::ListVerifications{}), lims::NotFound);
}

TEST_CASE("Every refused action kind is named in the trail, not lumped as unreadable",
          "[lims][audit]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();

    lims::AnalysisCatalogModel catalog;
    const auto nitrate = catalog.execute(
        lims::DefineAnalysis{.name = "Nitrate", .canonicalUnit = "mg_per_L", .decimalPlaces = 3});
    lims::SampleModel bench;
    bench.attachActionLog(log, std::string{});
    const auto client = bench.execute(lims::RegisterClient{.name = "Waterworks Ltd"});
    bench.execute(lims::RegisterSample{.clientId = client.clientId, .reference = "WW-1"});
    bench.execute(lims::ReceiveSample{});
    const auto atWork = bench.execute(lims::StartWork{});

    // One refusal of each remaining kind, so the trail's own classifier is
    // exercised on a *failed* entry rather than only a successful one. A
    // refused entry has no result to decode, so its kind comes from the
    // action id alone — which is exactly the path that would silently
    // degrade to `Unreadable` if an id were mistyped.
    CHECK_THROWS_AS(bench.execute(lims::CaptureConcentration{.analysisVersionId = nitrate.versionId}),
                    lims::ValidationError);
    CHECK_THROWS_AS(bench.execute(lims::QueuedCapture{.sampleId = atWork.id, .capturedBy = "alice"}),
                    lims::ValidationError);
    CHECK_THROWS_AS(bench.execute(lims::ResolveConflict{.conflictId = lims::ConflictId{424242},
                                                        .note = "no such conflict"}),
                    lims::NotFound);
    // A supervisor has to exist before a grant can be refused at all — the
    // bootstrap carve-out lets the *first* grant through whoever makes it.
    bench.execute(lims::GrantRole{.principal = "alice", .role = lims::LimsRole::Supervisor});
    {
        const ScopedPrincipal mallory{"mallory"};
        lims::SampleModel intruder;
        intruder.attachActionLog(log, std::to_string(*atWork.id));
        CHECK_THROWS_AS(intruder.execute(lims::GrantRole{.principal = "mallory",
                                                          .role = lims::LimsRole::Supervisor}),
                        lims::Forbidden);
    }

    std::vector<lims::AuditStepKind> refusedKinds;
    for (const auto& step : bench.execute(lims::GetAuditTrail{}).steps) {
        if (step.outcome == lims::AuditOutcome::Refused) {
            refusedKinds.push_back(step.kind);
        }
    }
    const auto has = [&refusedKinds](lims::AuditStepKind kind) {
        return std::find(refusedKinds.begin(), refusedKinds.end(), kind) != refusedKinds.end();
    };
    CHECK(has(lims::AuditStepKind::ResultCaptured));
    CHECK(has(lims::AuditStepKind::OfflineReplay));
    CHECK(has(lims::AuditStepKind::ConflictResolved));
    CHECK(has(lims::AuditStepKind::RoleGranted));
    // And none of them degraded to the catch-all.
    CHECK_FALSE(has(lims::AuditStepKind::Unreadable));
}

TEST_CASE("Listing conflicts needs an attached handler", "[lims][audit][offline]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::SampleModel model;
    CHECK_THROWS_AS(model.execute(lims::ListConflicts{}), lims::NotFound);
}
