// SPDX-License-Identifier: Apache-2.0
//
// PollModel's model-level suite. Task 5's cases: CreatePoll's generated
// tokens, OpenPoll finding the poll it created (and the keyed-attach
// pollId cache GetPollState later reads -- exercised indirectly via
// OpenPoll's returned state), and NotFound on an unknown pollId. Task 6
// appends SubmitVotes/UpdateVotes/AddComment: one-vote-per-option tallying,
// retry-idempotency (the DoD's "participant-token + option uniqueness is a
// model invariant, tested under retry" requirement), wholesale replacement,
// and the finalized-poll Conflict dead-letter both vote-writing actions and
// AddComment share.
#include "polls/core/errors.hpp"
#include "polls/core/types.hpp"
#include "polls/dto/event_dto.hpp"
#include "polls/dto/poll_dto.hpp"
#include "polls/dto/vote_dto.hpp"
#include "polls/models/poll_model.hpp"
#include "testkit/db_fixture.hpp"

// Task 9's own instance-rebirth test drives PollModel through real
// BridgeHandlers over a real Bridge/backend (BackendRig), not direct
// PollModel::execute() calls -- the only way to make one PollModel instance
// genuinely die (last handler naming its key destructed) and a fresh one take
// its place, per this rung's shared-instance design (BRIDGE_MODEL_KEY(
// polls::PollModel, polls::OpenPoll, &polls::OpenPoll::pollId) in
// poll_model.hpp).
#include <morph/core/bridge.hpp>

#include "testkit/backend_rig.hpp"
#include "testkit/pump.hpp"

// Test-only: FinalizePoll (Task 7) does not exist yet, so the two
// finalized-poll Conflict cases below reach into the entity directly to put
// a poll into the finalized state -- the same untransacted single-row
// mapper.Update() pattern test_bookmarks_schema.cpp/test_polls_schema.cpp
// already use for a direct DataMapper write (not test_bookmark_model.cpp,
// which only ever reads entities directly, never writes them). Production
// model code never does this (poll_model.cpp's own file comment: the entity
// is a poll_model.cpp-only implementation detail) -- this is the test
// harness reaching past that boundary on purpose, not a precedent for
// application code.
#include <Lightweight/DataMapper/DataMapper.hpp>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <morph/session/session.hpp>

#include "polls/db/poll_entity.hpp"

using morph::bridge::AllowShared;
using morph::bridge::BridgeHandler;
using morph::ladder::testkit::awaitQt;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using polls::AddComment;
using polls::Conflict;
using polls::CreatePoll;
using polls::FinalizePoll;
using polls::Forbidden;
using polls::GetEventsSince;
using polls::NotFound;
using polls::OpenPoll;
using polls::PollModel;
using polls::SubmitVotes;
using polls::UndoLastVoteChange;
using polls::UpdateVotes;
using polls::VoteChoice;

namespace {

/// @brief A `Context` carrying only @p token. Built field-by-field rather
///        than a designated initializer, for the identical
///        `-Wmissing-designated-field-initializers` reason
///        `test_bookmark_model.cpp`'s `contextFor` exists.
[[nodiscard]] morph::session::Context contextForToken(std::string token) {
    morph::session::Context ctx;
    ctx.token = std::move(token);
    return ctx;
}

/// @brief Installs a `Context` carrying only a bearer token, thread-locally,
///        for its scope. Same shape as `test_bookmark_model.cpp`'s
///        `ScopedPrincipal`, adapted to this rung's bearer-token-not-principal
///        design (README design decision 1): `PollModel::requireAdmin()`
///        reads `session::current()->token`, never `->principal`.
class ScopedToken {
public:
    explicit ScopedToken(std::string token) : _ctx{contextForToken(std::move(token))}, _scope{_ctx} {}

private:
    morph::session::Context _ctx;
    morph::session::detail::ScopedContext _scope;
};

/// @brief Marks the poll named by @p pollId finalized, bypassing
///        `FinalizePoll` (not implemented until Task 7) -- see the file
///        comment above.
void finalizePollDirectly(const std::string& pollId) {
    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<polls::db::PollRecord>()
                    .Where(Lightweight::FieldNameOf<&polls::db::PollRecord::pollId>, "=", pollId)
                    .All();
    REQUIRE_FALSE(rows.empty());
    auto& poll = rows.front();
    poll.finalized = true;
    mapper.Update(poll);
}

}  // namespace

TEST_CASE("CreatePoll returns three distinct tokens and OpenPoll finds the same poll", "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    auto created = model.execute(CreatePoll{.title = "Team offsite", .options = {{"2026-09-01"}, {"2026-09-02"}}});
    CHECK_FALSE(created.pollId.empty());
    REQUIRE(created.adminToken.hasValue());
    REQUIRE(created.participantToken.hasValue());
    CHECK_FALSE((*created.adminToken).empty());
    CHECK_FALSE((*created.participantToken).empty());
    CHECK(created.pollId != *created.adminToken);
    CHECK(created.pollId != *created.participantToken);
    // Compared through the payloads: the two newtypes are deliberately
    // different C++ types, so there is no cross-type `!=` to reach for.
    CHECK(*created.adminToken != *created.participantToken);

    auto state = model.execute(OpenPoll{.pollId = created.pollId});
    CHECK(state.pollId == created.pollId);
    CHECK(state.title == "Team offsite");
    CHECK(state.options.size() == 2);
    CHECK(state.options[0].label == "2026-09-01");
    CHECK(state.options[1].label == "2026-09-02");
    CHECK(state.finalized == polls::Finalized::No);
    CHECK(state.votes.empty());
    CHECK(state.comments.empty());
}

TEST_CASE("OpenPoll against an unknown pollId throws NotFound", "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    CHECK_THROWS_AS(model.execute(OpenPoll{.pollId = "no-such-poll"}), NotFound);
}

TEST_CASE("Two CreatePoll calls never collide on pollId/adminToken/participantToken", "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    auto a = model.execute(CreatePoll{.title = "A", .options = {{"1"}, {"2"}}});
    auto b = model.execute(CreatePoll{.title = "B", .options = {{"1"}, {"2"}}});
    CHECK(a.pollId != b.pollId);
    CHECK(a.adminToken != b.adminToken);
    CHECK(a.participantToken != b.participantToken);
}

TEST_CASE("GetPollState after OpenPoll returns the same poll's state", "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    auto created = model.execute(CreatePoll{.title = "Lunch spot", .options = {{"Cafe"}, {"Diner"}}});
    (void)model.execute(OpenPoll{.pollId = created.pollId});

    auto state = model.execute(polls::GetPollState{});
    CHECK(state.pollId == created.pollId);
    CHECK(state.title == "Lunch spot");
    CHECK(state.options.size() == 2);
}

TEST_CASE("GetPollState on a fresh handler never attached via OpenPoll throws NotFound", "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    CHECK_THROWS_AS(model.execute(polls::GetPollState{}), NotFound);
}

TEST_CASE("CreatePoll's validate() rejects an empty title and out-of-range option counts", "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    CHECK_THROWS_AS(model.execute(CreatePoll{.title = "", .options = {{"1"}, {"2"}}}), polls::ValidationError);
    CHECK_THROWS_AS(model.execute(CreatePoll{.title = "T", .options = {{"1"}}}), polls::ValidationError);
    CHECK_THROWS_AS(model.execute(CreatePoll{.title = "T", .options = {}}), polls::ValidationError);
}

TEST_CASE("SubmitVotes writes one vote per option, visible in the next GetPollState", "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    auto created = model.execute(CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    model.execute(OpenPoll{.pollId = created.pollId});
    auto opts = model.execute(polls::GetPollState{}).options;

    auto state = model.execute(SubmitVotes{.participantName = "alice",
                                           .votes = {{.optionId = opts[0].id, .choice = VoteChoice::Yes},
                                                     {.optionId = opts[1].id, .choice = VoteChoice::No}}});
    CHECK(state.options[0].yesCount == polls::Count::fromDouble(1.0));
    CHECK(state.options[1].noCount == polls::Count::fromDouble(1.0));
    REQUIRE(state.votes.size() == 2);
}

TEST_CASE("A retried SubmitVotes for the same participant does not double-count", "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    auto created = model.execute(CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    model.execute(OpenPoll{.pollId = created.pollId});
    auto opts = model.execute(polls::GetPollState{}).options;
    SubmitVotes action{.participantName = "alice", .votes = {{.optionId = opts[0].id, .choice = VoteChoice::Yes}}};
    model.execute(action);
    // The DoD names this as a retry scenario: the strand serializes but does
    // not dedup by itself, so the model's own unique constraint (backed by
    // applyVotes()'s delete-then-recreate) is what actually prevents
    // double-counting -- assert on the real outcome, not the mechanism.
    auto state = model.execute(action);                                 // retried identically
    CHECK(state.options[0].yesCount == polls::Count::fromDouble(1.0));  // still 1, not 2
    REQUIRE(state.votes.size() == 1);
}

TEST_CASE("UpdateVotes replaces a participant's prior votes wholesale", "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    auto created = model.execute(CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    model.execute(OpenPoll{.pollId = created.pollId});
    auto opts = model.execute(polls::GetPollState{}).options;
    model.execute(
        SubmitVotes{.participantName = "alice", .votes = {{.optionId = opts[0].id, .choice = VoteChoice::Yes}}});
    auto state = model.execute(
        UpdateVotes{.participantName = "alice", .votes = {{.optionId = opts[1].id, .choice = VoteChoice::Yes}}});
    CHECK(state.options[0].yesCount == polls::Count::fromDouble(0.0));  // alice's old vote is gone
    CHECK(state.options[1].yesCount == polls::Count::fromDouble(1.0));
    REQUIRE(state.votes.size() == 1);
}

TEST_CASE("SubmitVotes against a finalized poll throws Conflict, a visible dead-letter outcome", "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    auto created = model.execute(CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    model.execute(OpenPoll{.pollId = created.pollId});
    auto opts = model.execute(polls::GetPollState{}).options;
    finalizePollDirectly(created.pollId);
    CHECK_THROWS_AS(model.execute(SubmitVotes{.participantName = "bob",
                                              .votes = {{.optionId = opts[0].id, .choice = VoteChoice::Yes}}}),
                    Conflict);
}

TEST_CASE("AddComment writes a comment visible in the next GetPollState", "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    auto created = model.execute(CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    model.execute(OpenPoll{.pollId = created.pollId});
    auto state = model.execute(AddComment{.participantName = "alice", .body = "works for me"});
    REQUIRE(state.comments.size() == 1);
    CHECK(state.comments.front().body == "works for me");
}

TEST_CASE("AddComment against a finalized poll throws Conflict -- finalizing makes the poll read-only",
          "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    auto created = model.execute(CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    model.execute(OpenPoll{.pollId = created.pollId});
    finalizePollDirectly(created.pollId);
    CHECK_THROWS_AS(model.execute(AddComment{.participantName = "alice", .body = "too late"}), Conflict);
}

TEST_CASE("FinalizePoll requires the admin token in Context::token", "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    auto created = model.execute(CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    model.execute(OpenPoll{.pollId = created.pollId});
    auto opts = model.execute(polls::GetPollState{}).options;

    // No token at all:
    CHECK_THROWS_AS(model.execute(FinalizePoll{.optionId = opts[0].id}), Forbidden);

    // Wrong token (the participant token, not the admin token): still
    // Forbidden, not a silent success -- a participant may never finalize.
    {
        const ScopedToken scoped{*created.participantToken};
        CHECK_THROWS_AS(model.execute(FinalizePoll{.optionId = opts[0].id}), Forbidden);
    }

    // Right token:
    {
        const ScopedToken scoped{*created.adminToken};
        auto state = model.execute(FinalizePoll{.optionId = opts[0].id});
        CHECK(state.finalized == polls::Finalized::Yes);
        CHECK(state.finalizedOptionId == opts[0].id);
    }
}

TEST_CASE("Finalizing an already-finalized poll throws Conflict", "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    auto created = model.execute(CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    model.execute(OpenPoll{.pollId = created.pollId});
    auto opts = model.execute(polls::GetPollState{}).options;
    const ScopedToken scoped{*created.adminToken};
    model.execute(FinalizePoll{.optionId = opts[0].id});
    CHECK_THROWS_AS(model.execute(FinalizePoll{.optionId = opts[1].id}), Conflict);
}

TEST_CASE("FinalizePoll's admin-token check runs before the already-finalized check", "[polls][model]") {
    // A wrong-token caller against an *already-finalized* poll must still see
    // Forbidden, never Conflict -- Conflict would leak "this poll is already
    // finalized" to a caller who has not proven they may act on it at all.
    // See poll_model.cpp's own comment on this ordering.
    DbFixture fixture;
    PollModel model;
    auto created = model.execute(CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    model.execute(OpenPoll{.pollId = created.pollId});
    auto opts = model.execute(polls::GetPollState{}).options;
    {
        const ScopedToken scoped{*created.adminToken};
        model.execute(FinalizePoll{.optionId = opts[0].id});
    }
    {
        const ScopedToken scoped{*created.participantToken};
        CHECK_THROWS_AS(model.execute(FinalizePoll{.optionId = opts[1].id}), Forbidden);
    }
}

TEST_CASE("FinalizePoll rejects an optionId belonging to a different poll", "[polls][model]") {
    // /code-review max finding: finalizedOptionId is FK-shaped but not
    // FK-enforced (SQLite), so without an explicit membership check a poll
    // could finalize with an option id that exists, but belongs to some
    // *other* poll entirely.
    DbFixture fixture;
    PollModel modelA;
    auto createdA = modelA.execute(CreatePoll{.title = "Poll A", .options = {{"1"}, {"2"}}});
    modelA.execute(OpenPoll{.pollId = createdA.pollId});

    PollModel modelB;
    auto createdB = modelB.execute(CreatePoll{.title = "Poll B", .options = {{"3"}, {"4"}}});
    modelB.execute(OpenPoll{.pollId = createdB.pollId});
    auto optsB = modelB.execute(polls::GetPollState{}).options;

    const ScopedToken scoped{*createdA.adminToken};
    CHECK_THROWS_AS(modelA.execute(FinalizePoll{.optionId = optsB[0].id}), NotFound);

    // Poll A must still be genuinely unfinalized -- the rejected attempt left
    // no partial state behind.
    auto stateA = modelA.execute(polls::GetPollState{});
    CHECK(stateA.finalized == polls::Finalized::No);
}

TEST_CASE("SubmitVotes rejects a vote naming an optionId from a different poll, atomically", "[polls][model]") {
    // /code-review max finding: without this check, a cross-poll vote would
    // be written but never counted by buildState()'s per-poll tally loop --
    // the participant is told they voted, and the vote silently vanishes.
    DbFixture fixture;
    PollModel modelA;
    auto createdA = modelA.execute(CreatePoll{.title = "Poll A", .options = {{"1"}, {"2"}}});
    modelA.execute(OpenPoll{.pollId = createdA.pollId});
    auto optsA = modelA.execute(polls::GetPollState{}).options;

    PollModel modelB;
    auto createdB = modelB.execute(CreatePoll{.title = "Poll B", .options = {{"3"}, {"4"}}});
    modelB.execute(OpenPoll{.pollId = createdB.pollId});
    auto optsB = modelB.execute(polls::GetPollState{}).options;

    // One valid vote (poll A's own option) plus one cross-poll vote (poll
    // B's option) in the same submission -- the whole call must be rejected,
    // not partially applied.
    CHECK_THROWS_AS(modelA.execute(SubmitVotes{.participantName = "alice",
                                               .votes = {{.optionId = optsA[0].id, .choice = VoteChoice::Yes},
                                                         {.optionId = optsB[0].id, .choice = VoteChoice::No}}}),
                    NotFound);

    // Nothing was written -- not even the valid first vote.
    auto stateA = modelA.execute(polls::GetPollState{});
    CHECK(stateA.votes.empty());
}

TEST_CASE("SubmitVotes rejects two votes naming the same optionId with ValidationError, not a raw SQL error",
          "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    auto created = model.execute(CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    model.execute(OpenPoll{.pollId = created.pollId});
    auto opts = model.execute(polls::GetPollState{}).options;

    CHECK_THROWS_AS(model.execute(SubmitVotes{.participantName = "alice",
                                              .votes = {{.optionId = opts[0].id, .choice = VoteChoice::Yes},
                                                        {.optionId = opts[0].id, .choice = VoteChoice::No}}}),
                    polls::ValidationError);
}

TEST_CASE("GetEventsSince rejects a negative lastEventId with ValidationError", "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    auto created = model.execute(CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    model.execute(OpenPoll{.pollId = created.pollId});

    CHECK_THROWS_AS(model.execute(GetEventsSince{.lastEventId = polls::PollEventId{.value = -1}}),
                    polls::ValidationError);
}

// ---------------------------------------------------------------------------
// Task 8: UndoLastVoteChange. Per this task's own brief, the interleaving
// test below is written and run FIRST, before execute(UndoLastVoteChange)
// has a body -- its outcome is this rung's headline design record: proof
// that a principal-scoped compensating action can do what
// SessionLog::undoLast() (docs/spec/journal/journal.md) structurally
// cannot, since that API pops the newest journal entry regardless of which
// principal made it, and hands back a detached model holder no API can
// install into a live shared instance.
// ---------------------------------------------------------------------------

TEST_CASE(
    "Principal-scoped undo: A votes, B votes, A undoes -> only A's vote dies (the rung's headline design record)",
    "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    auto created = model.execute(CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    model.execute(OpenPoll{.pollId = created.pollId});
    auto opts = model.execute(polls::GetPollState{}).options;

    model.execute(
        SubmitVotes{.participantName = "alice", .votes = {{.optionId = opts[0].id, .choice = VoteChoice::Yes}}});
    model.execute(
        SubmitVotes{.participantName = "bob", .votes = {{.optionId = opts[0].id, .choice = VoteChoice::Yes}}});
    // Both voted yes on option 0: count should be 2.
    auto before = model.execute(polls::GetPollState{});
    REQUIRE(before.options[0].yesCount == polls::Count::fromDouble(2.0));

    auto undoResult = model.execute(UndoLastVoteChange{.participantName = "alice"});
    CHECK(undoResult.restored == polls::Restored::Yes);

    auto after = model.execute(polls::GetPollState{});
    // Alice's vote is gone; Bob's survives. This is the assertion that
    // SessionLog::undoLast() could never make true: it pops the newest
    // entry regardless of principal, which would have killed Bob's vote
    // (the more recent of the two), not Alice's own.
    CHECK(after.options[0].yesCount == polls::Count::fromDouble(1.0));
    const bool bobStillVotes =
        std::ranges::any_of(after.votes, [](const auto& v) { return v.participantName == "bob"; });
    const bool aliceStillVotes =
        std::ranges::any_of(after.votes, [](const auto& v) { return v.participantName == "alice"; });
    CHECK(bobStillVotes);
    CHECK_FALSE(aliceStillVotes);
}

TEST_CASE("UndoLastVoteChange with nothing to undo throws Conflict", "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    auto created = model.execute(CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    model.execute(OpenPoll{.pollId = created.pollId});
    CHECK_THROWS_AS(model.execute(UndoLastVoteChange{.participantName = "nobody-voted"}), Conflict);
}

TEST_CASE("Undo is one-shot: undoing twice in a row throws Conflict the second time", "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    auto created = model.execute(CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    model.execute(OpenPoll{.pollId = created.pollId});
    auto opts = model.execute(polls::GetPollState{}).options;
    model.execute(
        SubmitVotes{.participantName = "alice", .votes = {{.optionId = opts[0].id, .choice = VoteChoice::Yes}}});
    model.execute(UndoLastVoteChange{.participantName = "alice"});
    CHECK_THROWS_AS(model.execute(UndoLastVoteChange{.participantName = "alice"}), Conflict);
}

TEST_CASE("UndoLastVoteChange restores a genuinely non-empty prior vote set, not just \"no vote\"", "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    auto created = model.execute(CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    model.execute(OpenPoll{.pollId = created.pollId});
    auto opts = model.execute(polls::GetPollState{}).options;

    model.execute(SubmitVotes{.participantName = "alice",
                              .votes = {{.optionId = opts[0].id, .choice = VoteChoice::Yes},
                                        {.optionId = opts[1].id, .choice = VoteChoice::No}}});
    model.execute(
        UpdateVotes{.participantName = "alice", .votes = {{.optionId = opts[1].id, .choice = VoteChoice::IfNeedBe}}});
    model.execute(UndoLastVoteChange{.participantName = "alice"});

    auto after = model.execute(polls::GetPollState{});
    CHECK(after.votes.size() == 2);
    CHECK(after.options[0].yesCount == polls::Count::fromDouble(1.0));
    CHECK(after.options[1].noCount == polls::Count::fromDouble(1.0));
    CHECK(after.options[1].ifNeedBeCount == polls::Count::fromDouble(0.0));
}

// ---------------------------------------------------------------------------
// Task 9: GetEventsSince -- the Zulip-pattern event log's read side. Every
// mutating action above already appends a PollEventRecord (SubmitVotes/
// UpdateVotes/AddComment/FinalizePoll/UndoLastVoteChange, exercised by the
// tests above); these cases read that log back out.
// ---------------------------------------------------------------------------

TEST_CASE("GetEventsSince{} (from the beginning) returns every event in order", "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    auto created = model.execute(CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    model.execute(OpenPoll{.pollId = created.pollId});
    auto opts = model.execute(polls::GetPollState{}).options;
    model.execute(
        SubmitVotes{.participantName = "alice", .votes = {{.optionId = opts[0].id, .choice = VoteChoice::Yes}}});
    model.execute(AddComment{.participantName = "alice", .body = "hi"});

    auto events = model.execute(GetEventsSince{}).events;
    REQUIRE(events.size() == 2);
    CHECK(events[0].kind == "vote");
    CHECK(events[1].kind == "comment");
    CHECK(events[0].id.value < events[1].id.value);  // strictly increasing
}

TEST_CASE("GetEventsSince{lastEventId} returns only strictly-newer events", "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    auto created = model.execute(CreatePoll{.title = "T", .options = {{"1"}, {"2"}}});
    model.execute(OpenPoll{.pollId = created.pollId});
    auto opts = model.execute(polls::GetPollState{}).options;
    model.execute(
        SubmitVotes{.participantName = "alice", .votes = {{.optionId = opts[0].id, .choice = VoteChoice::Yes}}});
    auto firstEvents = model.execute(GetEventsSince{}).events;
    REQUIRE(firstEvents.size() == 1);

    model.execute(AddComment{.participantName = "alice", .body = "hi"});
    auto newEvents = model.execute(GetEventsSince{.lastEventId = firstEvents.front().id}).events;
    REQUIRE(newEvents.size() == 1);
    CHECK(newEvents.front().kind == "comment");
}

TEST_CASE("GetEventsSince throws NotFound against a handler never attached via OpenPoll", "[polls][model]") {
    DbFixture fixture;
    PollModel model;
    CHECK_THROWS_AS(model.execute(GetEventsSince{}), NotFound);
}

TEST_CASE(
    "The event log survives full detach/reattach (instance rebirth), and a stale cursor "
    "gets everything after it -- no epoch token needed",
    "[polls][model]") {
    // This is the DoD's own required test: "Event log survives full
    // detach/reattach (instance rebirth) and a stale cursor triggers a clean
    // full resync, verified by test." Per this rung's resolved design
    // decision (durable persistence alone closes the Zulip-pattern gap, no
    // epoch token needed): "clean full resync" here means the stale cursor
    // simply gets every real event since it, correctly, because poll_events'
    // autoincrement id survived the instance's death regardless of which
    // in-memory PollModel wrote which row.
    //
    // Goes through real BridgeHandlers over a real Bridge/backend
    // (BackendRig{Mode::Local, ...}), not direct PollModel::execute() calls
    // -- direct calls construct their own private PollModel per test-local
    // variable and never touch the shared instance directory at all, so
    // there would be no instance to kill. Two AllowShared handlers attach to
    // the same pollId (proving one shared instance, not two -- instances()
    // reports exactly one live key), both then go out of scope, and
    // BridgeHandler<PollModel, AllowShared>::instances() confirms the
    // directory is genuinely empty afterward -- not merely "the test didn't
    // crash". A fresh handler then reattaches and GetEventsSince with the
    // pre-death cursor gets exactly the events written after it.
    DbFixture fixture;
    BackendRig rig{Mode::Local, 1};

    std::string pollId;
    polls::PollEventId lastEventId;
    {
        // A plain (NoSharing) handler for CreatePoll: an AllowShared handler
        // that has never attached refuses every keyless action ("handler not
        // bound" -- see BridgeHandler<Model, AllowShared>'s own doc comment,
        // morph/core/bridge.hpp), and CreatePoll carries no BRIDGE_KEY_FROM
        // of its own to attach by.
        BridgeHandler<PollModel> creator{rig.bridge(0), rig.executor()};
        auto created = awaitQt(creator.execute(CreatePoll{.title = "T", .options = {{"1"}, {"2"}}}));
        pollId = created.pollId;

        // Two shared handlers naming the same key -- both land on one
        // instance (mirrors bank's "two shared handlers on one account reach
        // one instance", test_stateful_account.cpp).
        BridgeHandler<PollModel, AllowShared> handlerA{rig.bridge(0), rig.executor()};
        BridgeHandler<PollModel, AllowShared> handlerB{rig.bridge(0), rig.executor()};
        auto state = awaitQt(handlerA.execute(OpenPoll{.pollId = pollId}));
        (void)awaitQt(handlerB.execute(OpenPoll{.pollId = pollId}));
        REQUIRE(awaitQt(handlerA.instances()) == std::vector<std::string>{pollId});

        awaitQt(handlerA.execute(SubmitVotes{
            .participantName = "alice", .votes = {{.optionId = state.options[0].id, .choice = VoteChoice::Yes}}}));
        auto events = awaitQt(handlerB.execute(GetEventsSince{})).events;
        REQUIRE(events.size() == 1);
        lastEventId = events.back().id;

        // handlerA/handlerB (the only two handlers naming this poll's key)
        // and creator (never in the directory to begin with) all go out of
        // scope at the end of this block -- releasing the shared instance,
        // which destructs. This is the "instance rebirth" this test proves:
        // there is now no live PollModel instance for this poll anywhere.
    }

    // Real destruction, not assumed: a fresh AllowShared handler's own
    // instances() call shows an empty directory, not merely "no crash".
    {
        BridgeHandler<PollModel, AllowShared> prober{rig.bridge(0), rig.executor()};
        REQUIRE(awaitQt(prober.instances()).empty());
    }

    // Fresh handler -> a brand-new PollModel instance, re-attached from
    // scratch via OpenPoll (its own _pollId cache starts unset, exactly like
    // any other freshly-constructed PollModel). The event log itself lives in
    // SQLite, not in that now-dead instance's memory, so it is untouched.
    BridgeHandler<PollModel, AllowShared> handlerC{rig.bridge(0), rig.executor()};
    auto reopened = awaitQt(handlerC.execute(OpenPoll{.pollId = pollId}));
    REQUIRE(reopened.lastEventId == lastEventId);  // durable across the instance's death

    awaitQt(handlerC.execute(AddComment{.participantName = "bob", .body = "welcome back"}));

    auto sinceStale = awaitQt(handlerC.execute(GetEventsSince{.lastEventId = lastEventId})).events;
    REQUIRE(sinceStale.size() == 1);
    CHECK(sinceStale.front().kind == "comment");
    CHECK(sinceStale.front().id.value > lastEventId.value);
}
