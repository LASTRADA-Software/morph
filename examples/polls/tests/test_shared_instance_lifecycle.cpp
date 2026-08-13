// SPDX-License-Identifier: Apache-2.0
//
// Task 12: three genuinely new pieces of coverage this rung's README names as
// "Expected strain points" that no task above already covers.
//
// 1. The backend-mode matrix for the *keyed* attach path: CreatePoll (a
//    direct, non-keyed call over a plain BridgeHandler, exactly like
//    test_poll_model.cpp's own instance-rebirth test's "creator" handler and
//    test_app.cpp's own "creator") -> handler.execute(OpenPoll{pollId}) to
//    attach -> SubmitVotes -> GetPollState, across Mode::Local,
//    Mode::LocalSingleThread, Mode::Socket. Mirrors rung 2's Task 14
//    (examples/bookmarks/tests/test_bookmark_model.cpp's own
//    GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket) matrix),
//    but rung 2's matrix only ever proved the *plain-registration* path;
//    this proves the *keyed* attach path (registerModelShared/attachModel,
//    docs/spec/core/shared_instances.md) works identically across all three
//    modes.
// 2. Shared-instance lifetime: N BridgeHandler<PollModel, AllowShared>
//    instances attach to the same pollId, observe each other's writes, and
//    handler.instances() reflects the instance's real lifetime (present
//    while attached, absent once every attacher has released it) -- the
//    DoD's own "handler.instances() for an organizer dashboard" requirement.
// 3. Poisoned-instance attach: docs/spec/core/shared_instances.md's
//    "Failure modes" section documents that an instance whose very first
//    action's outcome fails is marked and evicted from the directory "the
//    next time anyone else attaches to that key -- not immediately", and
//    that "the handler that hit the failure does not self-heal: its primary
//    is already set to the poisoned key, so retrying the same keyed action
//    re-points nowhere (attachHandler's no-op-on-same-primary guard skips
//    the backend entirely) -- it keeps its broken instance". This test
//    attaches to a bad pollId twice from the *same* handler: the second
//    execute() never re-attaches (same primary, no-op guard), it just
//    re-dispatches OpenPoll against the same broken instance, and
//    PollModel::execute(OpenPoll) re-runs loadPollByPollId() on every call
//    (poll_model.cpp) -- so both attempts fail identically with NotFound,
//    proving there is no silently half-hydrated success on retry.
//
// Task 13: the last model-layer test task before the rung moves to
// presenters/GUI.
//
// 1. Cross-poll admin-token isolation: PollModel is keyed per-poll (each
//    poll is its own shared instance), so a participant token from poll A
//    must not let its holder finalize poll B. Written explicitly (rather
//    than assumed from the per-instance keying alone) because a bug in
//    requireAdmin()'s poll-row lookup could silently pass.
// 2. Bridge::setExecuteDeadline recovers a call the real rate limiter
//    (QtWebSocketServerConfig::messagesPerSecond) silently drops -- the
//    DoD's "run this rung's harness with messagesPerSecond configured ON"
//    requirement, proven end to end (not merely at the framework-prereqs
//    plan's own unit-test level) for the first time in this rung.
// 3. The cross-model rename-race analogue (rung 2's TagModel-renames-while-
//    BookmarkModel-writes race): this rung's README does not name an exact
//    analogue -- there is only one model type here (PollModel), so that
//    whole test class does not apply. Considered and explicitly skipped,
//    not silently omitted; see this task's commit message.

#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

#include "polls/auth/polls_authorizer.hpp"
#include "polls/dto/poll_dto.hpp"
#include "polls/dto/vote_dto.hpp"
#include "polls/models/poll_model.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <memory>
#include <string>
#include <vector>

using morph::bridge::AllowShared;
using morph::bridge::BridgeHandler;
using morph::ladder::testkit::awaitQt;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;
using polls::CreatePoll;
using polls::FinalizePoll;
using polls::GetPollState;
using polls::OpenPoll;
using polls::PollModel;
using polls::SubmitVotes;
using polls::VoteChoice;

TEST_CASE("PollModel over the full backend-mode matrix: create -> keyed-attach -> submit-vote round trip",
          "[polls][model]") {
    const auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    BackendRig rig{mode, 1, std::make_shared<polls::auth::PollsAuthorizer>()};

    // Plain (NoSharing) handler for CreatePoll: CreatePoll carries no key, so
    // nothing about it is shared/keyed -- the direct, non-keyed call Task 5's
    // own tests (and test_app.cpp's "creator") already use.
    auto creator = rig.client<PollModel>(0);
    const auto created =
        awaitQt(creator.execute(CreatePoll{.title = "Matrix poll", .options = {{"opt-a"}, {"opt-b"}}}));
    REQUIRE_FALSE(created.pollId.empty());

    // A fresh, AllowShared handler attaches via the *keyed* path --
    // handler.execute(OpenPoll{pollId}) -- proving keyed attach (not just
    // plain registration) works identically in every mode.
    BridgeHandler<PollModel, AllowShared> handler{rig.bridge(0), rig.executor()};
    const auto opened = awaitQt(handler.execute(OpenPoll{.pollId = created.pollId}));
    REQUIRE(opened.pollId == created.pollId);
    REQUIRE(opened.options.size() == 2);

    const auto afterVote = awaitQt(handler.execute(
        SubmitVotes{.participantName = "alice",
                    .votes = {{.optionId = opened.options[0].id, .choice = VoteChoice::Yes}}}));
    REQUIRE(afterVote.votes.size() == 1);
    CHECK(afterVote.votes.front().participantName == "alice");
    CHECK(afterVote.votes.front().choice == VoteChoice::Yes);
    CHECK(afterVote.options[0].yesCount == polls::Count::fromDouble(1.0));

    const auto state = awaitQt(handler.execute(GetPollState{}));
    REQUIRE(state.votes.size() == 1);
    CHECK(state.votes.front().participantName == "alice");
}

TEST_CASE("N shared handlers on one pollId observe each other's writes, and instances() reflects "
          "the instance's real lifetime",
          "[polls][model][shared-instances]") {
    DbFixture fixture;
    // 5 clients, not 4: the fifth connection is reserved for the fresh
    // "prober" handler below. Reusing one of the four attached connections
    // for it would race a fire-and-forget deregister's unsolicited (callId
    // 0) "ok" reply -- sent by BridgeHandler::~BridgeHandler on connection
    // teardown, per QtWebSocketBackend::deregisterModel's own doc comment --
    // against the prober's own synchronous instances() call on that same
    // connection: QtWebSocketBackend::onTextMessage matches *any* callId-0
    // reply to whichever sendSync happens to be parked, so a still-in-flight
    // deregister ack can be misdelivered as the instances() reply, corrupting
    // it. A genuinely fresh connection never had a deregister in flight, so
    // it cannot race one. This is the exact mechanism a since-fixed finding
    // was filed against -- a sync *register* racing a deregister; a sync
    // *instances()* call is the identical hazard, since both are ordinary
    // sendSync callers competing for the same callId-0 bucket -- a third
    // independent reproduction site, after rung 2's own Task 17 discovery
    // and QtWebSocketBackend::attachModel's empty-key path hitting it too.
    // QtWebSocketBackend::deregisterModel now assigns a real, tracked callId
    // rather than sharing the zero sentinel, closing the race framework-side;
    // this test's own connection-isolation setup (5 clients, not 4) is kept
    // regardless, since it costs nothing and this test still exercises the
    // same call shape.
    BackendRig rig{Mode::Socket, 5, std::make_shared<polls::auth::PollsAuthorizer>()};

    // Client 0's plain handler creates the poll -- CreatePoll carries no key.
    auto creator = rig.client<PollModel>(0);
    const auto created =
        awaitQt(creator.execute(CreatePoll{.title = "Team lunch", .options = {{"mon"}, {"tue"}, {"wed"}}}));

    // Four independent AllowShared handlers, each its own socket client, all
    // attach to the same pollId -- exercising cross-connection sharing, not
    // merely cross-handler sharing within one connection.
    std::vector<std::unique_ptr<BridgeHandler<PollModel, AllowShared>>> handlers;
    polls::OptionId firstOptionId;
    for (std::size_t i = 0; i < 4; ++i) {
        handlers.push_back(std::make_unique<BridgeHandler<PollModel, AllowShared>>(rig.bridge(i), rig.executor()));
        const auto opened = awaitQt(handlers.back()->execute(OpenPoll{.pollId = created.pollId}));
        REQUIRE(opened.pollId == created.pollId);
        if (i == 0) {
            firstOptionId = opened.options[0].id;
        }
    }

    // All four attached to one shared instance -- instances() reports
    // exactly one live key while at least one handler holds it.
    REQUIRE(awaitQt(handlers[0]->instances()) == std::vector<std::string>{created.pollId});

    // One handler submits a vote; the other three see it on their next
    // GetPollState, proving they share one instance's state, not four
    // divergent copies.
    (void) awaitQt(handlers[0]->execute(
        SubmitVotes{.participantName = "carol", .votes = {{.optionId = firstOptionId, .choice = VoteChoice::Yes}}}));
    for (std::size_t i = 1; i < handlers.size(); ++i) {
        const auto state = awaitQt(handlers[i]->execute(GetPollState{}));
        REQUIRE(state.votes.size() == 1);
        CHECK(state.votes.front().participantName == "carol");
    }

    // Detach all four -- releasing the shared instance, which destructs.
    // ~BridgeHandler's deregister is deliberately fire-and-forget over a
    // socket (QtWebSocketBackend::deregisterModel's own doc comment: no
    // nested QEventLoop in a destructor), so this call returns before the
    // server has necessarily *processed* all four -- there is no
    // synchronous handshake to wait on here, only the directory eventually
    // reflecting the release.
    handlers.clear();

    // A fifth, fresh handler -- on its own never-before-used connection, see
    // this test's opening comment -- probes the directory: the key must be
    // gone now that every prior attacher has released it, not merely "the
    // test didn't crash". Polled, not a single snapshot: per the comment
    // above, the four deregisters above are still in flight the instant
    // handlers.clear() returns, so the first instances() reply can
    // legitimately still list the key -- pumpUntil retries the (synchronous,
    // round-tripping) instances() call until the directory catches up or the
    // deadline elapses.
    BridgeHandler<PollModel, AllowShared> prober{rig.bridge(4), rig.executor()};
    std::vector<std::string> remaining;
    REQUIRE(pumpUntil([&] {
        remaining = awaitQt(prober.instances());
        return remaining.empty();
    }));
    CHECK(remaining.empty());
}

TEST_CASE("Opening a stale pollId is NotFound through .onError(), not a crash, and a second attempt "
          "to the same bad key gets a fresh (still-failing) instance, not stale poisoned state",
          "[polls][model][shared-instances]") {
    // Per docs/spec/core/shared_instances.md's "Failure modes" section: this
    // handler's primary is set to the poisoned key on the very first
    // execute() (attachHandler records the primary before dispatch), so its
    // own second execute() re-points nowhere -- the no-op-on-same-primary
    // guard skips the backend attach round trip entirely, and the action
    // simply re-dispatches against the same (still-broken) instance. Both
    // attempts fail identically -- NotFound, via .onError(), never a crash
    // and never a silently half-hydrated success -- because
    // PollModel::execute(OpenPoll) re-runs loadPollByPollId() on every call,
    // not only the first.
    DbFixture fixture;
    BackendRig rig{Mode::Socket, 1, std::make_shared<polls::auth::PollsAuthorizer>()};
    auto handler = rig.client<PollModel>(0);

    bool firstFailed = false;
    handler.execute(OpenPoll{.pollId = "not-a-real-poll"}).onError([&firstFailed](auto) { firstFailed = true; });
    REQUIRE(pumpUntil([&firstFailed] { return firstFailed; }));

    bool secondFailed = false;
    handler.execute(OpenPoll{.pollId = "not-a-real-poll"}).onError([&secondFailed](auto) { secondFailed = true; });
    REQUIRE(pumpUntil([&secondFailed] { return secondFailed; }));

    // Both attempts are genuinely NotFound (loadPollByPollId's own message),
    // not merely "something failed" -- confirmed directly rather than only
    // inferred from the onError firing. Checked by message, not by C++
    // exception type: over Mode::Socket the server-side polls::NotFound does
    // not survive the wire -- RemoteServer's dispatchExecute catches it and
    // replies "err" with only exc.what(), and QtWebSocketBackend::onTextMessage
    // reconstructs that as a generic std::runtime_error carrying the same
    // message (morph/qt/qt_websocket_backend.cpp's execute-reply handling).
    // rung 2's own matrix test (test_bookmark_model.cpp) sidesteps this
    // entirely by only asserting a concrete exception type over Local/
    // LocalSingleThread, never Socket -- this is that same constraint made
    // explicit rather than silently avoided.
    try {
        (void) awaitQt(handler.execute(OpenPoll{.pollId = "not-a-real-poll"}));
        FAIL("expected a third attempt against the same poisoned handler to fail identically");
    } catch (const std::exception& exc) {
        CHECK(std::string{exc.what()}.find("poll not found") != std::string::npos);
    }
}

TEST_CASE("A poll's admin token does not finalize a different poll", "[polls][model][shared-instances]") {
    // PollModel is keyed per-poll (each poll is its own shared instance), so
    // this ought to be implied by the per-instance keying alone -- but a bug
    // in requireAdmin()'s poll-row lookup (poll_model.cpp: it compares
    // ctx->token against *this instance's own* `poll.adminToken` column,
    // loaded via loadPollByPollId() against whichever pollId this handler is
    // attached to) could silently let a stale/wrong cached _pollId slip
    // through. Written explicitly rather than assumed.
    DbFixture fixture;
    BackendRig rig{Mode::Socket, 2, std::make_shared<polls::auth::PollsAuthorizer>()};
    auto handlerA = rig.client<PollModel>(0);
    auto handlerB = rig.client<PollModel>(1);
    auto createdA = awaitQt(handlerA.execute(CreatePoll{.title = "A", .options = {{"1"}, {"2"}}}));
    auto createdB = awaitQt(handlerB.execute(CreatePoll{.title = "B", .options = {{"1"}, {"2"}}}));
    awaitQt(handlerB.execute(OpenPoll{.pollId = createdB.pollId}));
    auto optsB = awaitQt(handlerB.execute(GetPollState{})).options;

    morph::session::Context ctx;
    ctx.token = *createdA.adminToken;  // poll A's admin token, used against poll B
    rig.bridge(1).setDefaultSession(ctx);
    bool failed = false;
    handlerB.execute(FinalizePoll{.optionId = optsB[0].id}).onError([&failed](auto) { failed = true; });
    REQUIRE(pumpUntil([&failed] { return failed; }));
}

TEST_CASE("Bridge::setExecuteDeadline recovers a call the real rate limiter silently drops",
          "[polls][model][shared-instances]") {
    // BackendRig's Mode::Socket constructor takes an optional
    // QtWebSocketServerConfig (Task 11's own README-named
    // "Expected strain point": pastebin's own maxMessageBytes case is the
    // precedent for configuring it via the rig rather than hand-building a
    // second server) -- messagesPerSecond set here is the real per-connection
    // token bucket documented in qt_websocket_server.hpp: capacity equals
    // messagesPerSecond, one token per incoming frame of any kind, refilling
    // continuously; a frame that finds an empty bucket is dropped silently,
    // no reply of any kind (mirrors tests/qt/test_qt_websocket.cpp's own
    // "messagesPerSecond throttles a burst on one connection" construction
    // pattern -- ThreadPoolExecutor -> RemoteServer -> QtWebSocketServer with
    // a low-messagesPerSecond cfg -- except BackendRig already threads that
    // cfg straight through, so no hand-built server is needed here).
    DbFixture fixture;
    ::morph::qt::QtWebSocketServerConfig cfg;
    cfg.messagesPerSecond = 5;  // bucket capacity 5, refills at 5/s -- same
                                 // value test_qt_websocket.cpp's own
                                 // messagesPerSecond test uses.
    BackendRig rig{Mode::Socket, 1, std::make_shared<polls::auth::PollsAuthorizer>(), cfg};

    // Set the deadline before any traffic: setExecuteDeadline races every
    // executeVia() call from this point on, so a genuinely dropped setup
    // frame (unlikely at this low a burst rate, but not impossible) fails
    // fast with ClientTimeoutError instead of hanging the test up to
    // awaitQt's own 5s internal pump deadline.
    rig.bridge(0).setExecuteDeadline(std::chrono::milliseconds{500});

    auto creator = rig.client<PollModel>(0);
    const auto created =
        awaitQt(creator.execute(CreatePoll{.title = "Rate-limited poll", .options = {{"a"}, {"b"}}}));
    BridgeHandler<PollModel, AllowShared> handler{rig.bridge(0), rig.executor()};
    const auto opened = awaitQt(handler.execute(OpenPoll{.pollId = created.pollId}));

    // Burst 20 SubmitVotes calls back-to-back, no pumping/awaiting in
    // between -- mirrors test_qt_websocket.cpp's own 20-frame burst. The
    // bucket's capacity is hard-capped at 5 regardless of any refill that
    // happened during setup above (state.tokens = std::min(capacity, ...)),
    // and this loop issues all 20 sends in a single native call stack with no
    // real wall-clock time between them, so refill-during-the-burst is
    // negligible: at least 15 of these 20 frames are guaranteed to find an
    // empty bucket and be dropped at the transport, never reaching
    // RemoteServer, with no reply of any kind. Distinct participant names so
    // any call that *does* get through always succeeds -- never a business
    // -logic Conflict -- keeping "no real reply" the only way a call can end
    // up in `errors` without also being a ClientTimeoutError.
    constexpr int kBurstSize = 20;
    int successes = 0;
    int errors = 0;
    int clientTimeouts = 0;
    for (int i = 0; i < kBurstSize; ++i) {
        handler
            .execute(SubmitVotes{.participantName = "voter-" + std::to_string(i),
                                  .votes = {{.optionId = opened.options[0].id, .choice = VoteChoice::Yes}}})
            .then([&successes](polls::GetPollStateResult) { ++successes; })
            .onError([&errors, &clientTimeouts](const std::exception_ptr& err) {
                ++errors;
                try {
                    std::rethrow_exception(err);
                } catch (const morph::backend::ClientTimeoutError&) {
                    ++clientTimeouts;
                } catch (...) {
                }
            });
    }

    // Every one of the 20 completions must settle -- some via a real reply,
    // the rest recovered by the deadline -- never left hanging.
    REQUIRE(pumpUntil([&] { return successes + errors >= kBurstSize; }, std::chrono::milliseconds{3000}));
    CHECK(successes + errors == kBurstSize);

    // Proof the drop was real, not merely that the deadline fired for some
    // unrelated reason: strictly fewer real replies than calls sent (the
    // "observing more calls than replies" confirmation the brief calls for),
    // and at least one of the shortfall was specifically recovered via
    // ClientTimeoutError rather than some other error.
    CHECK(successes < kBurstSize);
    CHECK(clientTimeouts >= 1);
}
