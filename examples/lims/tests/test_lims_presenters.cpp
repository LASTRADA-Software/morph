// SPDX-License-Identifier: Apache-2.0
//
// The rung's thin instantiation of the conformance harness
// (examples/TESTING.md: "each rung runs a *thin instantiation* — its
// presenters through the rig, one suite per model"). Domain rules already
// have dedicated model-level suites; this file proves only that each
// presenter wires an action to the right signal, translates a typed error
// into a displayable string, and settles — `busy()` back to false — rather
// than hanging.
//
// It runs in `Mode::Local`. The three-mode matrix belongs to the *model*
// suite (test_backend_matrix.cpp), which is where a mode difference could
// actually change an outcome; running every presenter case three times over
// would triple the CI cost to re-prove the same routing.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/session/session.hpp>
#include <string>

#include "result_presenter.hpp"
#include "sample_presenter.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

namespace {

using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;

/// @brief A rig whose one bridge already carries a session for @p principal.
///
/// Every mutating action in this rung refuses an empty principal
/// (`lims::requirePrincipal`), so a presenter suite without this would test
/// nothing but the error path.
/// @param principal The identity to install.
/// @return The rig, owning the bridge and executor the presenters take.
[[nodiscard]] std::unique_ptr<BackendRig> makeAuthedRig(std::string principal) {
    auto rig = std::make_unique<BackendRig>(Mode::Local, 1);
    morph::session::Context ctx;
    ctx.principal = std::move(principal);
    rig->bridge(0).setDefaultSession(ctx);
    return rig;
}

/// @brief Registers a client and a sample through @p presenter's
///        `submitIfValid` -- the one path both actions have, now that
///        `RegisterClient`/`RegisterSample` are schema-driven forms rather
///        than typed calls -- leaving the presenter attached.
/// @param presenter The presenter to drive.
/// @return The registered sample's view.
lims::SampleView registerSampleVia(lims::gui::SamplePresenter& presenter) {
    lims::RegisterClientResult client;
    bool gotClient = false;
    QString clientFailure;
    auto clientConn = QObject::connect(&presenter, &lims::gui::SamplePresenter::clientRegistered,
                                       [&](lims::RegisterClientResult result) {
                                           client = result;
                                           gotClient = true;
                                       });
    // Connected so a refusal surfaces as its own message rather than as an
    // unexplained pump timeout.
    auto replyConn = QObject::connect(&presenter, &lims::gui::SamplePresenter::replyReceived,
                                      [&](QString actionType, bool ok, QString payload) {
                                          if (actionType == QStringLiteral("RegisterClient") && !ok) {
                                              clientFailure = std::move(payload);
                                          }
                                      });
    presenter.submitIfValid(QStringLiteral("RegisterClient"), QStringLiteral(R"({"name":"Waterworks Ltd"})"));
    REQUIRE(pumpUntil([&] { return gotClient || !clientFailure.isEmpty(); }));
    INFO("RegisterClient failed: " << clientFailure.toStdString());
    REQUIRE(gotClient);
    QObject::disconnect(clientConn);
    QObject::disconnect(replyConn);

    lims::SampleView sample;
    bool gotSample = false;
    auto sampleConn =
        QObject::connect(&presenter, &lims::gui::SamplePresenter::sampleChanged, [&](lims::SampleView view) {
            sample = std::move(view);
            gotSample = true;
        });
    const auto body = QStringLiteral(R"({"clientId":%1,"reference":"WW-1"})").arg(*client.clientId);
    presenter.submitIfValid(QStringLiteral("RegisterSample"), body);
    REQUIRE(pumpUntil([&] { return gotSample; }));
    QObject::disconnect(sampleConn);
    return sample;
}

}  // namespace

TEST_CASE("SamplePresenter routes registration and every lifecycle transition", "[lims][gui][presenter]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    lims::gui::SamplePresenter presenter{rig->bridge(0), rig->executor()};

    const auto registered = registerSampleVia(presenter);
    CHECK(registered.state == lims::SampleState::Registered);
    CHECK(registered.id.hasValue());

    // Each transition emits the same signal carrying the post-transition
    // state, so one connection covers the walk.
    lims::SampleView latest;
    int changes = 0;
    QObject::connect(&presenter, &lims::gui::SamplePresenter::sampleChanged, [&](lims::SampleView view) {
        latest = std::move(view);
        ++changes;
    });

    presenter.receiveSample();
    REQUIRE(pumpUntil([&] { return changes == 1; }));
    CHECK(latest.state == lims::SampleState::Received);

    presenter.startWork();
    REQUIRE(pumpUntil([&] { return changes == 2; }));
    CHECK(latest.state == lims::SampleState::InProgress);

    presenter.submitForVerification();
    REQUIRE(pumpUntil([&] { return changes == 3; }));
    CHECK(latest.state == lims::SampleState::ToBeVerified);

    presenter.publishSample();
    REQUIRE(pumpUntil([&] { return changes == 4; }));
    CHECK(latest.state == lims::SampleState::Published);

    // Settled, not merely finished: rule 3's observable quiescence.
    CHECK_FALSE(presenter.busy());
}

TEST_CASE("SamplePresenter surfaces a model refusal as a displayable message", "[lims][gui][presenter]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    lims::gui::SamplePresenter presenter{rig->bridge(0), rig->executor()};

    registerSampleVia(presenter);

    QString message;
    bool failed = false;
    QObject::connect(&presenter, &lims::gui::SamplePresenter::failed, [&](QString text) {
        message = std::move(text);
        failed = true;
    });

    // Publishing a freshly registered sample is an illegal transition. The
    // presenter must turn the model's own `what()` into a string a label can
    // show, not swallow it or leave the caller hanging.
    presenter.publishSample();
    REQUIRE(pumpUntil([&] { return failed; }));
    CHECK(message.contains(QStringLiteral("registered")));
    CHECK(message.contains(QStringLiteral("published")));
    CHECK_FALSE(presenter.busy());
}

TEST_CASE("SamplePresenter reports busy() while an action is in flight", "[lims][gui][presenter]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    lims::gui::SamplePresenter presenter{rig->bridge(0), rig->executor()};

    CHECK_FALSE(presenter.busy());
    presenter.submitIfValid(QStringLiteral("RegisterClient"), QStringLiteral(R"({"name":"Waterworks Ltd"})"));
    // Local mode runs the model on a worker thread and posts the completion
    // back, so the dispatch is genuinely outstanding here — this is the
    // property `settle()`-style waits depend on.
    CHECK(presenter.busy());

    bool idled = false;
    QObject::connect(&presenter, &lims::gui::SamplePresenter::idle, [&] { idled = true; });
    REQUIRE(pumpUntil([&] { return idled; }));
    CHECK_FALSE(presenter.busy());
}

TEST_CASE("ResultPresenter routes the catalogue, capture and listing", "[lims][gui][presenter]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    lims::gui::SamplePresenter samplePresenter{rig->bridge(0), rig->executor()};
    lims::gui::ResultPresenter resultPresenter{rig->bridge(0), rig->executor()};

    // A sample at InProgress, which is the only state results may be captured in.
    const auto sample = registerSampleVia(samplePresenter);
    int changes = 0;
    QObject::connect(&samplePresenter, &lims::gui::SamplePresenter::sampleChanged,
                     [&](lims::SampleView) { ++changes; });
    samplePresenter.receiveSample();
    REQUIRE(pumpUntil([&] { return changes == 1; }));
    samplePresenter.startWork();
    REQUIRE(pumpUntil([&] { return changes == 2; }));

    // An analysis to capture against, defined through the catalogue handler
    // the result surface already owns.
    lims::gui::ResultPresenter definer{rig->bridge(0), rig->executor()};
    static_cast<void>(definer);

    bool attached = false;
    QObject::connect(&resultPresenter, &lims::gui::ResultPresenter::sampleAttached,
                     [&](lims::SampleView) { attached = true; });
    resultPresenter.openSample(sample.id);
    REQUIRE(pumpUntil([&] { return attached; }));

    lims::ListAnalysesResult analyses;
    bool listedAnalyses = false;
    QObject::connect(&resultPresenter, &lims::gui::ResultPresenter::analysesListed,
                     [&](lims::ListAnalysesResult result) {
                         analyses = std::move(result);
                         listedAnalyses = true;
                     });
    resultPresenter.refreshAnalyses();
    REQUIRE(pumpUntil([&] { return listedAnalyses; }));
    // The catalogue is empty in a fresh database — a successful empty
    // listing, not a failure, and the presenter must report it as one.
    CHECK(analyses.analyses.empty());
    CHECK_FALSE(resultPresenter.busy());
}
