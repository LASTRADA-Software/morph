// SPDX-License-Identifier: Apache-2.0
//
// The backend-mode matrix (examples/TESTING.md, "The dual-mode fixture"):
// one test body, run against `Mode::Local`, `Mode::LocalSingleThread` and
// `Mode::Socket`, so a model that works in-process is proven to work over a
// real socket against a real `RemoteServer` too.
//
// This is also the only place this rung's *keyed* wiring is exercised through
// a `Bridge` at all. The model suites construct `SampleModel` directly and
// call `execute()`, which never touches `ActionKeyTraits` — so the
// hand-written `ActionKeyTraits<OpenSample>` (payload-keyed) and
// `ActionKeyTraits<RegisterSample>` (result-keyed) specialisations were, until
// this file, compiled but never run.
//
// Socket mode needs a *verifying* authorizer: with a non-authenticating one,
// `RemoteServer` clears `Context::principal` before dispatch rather than
// passing the client's unverified claim through, and every mutating action in
// this rung then refuses with `EmptyPrincipalError`. `lims::auth::
// LimsAuthorizer` is that authorizer, so the matrix runs it end-to-end
// against a real server rather than only unit-testing its policy.

#include "lims/auth/lims_authorizer.hpp"
#include "lims/core/errors.hpp"
#include "lims/models/analysis_catalog_model.hpp"
#include "lims/models/sample_model.hpp"
#include "lims/offline/field_outbox.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <morph/core/backend.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/model.hpp>
#include <morph/offline/offline_queue.hpp>
#include <morph/session/session.hpp>
#include <morph/session/session_auth.hpp>
#include <morph/util/rational.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

using morph::bridge::AllowShared;
using morph::bridge::BridgeHandler;
using morph::ladder::testkit::awaitQt;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

namespace {

constexpr std::string_view kSecret = "lims-matrix-test-secret-at-least-32-bytes";

/// @brief An exact rational at @p places decimal places.
/// @param num Numerator.
/// @param den Denominator.
/// @param places Decimal-precision tag.
/// @return The canonical rational.
[[nodiscard]] Rational exact(std::int64_t num, std::int64_t den, std::uint32_t places) {
    return Rational{Numerator{num}, Denominator{den}, DecimalPlaces{places}};
}

/// @brief A signed session `Context` for @p principal.
/// @param issuer The issuer to mint with.
/// @param principal The identity to name in the claims.
/// @return The context, ready for `Bridge::setDefaultSession`.
[[nodiscard]] morph::session::Context tokenContextFor(const morph::session::TokenIssuer& issuer,
                                                       std::string principal) {
    morph::session::Context ctx;
    ctx.principal = principal;
    ctx.token = issuer.issue(morph::session::SessionToken{
        .principal = std::move(principal), .issuedAtMs = 0, .expiresAtMs = 4102444800000, .roles = {}});
    return ctx;
}

/// @brief This rung's real authorizer, wired to the same `lims_operators`
///        query the model reads.
/// @return The authorizer, for `BackendRig`'s `Socket` mode.
[[nodiscard]] std::shared_ptr<lims::auth::LimsAuthorizer> makeAuthorizer() {
    return std::make_shared<lims::auth::LimsAuthorizer>(
        std::string{kSecret},
        [](std::string_view principal) { return lims::SampleModel::rolesOf(principal); });
}

}  // namespace

TEST_CASE("SampleModel over the full backend-mode matrix: register, attach, transition, capture",
          "[lims][matrix]") {
    const auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;

    BackendRig rig{mode, 1, makeAuthorizer()};
    const morph::session::TokenIssuer issuer{std::string{kSecret}, morph::session::hmacSha256};
    rig.bridge(0).setDefaultSession(tokenContextFor(issuer, "alice"));

    // A catalogue entry, through a plain handler on the unkeyed model.
    auto catalog = rig.client<lims::AnalysisCatalogModel>(0);
    const auto nitrate = awaitQt(catalog.execute(
        lims::DefineAnalysis{.name = "Nitrate", .canonicalUnit = "mg_per_L", .decimalPlaces = 3}));
    REQUIRE(nitrate.versionId.hasValue());

    // `RegisterClient` carries no key at all, so it needs a plain handler:
    // an `AllowShared` one is not bound until something attaches it.
    auto creator = rig.client<lims::SampleModel>(0);
    const auto client = awaitQt(creator.execute(lims::RegisterClient{.name = "Waterworks Ltd"}));
    REQUIRE(client.clientId.hasValue());

    // `RegisterSample` is *result*-keyed: the framework runs it on an
    // anonymous instance and promotes that instance to the id the result
    // names, so this shared handler is attached to the new sample by the time
    // the completion resolves. Nothing in the model suites exercises this —
    // they construct the model directly.
    BridgeHandler<lims::SampleModel, AllowShared> handler{rig.bridge(0), rig.executor()};
    const auto registered =
        awaitQt(handler.execute(lims::RegisterSample{.clientId = client.clientId, .reference = "WW-1"}));
    REQUIRE(registered.id.hasValue());
    CHECK(registered.state == lims::SampleState::Registered);

    // Key-less actions on the same handler must now land on that instance.
    // Before the result-keyed promotion above they would have had nothing to
    // run against.
    CHECK(awaitQt(handler.execute(lims::ReceiveSample{})).state == lims::SampleState::Received);
    CHECK(awaitQt(handler.execute(lims::StartWork{})).state == lims::SampleState::InProgress);

    const auto captured = awaitQt(handler.execute(lims::CaptureConcentration{
        .analysisVersionId = nitrate.versionId, .value = lims::Concentration{exact(12, 5, 3)}}));
    REQUIRE(captured.value.hasValue());
    CHECK((*captured.value).numerator == 12);
    // The reading is attributed to the *authenticated* principal. In Socket
    // mode that is the token's, established by LimsAuthorizer::authenticate —
    // not the client's claim.
    CHECK(captured.capturedBy == "alice");

    const auto listed = awaitQt(handler.execute(lims::ListResults{}));
    REQUIRE(listed.results.size() == 1);
}

TEST_CASE("A payload-keyed OpenSample attaches a fresh handler in every mode", "[lims][matrix]") {
    const auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;

    BackendRig rig{mode, 1, makeAuthorizer()};
    const morph::session::TokenIssuer issuer{std::string{kSecret}, morph::session::hmacSha256};
    rig.bridge(0).setDefaultSession(tokenContextFor(issuer, "alice"));

    auto creator = rig.client<lims::SampleModel>(0);
    const auto client = awaitQt(creator.execute(lims::RegisterClient{.name = "Waterworks Ltd"}));
    const auto registered =
        awaitQt(creator.execute(lims::RegisterSample{.clientId = client.clientId, .reference = "WW-2"}));

    // A *different*, freshly built handler attaches by id — the payload-keyed
    // path, through the hand-written `ActionKeyTraits<OpenSample>`.
    BridgeHandler<lims::SampleModel, AllowShared> office{rig.bridge(0), rig.executor()};
    const auto opened = awaitQt(office.execute(lims::OpenSample{.sampleId = registered.id}));
    CHECK(opened.id == registered.id);
    CHECK(opened.reference == "WW-2");

    // And key-less actions on it now act on that sample.
    CHECK(awaitQt(office.execute(lims::ReceiveSample{})).state == lims::SampleState::Received);
    CHECK(awaitQt(office.execute(lims::GetSample{})).state == lims::SampleState::Received);
}

TEST_CASE("Two connections attached to one sample share the instance", "[lims][matrix][socket-only]") {
    // Socket mode only: it is the only mode with a server-side shared-instance
    // directory, which is the thing under test here rather than mere
    // cross-handler sharing inside one process.
    DbFixture fixture;
    BackendRig rig{Mode::Socket, 2, makeAuthorizer()};
    const morph::session::TokenIssuer issuer{std::string{kSecret}, morph::session::hmacSha256};
    rig.bridge(0).setDefaultSession(tokenContextFor(issuer, "alice"));
    rig.bridge(1).setDefaultSession(tokenContextFor(issuer, "bob"));

    auto creator = rig.client<lims::SampleModel>(0);
    const auto client = awaitQt(creator.execute(lims::RegisterClient{.name = "Waterworks Ltd"}));
    const auto registered =
        awaitQt(creator.execute(lims::RegisterSample{.clientId = client.clientId, .reference = "WW-3"}));

    BridgeHandler<lims::SampleModel, AllowShared> bench{rig.bridge(0), rig.executor()};
    BridgeHandler<lims::SampleModel, AllowShared> office{rig.bridge(1), rig.executor()};
    awaitQt(bench.execute(lims::OpenSample{.sampleId = registered.id}));
    awaitQt(office.execute(lims::OpenSample{.sampleId = registered.id}));

    // The bench moves the sample; the office sees it without re-attaching.
    awaitQt(bench.execute(lims::ReceiveSample{}));
    const auto seenByOffice = awaitQt(office.execute(lims::GetSample{}));
    CHECK(seenByOffice.state == lims::SampleState::Received);
    CHECK(*seenByOffice.version == *registered.version + 1);
}

TEST_CASE("An offline capture replays through the bridge under its operator's own session",
          "[lims][matrix][offline]") {
    // §7's supported replay path, driven the way a reconnecting client
    // actually drives it: the client drains its own queue and re-dispatches
    // each item as an ordinary action through its authenticated `Bridge`.
    // That is what makes `QueuedCapture`'s `capturedBy` check mean anything —
    // see this rung's README §7 decision and docs/findings/014 for why the
    // framework's own `onBackendChanged()` drain cannot carry a session.
    const auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;

    BackendRig rig{mode, 1, makeAuthorizer()};
    const morph::session::TokenIssuer issuer{std::string{kSecret}, morph::session::hmacSha256};
    rig.bridge(0).setDefaultSession(tokenContextFor(issuer, "fiona"));

    auto catalog = rig.client<lims::AnalysisCatalogModel>(0);
    const auto nitrate = awaitQt(catalog.execute(
        lims::DefineAnalysis{.name = "Nitrate", .canonicalUnit = "mg_per_L", .decimalPlaces = 3}));

    auto creator = rig.client<lims::SampleModel>(0);
    const auto client = awaitQt(creator.execute(lims::RegisterClient{.name = "Waterworks Ltd"}));
    BridgeHandler<lims::SampleModel, AllowShared> handler{rig.bridge(0), rig.executor()};
    const auto registered =
        awaitQt(handler.execute(lims::RegisterSample{.clientId = client.clientId, .reference = "WW-4"}));
    awaitQt(handler.execute(lims::ReceiveSample{}));
    const auto atWork = awaitQt(handler.execute(lims::StartWork{}));

    // Out in the field: two edits queued against the version the client last
    // saw, chained so the second builds on the first.
    auto queue = std::make_shared<morph::offline::InMemoryOfflineQueue>();
    lims::offline::FieldOutbox outbox{queue, "fiona"};
    outbox.observe(atWork);
    outbox.enqueue(atWork.id, lims::CaptureConcentration{.analysisVersionId = nitrate.versionId,
                                                          .value = lims::Concentration{exact(12, 5, 3)}});
    outbox.enqueue(atWork.id, lims::CaptureConcentration{.analysisVersionId = nitrate.versionId,
                                                          .value = lims::Concentration{exact(13, 5, 3)}});

    // Reconnect: drain and re-dispatch. Every item goes through the same
    // authenticated bridge every other action uses.
    for (const auto& item : queue->drain()) {
        const auto queued = morph::model::ActionTraits<lims::QueuedCapture>::fromJson(item.payload);
        const auto outcome = awaitQt(handler.execute(queued));
        CHECK(outcome.outcome == lims::ReplayOutcome::Applied);
        queue->markDone(item.id);
    }
    CHECK(queue->size() == 0);

    const auto listed = awaitQt(handler.execute(lims::ListResults{}));
    REQUIRE(listed.results.size() == 1);
    // The second edit won, and it is attributed to the field operator.
    CHECK((*listed.results[0].value).numerator == 13);
    CHECK(listed.results[0].capturedBy == "fiona");
    CHECK(awaitQt(handler.execute(lims::ListConflicts{})).conflicts.empty());
}

TEST_CASE("A queued capture replayed as the wrong operator is refused over the wire too",
          "[lims][matrix][offline]") {
    DbFixture fixture;
    BackendRig rig{Mode::Socket, 1, makeAuthorizer()};
    const morph::session::TokenIssuer issuer{std::string{kSecret}, morph::session::hmacSha256};
    // The bridge is authenticated as *mallory*, and the queued item claims
    // *fiona*. In Socket mode the principal the model sees is the token's, so
    // this is the check working against a real server rather than against a
    // thread-local a test set.
    rig.bridge(0).setDefaultSession(tokenContextFor(issuer, "mallory"));

    auto catalog = rig.client<lims::AnalysisCatalogModel>(0);
    const auto nitrate = awaitQt(catalog.execute(
        lims::DefineAnalysis{.name = "Nitrate", .canonicalUnit = "mg_per_L", .decimalPlaces = 3}));
    auto creator = rig.client<lims::SampleModel>(0);
    const auto client = awaitQt(creator.execute(lims::RegisterClient{.name = "Waterworks Ltd"}));
    BridgeHandler<lims::SampleModel, AllowShared> handler{rig.bridge(0), rig.executor()};
    const auto registered =
        awaitQt(handler.execute(lims::RegisterSample{.clientId = client.clientId, .reference = "WW-5"}));
    awaitQt(handler.execute(lims::ReceiveSample{}));
    const auto atWork = awaitQt(handler.execute(lims::StartWork{}));

    auto queue = std::make_shared<morph::offline::InMemoryOfflineQueue>();
    lims::offline::FieldOutbox outbox{queue, "fiona"};
    outbox.observe(atWork);
    const auto queued = outbox.enqueue(
        atWork.id, lims::CaptureConcentration{.analysisVersionId = nitrate.versionId,
                                              .value = lims::Concentration{exact(12, 5, 3)}});

    CHECK_THROWS(awaitQt(handler.execute(queued)));
    CHECK(awaitQt(handler.execute(lims::ListResults{})).results.empty());
}

// ── The framework's own onBackendChanged() drain ───────────────────────────

TEST_CASE("onBackendChanged fires on switchBackend, and fails closed with no session",
          "[lims][matrix][offline][finding]") {
    // `docs/spec/offline/offline.md` names `Model::onBackendChanged()` as the
    // replay seam for outcomes richer than success/failure — conflicts,
    // merges, discards. `SampleModel` implements it, and the §7 suite drives
    // it by calling it directly from inside a `ScopedPrincipal`.
    //
    // This case drives it the way the *framework* does: `switchBackend()`
    // posts it onto the new model's own strand. There is no session there —
    // `morph::session::current()` is a thread-local established by the
    // dispatcher during an `execute`, and a posted `onBackendChanged` is not
    // an execute. So every queued item is refused for want of a principal.
    //
    // That is the behaviour this case pins, and it is the *right* behaviour
    // for this rung: a lab reading replayed with no identified author is
    // exactly what the README calls disqualifying. But it does mean the
    // framework's own replay seam cannot carry an authenticated replay, which
    // is why §7's supported path is the re-dispatch above. See
    // docs/findings/014.
    DbFixture fixture;

    auto queue = std::make_shared<morph::offline::InMemoryOfflineQueue>();
    lims::SampleId sampleId;
    lims::AnalysisVersionId versionId;
    {
        const morph::session::Context ctx{.principal = "fiona"};
        const morph::session::detail::ScopedContext scope{ctx};
        lims::AnalysisCatalogModel catalog;
        versionId = catalog
                        .execute(lims::DefineAnalysis{
                            .name = "Nitrate", .canonicalUnit = "mg_per_L", .decimalPlaces = 3})
                        .versionId;
        lims::SampleModel setup;
        const auto client = setup.execute(lims::RegisterClient{.name = "Waterworks Ltd"});
        const auto sample = setup.execute(lims::RegisterSample{.clientId = client.clientId, .reference = "WW-6"});
        setup.execute(lims::ReceiveSample{});
        const auto atWork = setup.execute(lims::StartWork{});
        sampleId = atWork.id;

        lims::offline::FieldOutbox outbox{queue, "fiona"};
        outbox.observe(atWork);
        outbox.enqueue(atWork.id, lims::CaptureConcentration{.analysisVersionId = versionId,
                                                             .value = lims::Concentration{exact(12, 5, 3)}});
    }
    REQUIRE(queue->size() == 1);

    // A binding whose factory hands the model the queue — the public
    // `Bridge::registerHandler(binding)` seam, and the only way an
    // application can inject a dependency into a registry-constructed model.
    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = std::string{morph::model::ModelTraits<lims::SampleModel>::typeId()};
    binding->modelFactory = [queue]() -> std::unique_ptr<morph::model::detail::IModelHolder> {
        auto holder = std::make_unique<morph::model::detail::ModelHolder<lims::SampleModel>>();
        holder->model.attachOfflineQueue(queue);
        return holder;
    };

    morph::exec::ThreadPoolExecutor before{1};
    morph::exec::ThreadPoolExecutor after{1};
    morph::ladder::testkit::detail::QtDrivenMainThreadExecutor callbackExecutor;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(before)};
    morph::bridge::BridgeHandler<lims::SampleModel> handler{bridge, &callbackExecutor, binding};

    bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(after));

    // The drain is posted, so wait for it rather than assuming it finished.
    REQUIRE(morph::ladder::testkit::pumpUntil([&] { return queue->size() == 0; }));

    // Every item was `markDone`d — the queue does not block — but nothing was
    // applied, because nothing could say who was applying it.
    lims::SampleModel reader;
    {
        const morph::session::Context ctx{.principal = "fiona"};
        const morph::session::detail::ScopedContext scope{ctx};
        reader.execute(lims::OpenSample{.sampleId = sampleId});
        CHECK(reader.execute(lims::ListResults{}).results.empty());
        // Nor was it turned into a conflict a human is asked to resolve: a
        // refusal for want of an author is not a data conflict.
        CHECK(reader.execute(lims::ListConflicts{}).conflicts.empty());
    }
}
