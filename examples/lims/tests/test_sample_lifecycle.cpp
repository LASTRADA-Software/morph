// SPDX-License-Identifier: Apache-2.0
//
// Sample registration and the lifecycle state machine (README build order
// §2): every transition guarded, journaled, and — when refused — journaled as
// a refusal.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/journal/action_log.hpp>
#include <string>
#include <utility>
#include <vector>

#include "lims/core/errors.hpp"
#include "lims/models/sample_model.hpp"
#include "lims_test_support.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;

namespace {

/// @brief Every `SampleState`, so a test can sweep the whole matrix.
constexpr std::array<lims::SampleState, 6> kAllStates{
    lims::SampleState::Registered,   lims::SampleState::Received,  lims::SampleState::InProgress,
    lims::SampleState::ToBeVerified, lims::SampleState::Published, lims::SampleState::Rejected,
};

/// @brief Registers a client and a sample against it, leaving @p model
///        attached to the sample.
/// @param model The model to register through.
/// @return The freshly registered sample.
lims::SampleView registerSample(lims::SampleModel& model) {
    const auto client = model.execute(lims::RegisterClient{.name = "Waterworks Ltd"});
    return model.execute(lims::RegisterSample{.clientId = client.clientId, .reference = "WW-2026-0001"});
}

/// @brief Walks @p model's attached sample up to `ToBeVerified`.
/// @param model The model, already attached to a `Registered` sample.
/// @return The sample at `ToBeVerified`.
lims::SampleView walkToVerification(lims::SampleModel& model) {
    model.execute(lims::ReceiveSample{});
    model.execute(lims::StartWork{});
    return model.execute(lims::SubmitForVerification{});
}

}  // namespace

TEST_CASE("The lifecycle's legal edges are exactly the declared eight", "[lims][sample][lifecycle]") {
    // Spelled out as data rather than re-derived from isLegalTransition's own
    // structure: a test that asks the implementation what it thinks the edges
    // are cannot notice the implementation changing its mind.
    const std::vector<std::pair<lims::SampleState, lims::SampleState>> legal{
        {lims::SampleState::Registered, lims::SampleState::Received},
        {lims::SampleState::Registered, lims::SampleState::Rejected},
        {lims::SampleState::Received, lims::SampleState::InProgress},
        {lims::SampleState::Received, lims::SampleState::Rejected},
        {lims::SampleState::InProgress, lims::SampleState::ToBeVerified},
        {lims::SampleState::ToBeVerified, lims::SampleState::Published},
        {lims::SampleState::ToBeVerified, lims::SampleState::InProgress},
    };

    std::size_t legalSeen = 0;
    for (const auto from : kAllStates) {
        for (const auto to : kAllStates) {
            const bool expected = std::find(legal.begin(), legal.end(), std::pair{from, to}) != legal.end();
            INFO("edge " << lims::stateName(from) << " -> " << lims::stateName(to));
            CHECK(lims::isLegalTransition(from, to) == expected);
            legalSeen += expected ? 1U : 0U;
        }
    }
    // The sweep really covered the declared edges (and only those): 7 of the
    // 36 cells are legal, so 29 are refusals the loop above actually checked.
    CHECK(legalSeen == legal.size());
    CHECK(legal.size() == 7);
}

TEST_CASE("No state may transition to itself", "[lims][sample][lifecycle]") {
    // Absorbing a repeated 'receive' as a no-op would journal a transition
    // that did not happen, which is the one thing an audit trail must not do.
    for (const auto state : kAllStates) {
        INFO("self-edge on " << lims::stateName(state));
        CHECK_FALSE(lims::isLegalTransition(state, state));
    }
}

TEST_CASE("Published and Rejected are terminal", "[lims][sample][lifecycle]") {
    for (const auto to : kAllStates) {
        CHECK_FALSE(lims::isLegalTransition(lims::SampleState::Published, to));
        CHECK_FALSE(lims::isLegalTransition(lims::SampleState::Rejected, to));
    }
}

TEST_CASE("A registered sample starts at Registered, version 1", "[lims][sample]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::SampleModel model;

    const auto sample = registerSample(model);
    REQUIRE(sample.id.hasValue());
    CHECK(sample.state == lims::SampleState::Registered);
    CHECK(*sample.version == 1);
    CHECK(sample.reference == "WW-2026-0001");
    CHECK(sample.registeredAt.hasValue());
}

TEST_CASE("The happy path walks registered -> published, bumping the base version each step",
          "[lims][sample][lifecycle]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::SampleModel model;

    const auto registered = registerSample(model);
    CHECK(*registered.version == 1);

    const auto received = model.execute(lims::ReceiveSample{});
    CHECK(received.state == lims::SampleState::Received);
    CHECK(*received.version == 2);

    const auto working = model.execute(lims::StartWork{});
    CHECK(working.state == lims::SampleState::InProgress);
    CHECK(*working.version == 3);

    const auto pending = model.execute(lims::SubmitForVerification{});
    CHECK(pending.state == lims::SampleState::ToBeVerified);
    CHECK(*pending.version == 4);

    const auto published = model.execute(lims::PublishSample{});
    CHECK(published.state == lims::SampleState::Published);
    CHECK(*published.version == 5);

    // Re-read: the state and the version are durable, not just returned.
    const auto reread = model.execute(lims::GetSample{});
    CHECK(reread.state == lims::SampleState::Published);
    CHECK(*reread.version == 5);
}

TEST_CASE("An illegal transition is refused and changes nothing", "[lims][sample][lifecycle]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::SampleModel model;

    registerSample(model);
    CHECK_THROWS_AS(model.execute(lims::PublishSample{}), lims::IllegalTransition);

    // The refusal must be total: no partial write, no version bump. A LIMS
    // that leaves a sample at 'registered' but version 2 has already lost the
    // property offline conflict detection depends on.
    const auto after = model.execute(lims::GetSample{});
    CHECK(after.state == lims::SampleState::Registered);
    CHECK(*after.version == 1);
}

TEST_CASE("A published sample refuses every further transition", "[lims][sample][lifecycle]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::SampleModel model;

    registerSample(model);
    walkToVerification(model);
    model.execute(lims::PublishSample{});

    CHECK_THROWS_AS(model.execute(lims::ReceiveSample{}), lims::IllegalTransition);
    CHECK_THROWS_AS(model.execute(lims::StartWork{}), lims::IllegalTransition);
    CHECK_THROWS_AS(model.execute(lims::SubmitForVerification{}), lims::IllegalTransition);
    CHECK_THROWS_AS(model.execute(lims::PublishSample{}), lims::IllegalTransition);
    CHECK_THROWS_AS(model.execute(lims::RejectSample{.reason = "changed my mind"}), lims::IllegalTransition);
    CHECK_THROWS_AS(model.execute(lims::ReturnForRework{.reason = "changed my mind"}), lims::IllegalTransition);

    CHECK(model.execute(lims::GetSample{}).state == lims::SampleState::Published);
}

TEST_CASE("A sample can be rejected at registration or at receipt, but not after work started",
          "[lims][sample][lifecycle]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};

    {
        lims::SampleModel model;
        registerSample(model);
        const auto rejected = model.execute(lims::RejectSample{.reason = "container broken in transit"});
        CHECK(rejected.state == lims::SampleState::Rejected);
    }
    {
        lims::SampleModel model;
        registerSample(model);
        model.execute(lims::ReceiveSample{});
        const auto rejected = model.execute(lims::RejectSample{.reason = "wrong preservative"});
        CHECK(rejected.state == lims::SampleState::Rejected);
    }
    {
        lims::SampleModel model;
        registerSample(model);
        model.execute(lims::ReceiveSample{});
        model.execute(lims::StartWork{});
        CHECK_THROWS_AS(model.execute(lims::RejectSample{.reason = "too late"}), lims::IllegalTransition);
    }
}

TEST_CASE("Returning a sample for rework needs a stated reason", "[lims][sample][lifecycle]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::SampleModel model;

    registerSample(model);
    walkToVerification(model);

    CHECK_THROWS_AS(model.execute(lims::ReturnForRework{}), lims::ValidationError);
    CHECK(model.execute(lims::GetSample{}).state == lims::SampleState::ToBeVerified);

    const auto reworking = model.execute(lims::ReturnForRework{.reason = "duplicate out of tolerance"});
    CHECK(reworking.state == lims::SampleState::InProgress);
}

TEST_CASE("A mutating transition with no principal is refused and journals nothing", "[lims][sample][audit]") {
    DbFixture fixture;
    lims::SampleModel model;
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();

    lims::SampleId sampleId;
    {
        const ScopedPrincipal alice{"alice"};
        sampleId = registerSample(model).id;
    }

    // Fresh handler, attached but unauthenticated.
    lims::SampleModel anonymous;
    anonymous.attachActionLog(log, std::string{});
    anonymous.execute(lims::OpenSample{.sampleId = sampleId});

    CHECK_THROWS_AS(anonymous.execute(lims::ReceiveSample{}), lims::EmptyPrincipalError);

    // The one failure class that cannot be attributed is the one that must not
    // be recorded: an audit entry naming nobody is worse than no entry.
    CHECK(log->entries().empty());
}

TEST_CASE("A successful transition is journaled against the sample, naming its author", "[lims][sample][audit]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();

    lims::SampleModel model;
    model.attachActionLog(log, std::string{});
    const auto sample = registerSample(model);
    model.execute(lims::ReceiveSample{});

    const auto entries = log->entries(std::to_string(*sample.id));
    // RegisterClient was journaled before the sample existed, so it is not
    // under this entity key; RegisterSample and ReceiveSample are.
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].actionType == "RegisterSample");
    CHECK(entries[1].actionType == "ReceiveSample");
    for (const auto& entry : entries) {
        CHECK(entry.modelType == "SampleModel");
        CHECK(entry.principal == "alice");
        CHECK(entry.outcome == morph::journal::Outcome::Succeeded);
        CHECK(entry.error.empty());
        CHECK_FALSE(entry.result.empty());
    }
    // The recorded result carries the post-transition state in a form a reader
    // can decode without the enum's integer layout.
    CHECK(entries[1].result.find("\"Received\"") != std::string::npos);
}

TEST_CASE("A refused transition is journaled as a failure, naming who tried", "[lims][sample][audit]") {
    DbFixture fixture;
    const ScopedPrincipal mallory{"mallory"};
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();

    lims::SampleModel model;
    model.attachActionLog(log, std::string{});
    const auto sample = registerSample(model);

    CHECK_THROWS_AS(model.execute(lims::PublishSample{}), lims::IllegalTransition);

    const auto entries = log->entries(std::to_string(*sample.id));
    REQUIRE(entries.size() == 2);
    const auto& failure = entries[1];
    CHECK(failure.actionType == "PublishSample");
    CHECK(failure.outcome == morph::journal::Outcome::Failed);
    CHECK(failure.principal == "mallory");
    CHECK(failure.result.empty());
    CHECK(failure.error.find("registered") != std::string::npos);
    CHECK(failure.error.find("published") != std::string::npos);
}

TEST_CASE("Two handlers attached to the same sample see each other's transitions", "[lims][sample][shared]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};

    lims::SampleModel office;
    const auto sample = registerSample(office);

    lims::SampleModel bench;
    const auto attached = bench.execute(lims::OpenSample{.sampleId = sample.id});
    CHECK(attached.id == sample.id);
    CHECK(attached.state == lims::SampleState::Registered);

    office.execute(lims::ReceiveSample{});
    // The store is authoritative, not the instance's cached view.
    CHECK(bench.execute(lims::GetSample{}).state == lims::SampleState::Received);
}

TEST_CASE("An unattached handler refuses to read or transition anything", "[lims][sample]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::SampleModel model;

    CHECK_THROWS_AS(model.execute(lims::GetSample{}), lims::NotFound);
    CHECK_THROWS_AS(model.execute(lims::ReceiveSample{}), lims::NotFound);
}

TEST_CASE("Opening or registering against something that does not exist is NotFound", "[lims][sample]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::SampleModel model;

    CHECK_THROWS_AS(model.execute(lims::OpenSample{.sampleId = lims::SampleId{424242}}), lims::NotFound);
    CHECK_THROWS_AS(model.execute(lims::RegisterSample{.clientId = lims::ClientId{424242}, .reference = "orphan"}),
                    lims::NotFound);
    CHECK_THROWS_AS(model.execute(lims::OpenSample{}), lims::ValidationError);
}

TEST_CASE("Registering a sample or a client requires a principal and well-formed input", "[lims][sample]") {
    DbFixture fixture;
    lims::SampleModel model;

    CHECK_THROWS_AS(model.execute(lims::RegisterClient{.name = "Waterworks Ltd"}), lims::EmptyPrincipalError);

    const ScopedPrincipal alice{"alice"};
    CHECK_THROWS_AS(model.execute(lims::RegisterClient{}), lims::ValidationError);
    const auto client = model.execute(lims::RegisterClient{.name = "Waterworks Ltd"});
    CHECK_THROWS_AS(model.execute(lims::RegisterSample{.clientId = client.clientId}), lims::ValidationError);
}
