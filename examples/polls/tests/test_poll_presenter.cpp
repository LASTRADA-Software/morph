// SPDX-License-Identifier: Apache-2.0
//
// PollPresenter's own suite (Task 14, mirroring rung 2's Task 17
// test_bookmark_presenter.cpp): each of its nine actions
// (createPoll/openPoll/getPollState/submitVotes/updateVotes/addComment/
// finalizePoll/undoLastVoteChange/getEventsSince) round-trips through the
// presenter's own signals -- not the model directly -- across the full
// BackendRig mode matrix (Local/LocalSingleThread/Socket,
// examples/TESTING.md "The dual-mode fixture"), plus a
// validation-failure-routing case and two "emits failed, not a crash"
// cases. Domain rules (vote tallying, undo's principal-scoping, the
// event log's ordering/cursor semantics, admin-token gating, ...) already
// have a dedicated suite at the model level (test_poll_model.cpp); this
// file only proves the presenter wires each action to the right signal,
// sets busy()/idle() correctly, and neither crashes nor hangs -- the
// "translates and routes only" contract poll_presenter.hpp's own doc
// comment states (examples/IMPLEMENTATION.md rule 2).
//
// Unlike bookmarks/pastebin, this rung needs no signed token at all for
// most actions -- PollsAuthorizer permits every register/instance hook
// unconditionally (polls_authorizer.hpp's own @file comment), and
// PollModel calls no requirePrincipal() anywhere. The one real per-call
// check this rung has is FinalizePoll's requireAdmin(), comparing
// session::current()->token against the poll's own stored admin token --
// exercised below by setting a bare (unsigned) Context::token to the
// admin token CreatePoll returned, exactly test_poll_model.cpp's/
// test_shared_instance_lifecycle.cpp's own pattern.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <memory>
#include <morph/session/session.hpp>
#include <polls/auth/polls_authorizer.hpp>
#include <string>
#include <vector>

#include "poll_presenter.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

namespace {

using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;

/// @brief Builds a rig with a fresh `PollsAuthorizer`, for @p mode. Every
///        polls test file that touches `Mode::Socket` passes an explicit
///        authorizer (test_shared_instance_lifecycle.cpp's own
///        `makeRig`-shaped call sites) -- this mirrors that, even though
///        `PollsAuthorizer` behaves identically to the default for every
///        action this suite exercises (see this file's own top comment).
[[nodiscard]] std::unique_ptr<BackendRig> makeRig(Mode mode, std::size_t nClients = 1) {
    return std::make_unique<BackendRig>(mode, nClients, std::make_shared<polls::auth::PollsAuthorizer>());
}

}  // namespace

TEST_CASE("PollPresenter::createPoll then openPoll round-trips a poll, all three backend modes",
          "[polls][presenter]") {
    const auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto rig = makeRig(mode);
    polls::gui::PollPresenter presenter{rig->bridge(0), rig->executor()};

    polls::CreatePollResult createdResult;
    bool created = false;
    QObject::connect(&presenter, &polls::gui::PollPresenter::created, [&](polls::CreatePollResult result) {
        createdResult = std::move(result);
        created = true;
    });
    presenter.createPoll(polls::CreatePoll{.title = "Team offsite", .options = {{"2026-09-01"}, {"2026-09-02"}}});
    REQUIRE(pumpUntil([&] { return created; }));
    REQUIRE_FALSE(presenter.busy());
    CHECK_FALSE(createdResult.pollId.empty());
    REQUIRE(createdResult.adminToken.hasValue());
    REQUIRE(createdResult.participantToken.hasValue());
    CHECK_FALSE((*createdResult.adminToken).empty());
    CHECK_FALSE((*createdResult.participantToken).empty());

    polls::GetPollStateResult opened;
    bool gotOpened = false;
    QObject::connect(&presenter, &polls::gui::PollPresenter::opened, [&](polls::GetPollStateResult result) {
        opened = std::move(result);
        gotOpened = true;
    });
    presenter.openPoll(createdResult.pollId);
    REQUIRE(pumpUntil([&] { return gotOpened; }));
    REQUIRE_FALSE(presenter.busy());
    CHECK(opened.pollId == createdResult.pollId);
    CHECK(opened.title == "Team offsite");
    REQUIRE(opened.options.size() == 2);
    CHECK(opened.options[0].label == "2026-09-01");
    CHECK(opened.options[1].label == "2026-09-02");
    CHECK(opened.finalized == polls::Finalized::No);
}

TEST_CASE("PollPresenter::getPollState after openPoll returns the same poll's state, all three backend modes",
          "[polls][presenter]") {
    const auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto rig = makeRig(mode);
    polls::gui::PollPresenter presenter{rig->bridge(0), rig->executor()};

    polls::CreatePollResult createdResult;
    bool created = false;
    QObject::connect(&presenter, &polls::gui::PollPresenter::created, [&](polls::CreatePollResult result) {
        createdResult = std::move(result);
        created = true;
    });
    presenter.createPoll(polls::CreatePoll{.title = "Lunch spot", .options = {{"Cafe"}, {"Diner"}}});
    REQUIRE(pumpUntil([&] { return created; }));

    bool gotOpened = false;
    QObject::connect(&presenter, &polls::gui::PollPresenter::opened,
                     [&](polls::GetPollStateResult) { gotOpened = true; });
    presenter.openPoll(createdResult.pollId);
    REQUIRE(pumpUntil([&] { return gotOpened; }));

    polls::GetPollStateResult state;
    bool gotState = false;
    QObject::connect(&presenter, &polls::gui::PollPresenter::stateLoaded, [&](polls::GetPollStateResult result) {
        state = std::move(result);
        gotState = true;
    });
    presenter.getPollState(polls::GetPollState{});
    REQUIRE(pumpUntil([&] { return gotState; }));
    REQUIRE_FALSE(presenter.busy());
    CHECK(state.pollId == createdResult.pollId);
    CHECK(state.title == "Lunch spot");
    REQUIRE(state.options.size() == 2);
}

TEST_CASE("PollPresenter::submitVotes tallies a participant's vote, all three backend modes", "[polls][presenter]") {
    const auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto rig = makeRig(mode);
    polls::gui::PollPresenter presenter{rig->bridge(0), rig->executor()};

    polls::CreatePollResult createdResult;
    bool created = false;
    QObject::connect(&presenter, &polls::gui::PollPresenter::created, [&](polls::CreatePollResult result) {
        createdResult = std::move(result);
        created = true;
    });
    presenter.createPoll(polls::CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    REQUIRE(pumpUntil([&] { return created; }));

    polls::GetPollStateResult opened;
    bool gotOpened = false;
    QObject::connect(&presenter, &polls::gui::PollPresenter::opened, [&](polls::GetPollStateResult result) {
        opened = std::move(result);
        gotOpened = true;
    });
    presenter.openPoll(createdResult.pollId);
    REQUIRE(pumpUntil([&] { return gotOpened; }));
    REQUIRE(opened.options.size() == 2);

    polls::GetPollStateResult afterVote;
    bool gotVote = false;
    QObject::connect(&presenter, &polls::gui::PollPresenter::votesSubmitted, [&](polls::GetPollStateResult result) {
        afterVote = std::move(result);
        gotVote = true;
    });
    presenter.submitVotes(polls::SubmitVotes{
        .participantName = "alice", .votes = {{.optionId = opened.options[0].id, .choice = polls::VoteChoice::Yes}}});
    REQUIRE(pumpUntil([&] { return gotVote; }));
    REQUIRE_FALSE(presenter.busy());
    REQUIRE(afterVote.votes.size() == 1);
    CHECK(afterVote.votes.front().participantName == "alice");
    CHECK(afterVote.options[0].yesCount == polls::Count::fromDouble(1.0));
}

TEST_CASE("PollPresenter::updateVotes replaces a participant's votes wholesale, all three backend modes",
          "[polls][presenter]") {
    const auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto rig = makeRig(mode);
    polls::gui::PollPresenter presenter{rig->bridge(0), rig->executor()};

    polls::CreatePollResult createdResult;
    bool created = false;
    QObject::connect(&presenter, &polls::gui::PollPresenter::created, [&](polls::CreatePollResult result) {
        createdResult = std::move(result);
        created = true;
    });
    presenter.createPoll(polls::CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    REQUIRE(pumpUntil([&] { return created; }));

    polls::GetPollStateResult opened;
    bool gotOpened = false;
    QObject::connect(&presenter, &polls::gui::PollPresenter::opened, [&](polls::GetPollStateResult result) {
        opened = std::move(result);
        gotOpened = true;
    });
    presenter.openPoll(createdResult.pollId);
    REQUIRE(pumpUntil([&] { return gotOpened; }));

    bool submitted = false;
    QObject::connect(&presenter, &polls::gui::PollPresenter::votesSubmitted,
                     [&](polls::GetPollStateResult) { submitted = true; });
    presenter.submitVotes(polls::SubmitVotes{
        .participantName = "alice", .votes = {{.optionId = opened.options[0].id, .choice = polls::VoteChoice::Yes}}});
    REQUIRE(pumpUntil([&] { return submitted; }));

    polls::GetPollStateResult afterUpdate;
    bool updated = false;
    QObject::connect(&presenter, &polls::gui::PollPresenter::votesUpdated, [&](polls::GetPollStateResult result) {
        afterUpdate = std::move(result);
        updated = true;
    });
    presenter.updateVotes(polls::UpdateVotes{
        .participantName = "alice", .votes = {{.optionId = opened.options[1].id, .choice = polls::VoteChoice::Yes}}});
    REQUIRE(pumpUntil([&] { return updated; }));
    REQUIRE_FALSE(presenter.busy());
    REQUIRE(afterUpdate.votes.size() == 1);
    CHECK(afterUpdate.options[0].yesCount == polls::Count::fromDouble(0.0));  // alice's old vote is gone
    CHECK(afterUpdate.options[1].yesCount == polls::Count::fromDouble(1.0));
}

TEST_CASE("PollPresenter::addComment writes a comment visible in the next getPollState, all three backend modes",
          "[polls][presenter]") {
    const auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto rig = makeRig(mode);
    polls::gui::PollPresenter presenter{rig->bridge(0), rig->executor()};

    polls::CreatePollResult createdResult;
    bool created = false;
    QObject::connect(&presenter, &polls::gui::PollPresenter::created, [&](polls::CreatePollResult result) {
        createdResult = std::move(result);
        created = true;
    });
    presenter.createPoll(polls::CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    REQUIRE(pumpUntil([&] { return created; }));

    bool gotOpened = false;
    QObject::connect(&presenter, &polls::gui::PollPresenter::opened,
                     [&](polls::GetPollStateResult) { gotOpened = true; });
    presenter.openPoll(createdResult.pollId);
    REQUIRE(pumpUntil([&] { return gotOpened; }));

    polls::GetPollStateResult afterComment;
    bool commented = false;
    QObject::connect(&presenter, &polls::gui::PollPresenter::commentAdded, [&](polls::GetPollStateResult result) {
        afterComment = std::move(result);
        commented = true;
    });
    presenter.addComment(polls::AddComment{.participantName = "alice", .body = "works for me"});
    REQUIRE(pumpUntil([&] { return commented; }));
    REQUIRE_FALSE(presenter.busy());
    REQUIRE(afterComment.comments.size() == 1);
    CHECK(afterComment.comments.front().body == "works for me");
    CHECK(afterComment.comments.front().participantName == "alice");
}

TEST_CASE("PollPresenter::finalizePoll marks the poll finalized given the admin token, all three backend modes",
          "[polls][presenter]") {
    const auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto rig = makeRig(mode);
    polls::gui::PollPresenter presenter{rig->bridge(0), rig->executor()};

    polls::CreatePollResult createdResult;
    bool created = false;
    QObject::connect(&presenter, &polls::gui::PollPresenter::created, [&](polls::CreatePollResult result) {
        createdResult = std::move(result);
        created = true;
    });
    presenter.createPoll(polls::CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    REQUIRE(pumpUntil([&] { return created; }));

    polls::GetPollStateResult opened;
    bool gotOpened = false;
    QObject::connect(&presenter, &polls::gui::PollPresenter::opened, [&](polls::GetPollStateResult result) {
        opened = std::move(result);
        gotOpened = true;
    });
    presenter.openPoll(createdResult.pollId);
    REQUIRE(pumpUntil([&] { return gotOpened; }));

    // The bare (unsigned) admin token in Context::token is this rung's whole
    // admin identity -- see this file's own top comment.
    morph::session::Context ctx;
    ctx.token = *createdResult.adminToken;
    rig->bridge(0).setDefaultSession(ctx);

    polls::GetPollStateResult finalizedResult;
    bool finalizedFired = false;
    QObject::connect(&presenter, &polls::gui::PollPresenter::finalized, [&](polls::GetPollStateResult result) {
        finalizedResult = std::move(result);
        finalizedFired = true;
    });
    presenter.finalizePoll(polls::FinalizePoll{.optionId = opened.options[0].id});
    REQUIRE(pumpUntil([&] { return finalizedFired; }));
    REQUIRE_FALSE(presenter.busy());
    CHECK(finalizedResult.finalized == polls::Finalized::Yes);
    CHECK(finalizedResult.finalizedOptionId == opened.options[0].id);
}

TEST_CASE("PollPresenter::undoLastVoteChange reverses a participant's own last vote, all three backend modes",
          "[polls][presenter]") {
    const auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto rig = makeRig(mode);
    polls::gui::PollPresenter presenter{rig->bridge(0), rig->executor()};

    polls::CreatePollResult createdResult;
    bool created = false;
    QObject::connect(&presenter, &polls::gui::PollPresenter::created, [&](polls::CreatePollResult result) {
        createdResult = std::move(result);
        created = true;
    });
    presenter.createPoll(polls::CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    REQUIRE(pumpUntil([&] { return created; }));

    polls::GetPollStateResult opened;
    bool gotOpened = false;
    QObject::connect(&presenter, &polls::gui::PollPresenter::opened, [&](polls::GetPollStateResult result) {
        opened = std::move(result);
        gotOpened = true;
    });
    presenter.openPoll(createdResult.pollId);
    REQUIRE(pumpUntil([&] { return gotOpened; }));

    bool submitted = false;
    QObject::connect(&presenter, &polls::gui::PollPresenter::votesSubmitted,
                     [&](polls::GetPollStateResult) { submitted = true; });
    presenter.submitVotes(polls::SubmitVotes{
        .participantName = "alice", .votes = {{.optionId = opened.options[0].id, .choice = polls::VoteChoice::Yes}}});
    REQUIRE(pumpUntil([&] { return submitted; }));

    polls::UndoLastVoteChangeResult undoResult;
    bool undone = false;
    QObject::connect(&presenter, &polls::gui::PollPresenter::voteChangeUndone,
                     [&](polls::UndoLastVoteChangeResult result) {
                         undoResult = result;
                         undone = true;
                     });
    presenter.undoLastVoteChange(polls::UndoLastVoteChange{.participantName = "alice"});
    REQUIRE(pumpUntil([&] { return undone; }));
    REQUIRE_FALSE(presenter.busy());
    CHECK(undoResult.restored == polls::Restored::Yes);

    polls::GetPollStateResult afterUndo;
    bool gotState = false;
    QObject::connect(&presenter, &polls::gui::PollPresenter::stateLoaded, [&](polls::GetPollStateResult result) {
        afterUndo = std::move(result);
        gotState = true;
    });
    presenter.getPollState(polls::GetPollState{});
    REQUIRE(pumpUntil([&] { return gotState; }));
    CHECK(afterUndo.votes.empty());
    CHECK(afterUndo.options[0].yesCount == polls::Count::fromDouble(0.0));
}

TEST_CASE(
    "PollPresenter::getEventsSince returns every event recorded on this handler's attached poll, "
    "all three backend modes",
    "[polls][presenter]") {
    const auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto rig = makeRig(mode);
    polls::gui::PollPresenter presenter{rig->bridge(0), rig->executor()};

    polls::CreatePollResult createdResult;
    bool created = false;
    QObject::connect(&presenter, &polls::gui::PollPresenter::created, [&](polls::CreatePollResult result) {
        createdResult = std::move(result);
        created = true;
    });
    presenter.createPoll(polls::CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    REQUIRE(pumpUntil([&] { return created; }));

    polls::GetPollStateResult opened;
    bool gotOpened = false;
    QObject::connect(&presenter, &polls::gui::PollPresenter::opened, [&](polls::GetPollStateResult result) {
        opened = std::move(result);
        gotOpened = true;
    });
    presenter.openPoll(createdResult.pollId);
    REQUIRE(pumpUntil([&] { return gotOpened; }));

    bool submitted = false;
    QObject::connect(&presenter, &polls::gui::PollPresenter::votesSubmitted,
                     [&](polls::GetPollStateResult) { submitted = true; });
    presenter.submitVotes(polls::SubmitVotes{
        .participantName = "alice", .votes = {{.optionId = opened.options[0].id, .choice = polls::VoteChoice::Yes}}});
    REQUIRE(pumpUntil([&] { return submitted; }));

    bool commented = false;
    QObject::connect(&presenter, &polls::gui::PollPresenter::commentAdded,
                     [&](polls::GetPollStateResult) { commented = true; });
    presenter.addComment(polls::AddComment{.participantName = "alice", .body = "hi"});
    REQUIRE(pumpUntil([&] { return commented; }));

    polls::GetEventsSinceResult events;
    bool gotEvents = false;
    QObject::connect(&presenter, &polls::gui::PollPresenter::eventsReceived, [&](polls::GetEventsSinceResult result) {
        events = std::move(result);
        gotEvents = true;
    });
    presenter.getEventsSince(polls::GetEventsSince{});
    REQUIRE(pumpUntil([&] { return gotEvents; }));
    REQUIRE_FALSE(presenter.busy());
    REQUIRE(events.events.size() == 2);
    CHECK(events.events[0].kind == "vote");
    CHECK(events.events[1].kind == "comment");
    CHECK(events.events[0].id.value < events.events[1].id.value);
}

TEST_CASE("Every PollPresenter validation-driven action routes its failure to failed(), not just createPoll()",
          "[polls][presenter]") {
    // Not a completeness ritual: each action's `reportError` is wired
    // independently at its own `track()` call site (`poll_presenter.cpp`),
    // so a passing test for one action says nothing about whether another
    // action's wiring is correct. See test_bookmark_presenter.cpp's
    // identical test for the same rationale.
    // getPollState/getEventsSince are excluded here (both have
    // `validate() { return true; }` unconditionally -- their only reachable
    // failure is the genuine "never attached via openPoll" NotFound covered
    // by the dedicated case below.
    DbFixture fixture;
    auto rig = makeRig(Mode::Local);
    polls::gui::PollPresenter presenter{rig->bridge(0), rig->executor()};

    QString failure;
    int failures = 0;
    QObject::connect(&presenter, &polls::gui::PollPresenter::failed, [&](QString message) {
        failure = message;
        ++failures;
    });

    // createPoll: empty title and no options both fail CreatePoll::validate().
    presenter.createPoll(polls::CreatePoll{});
    REQUIRE(pumpUntil([&] { return failures == 1; }));
    REQUIRE_FALSE(presenter.busy());

    // openPoll: an empty pollId fails OpenPoll::validate().
    presenter.openPoll("");
    REQUIRE(pumpUntil([&] { return failures == 2; }));
    REQUIRE_FALSE(presenter.busy());

    // submitVotes/updateVotes: empty participantName and empty votes both fail validate().
    presenter.submitVotes(polls::SubmitVotes{});
    REQUIRE(pumpUntil([&] { return failures == 3; }));
    presenter.updateVotes(polls::UpdateVotes{});
    REQUIRE(pumpUntil([&] { return failures == 4; }));
    REQUIRE_FALSE(presenter.busy());

    // addComment: empty participantName/body fails validate().
    presenter.addComment(polls::AddComment{});
    REQUIRE(pumpUntil([&] { return failures == 5; }));
    REQUIRE_FALSE(presenter.busy());

    // finalizePoll: a disengaged optionId fails validate().
    presenter.finalizePoll(polls::FinalizePoll{});
    REQUIRE(pumpUntil([&] { return failures == 6; }));
    REQUIRE_FALSE(presenter.busy());

    // undoLastVoteChange: an empty participantName fails validate().
    presenter.undoLastVoteChange(polls::UndoLastVoteChange{});
    REQUIRE(pumpUntil([&] { return failures == 7; }));
    REQUIRE_FALSE(presenter.busy());
    CHECK_FALSE(failure.isEmpty());
}

TEST_CASE(
    "PollPresenter::getPollState and getEventsSince against a handler never attached via openPoll "
    "emit failed, not a crash",
    "[polls][presenter]") {
    // PollModel::execute(GetPollState)/execute(GetEventsSince) both throw
    // NotFound when this handler's own _pollId was never populated by a
    // prior execute(OpenPoll) (poll_model.cpp) -- proves the presenter
    // surfaces that as failed() rather than crashing, using a handler that
    // never called openPoll() at all.
    DbFixture fixture;
    auto rig = makeRig(Mode::Local);
    polls::gui::PollPresenter presenter{rig->bridge(0), rig->executor()};

    QString failure;
    int failures = 0;
    QObject::connect(&presenter, &polls::gui::PollPresenter::failed, [&](QString message) {
        failure = message;
        ++failures;
    });

    presenter.getPollState(polls::GetPollState{});
    REQUIRE(pumpUntil([&] { return failures == 1; }));
    REQUIRE_FALSE(presenter.busy());

    presenter.getEventsSince(polls::GetEventsSince{});
    REQUIRE(pumpUntil([&] { return failures == 2; }));
    CHECK_FALSE(failure.isEmpty());
    REQUIRE_FALSE(presenter.busy());
}

TEST_CASE("PollPresenter::finalizePoll with no session at all emits failed, not a crash", "[polls][presenter]") {
    // Mirrors test_bookmark_presenter.cpp's own "no session at all" case,
    // adapted to this rung's actual auth shape -- see this file's own top
    // comment. createPoll/openPoll need no session at all (PollsAuthorizer
    // permits everything, and neither action's model code checks
    // session::current()); only finalizePoll's requireAdmin() genuinely
    // checks Context::token, so a bridge that never had setDefaultSession
    // called on it reaches that check with an empty token, which can never
    // equal a real admin token.
    DbFixture fixture;
    auto rig = makeRig(Mode::Local);
    polls::gui::PollPresenter presenter{rig->bridge(0), rig->executor()};

    polls::CreatePollResult createdResult;
    bool created = false;
    QObject::connect(&presenter, &polls::gui::PollPresenter::created, [&](polls::CreatePollResult result) {
        createdResult = std::move(result);
        created = true;
    });
    presenter.createPoll(polls::CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    REQUIRE(pumpUntil([&] { return created; }));

    polls::GetPollStateResult opened;
    bool gotOpened = false;
    QObject::connect(&presenter, &polls::gui::PollPresenter::opened, [&](polls::GetPollStateResult result) {
        opened = std::move(result);
        gotOpened = true;
    });
    presenter.openPoll(createdResult.pollId);
    REQUIRE(pumpUntil([&] { return gotOpened; }));

    QString failure;
    bool failed = false;
    QObject::connect(&presenter, &polls::gui::PollPresenter::failed, [&](QString message) {
        failure = message;
        failed = true;
    });
    presenter.finalizePoll(polls::FinalizePoll{.optionId = opened.options[0].id});
    REQUIRE(pumpUntil([&] { return failed; }));
    CHECK_FALSE(failure.isEmpty());
    REQUIRE_FALSE(presenter.busy());
}
