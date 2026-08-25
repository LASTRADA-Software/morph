// SPDX-License-Identifier: Apache-2.0
//
// Task 15: this rung's framework-level deliverable. Lives alongside
// test_presenter.cpp (which tests gui/presenter.hpp from testkit/, the
// established precedent for where examples/common/gui/'s own tests live) --
// not a fresh examples/common/tests/ directory. See
// examples/common/CMakeLists.txt's ladder_common_tests target.
//
// EventPoller<EventT, EventIdT> is generic (see event_poller.hpp's own doc
// comment for why), so these tests exercise it against a small fake feed
// model of this file's own -- FeedModel/GetFeedSince/GetFeedSinceResult --
// rather than polls::PollModel/GetEventsSince, mirroring how
// test_backend_rig.cpp and test_presenter.cpp each build their own throwaway
// probe model instead of depending on a real rung's.
//
// The one piece of real Bridge machinery these tests deliberately exercise
// for real, not through a fake: Bridge::setExecuteDeadline. EventPoller's
// constructor calls it, and the "survives a ClientTimeoutError" test below
// drives a genuine BridgeHandler<FeedModel>::execute() call that never
// replies, letting the real Bridge::TimeoutScheduler resolve it with a real
// morph::backend::ClientTimeoutError -- the same mechanism (and the same
// class doc comment already pointed here) as
// examples/polls/tests/test_shared_instance_lifecycle.cpp's own
// "Bridge::setExecuteDeadline recovers a call the real rate limiter silently
// drops" test, just without standing up a rate-limited WebSocket server: a
// condition-variable-gated model call is enough to force the deadline to
// fire, deterministically and without any sleep_for (examples/TESTING.md,
// "Pumping discipline -- no sleeps").

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "gui/app_context.hpp"
#include "gui/event_poller.hpp"
#include "testkit/pump.hpp"

// Deliberately at file scope, not inside an anonymous namespace: glz's
// reflection (which BRIDGE_REGISTER_MODEL/BRIDGE_REGISTER_ACTION rely on to
// serialize these types) needs external linkage on the type -- see
// testkit/test_backend_rig.cpp's RigProbeModel for the identical precedent
// and rationale.
struct FeedEvent {
    int id = 0;
    std::string summary;
};

struct GetFeedSince {
    int lastEventId = 0;
};

struct GetFeedSinceResult {
    std::vector<FeedEvent> events;
};

/// @brief `FeedModel`'s process-wide control block -- a free-standing type
///        (not nested inside `FeedModel` itself) because a static data
///        member's in-class initializer cannot reference a nested class's
///        own in-class default member initializers before that nested
///        class's definition is complete (a real compiler restriction, not
///        a style choice -- nesting this and writing
///        `static inline Control control{};` fails to compile under clang
///        with "default member initializer ... needed within definition of
///        enclosing class").
struct FeedControl {
    std::vector<FeedEvent> events;
    std::atomic<int> callCount{0};
    std::atomic<bool> blockFirstCall{false};
    std::atomic<bool> throwNotFound{false};
    // Unlike throwNotFound (which throws immediately, before any block), this
    // throws only after a blockFirstCall wait is released -- lets a test drive
    // the *error* side of a call that was genuinely in flight, the same way
    // blockFirstCall + a plain return already drives the success side.
    std::atomic<bool> throwAfterRelease{false};
    std::mutex releaseMutex;
    std::condition_variable releaseCv;
    bool released = false;
};

/// @brief Backing model for these tests. `control` is static (process-wide)
///        rather than an instance field because this test relies on the
///        plain `BRIDGE_REGISTER_MODEL` default-construction path — the same
///        choice `morph::ladder::now()`'s `ScopedClockOverride` process-global
///        slot makes (`examples/common/clock.hpp`) — rather than adopting
///        `ModelRegistryFactory`'s per-instance construction-hook seam
///        (`include/morph/core/registry.hpp`) that would let a fresh
///        `FeedModel` instance receive its own fixture data directly. Reset
///        with `resetFeedControl()` at the top of every TEST_CASE that
///        touches it.
struct FeedModel {
    static inline FeedControl control{};

    GetFeedSinceResult execute(GetFeedSince action) {
        const int thisCall = control.callCount.fetch_add(1) + 1;
        if (control.throwNotFound.load()) {
            throw std::runtime_error{"NotFound: feed does not exist"};
        }
        if (control.blockFirstCall.load() && thisCall == 1) {
            // Blocks this worker-pool thread until the test releases it --
            // simulating a frame a rate limiter silently drops, without any
            // sleep_for. Bridge's own TimeoutScheduler (armed by
            // EventPoller's constructor via setExecuteDeadline) races this
            // independently and resolves the caller's Completion with
            // ClientTimeoutError long before this wait ever returns; the
            // test observes that via pumpUntil, then releases this wait
            // itself so ~ThreadPoolExecutor's join at teardown does not
            // hang on a permanently blocked worker.
            std::unique_lock lock{control.releaseMutex};
            control.releaseCv.wait(lock, [] { return control.released; });
            lock.unlock();
            if (control.throwAfterRelease.load()) {
                throw std::runtime_error{"boom: failed after release"};
            }
        }
        GetFeedSinceResult result;
        for (const auto& event : control.events) {
            if (event.id > action.lastEventId) {
                result.events.push_back(event);
            }
        }
        return result;
    }
};

BRIDGE_REGISTER_MODEL(FeedModel, "EventPollerTestFeedModel")
BRIDGE_REGISTER_ACTION(FeedModel, GetFeedSince, "EventPollerTestGetFeedSince")

namespace {

void resetFeedControl() {
    auto& control = FeedModel::control;
    control.events.clear();
    control.callCount.store(0);
    control.blockFirstCall.store(false);
    control.throwNotFound.store(false);
    control.throwAfterRelease.store(false);
    // Under releaseMutex, matching releaseBlockedCall()'s own write: a worker
    // thread left blocked in FeedModel::execute() by a *previous* test case
    // can still be reading this flag under the same mutex, so an unguarded
    // write here is a data race (and a ThreadSanitizer report waiting to
    // happen -- this suite is expected to run under /sanitize eventually).
    {
        const std::lock_guard lock{control.releaseMutex};
        control.released = false;
    }
}

void releaseBlockedCall() {
    {
        const std::lock_guard lock{FeedModel::control.releaseMutex};
        FeedModel::control.released = true;
    }
    FeedModel::control.releaseCv.notify_all();
}

using Poller = morph::ladder::gui::EventPoller<FeedEvent, int>;

/// @brief The production wiring's stand-in for these tests: a `Dispatch`
///        closure driving a real `BridgeHandler<FeedModel>` directly, rather
///        than a presenter's own signal-based API -- see event_poller.hpp's
///        "Why dispatch is a caller-supplied closure" doc comment for why
///        that keeps `ClientTimeoutError` a real, catchable exception type
///        here instead of a string comparison.
Poller::Dispatch makeDispatch(std::shared_ptr<morph::bridge::BridgeHandler<FeedModel>> handler) {
    return [handler](int lastEventId, Poller::OnSuccess onSuccess, Poller::OnError onError) {
        handler->execute(GetFeedSince{.lastEventId = lastEventId})
            .then([handler, lastEventId, onSuccess](GetFeedSinceResult result) {
                const int newLastEventId = result.events.empty() ? lastEventId : result.events.back().id;
                onSuccess(std::move(result.events), newLastEventId);
            })
            .onError([handler, onError](std::exception_ptr err) { onError(std::move(err)); });
    };
}

}  // namespace

TEST_CASE("EventPoller applies every event returned since the last tick and advances its cursor",
          "[gui][event-poller]") {
    resetFeedControl();
    FeedModel::control.events = {{.id = 1, .summary = "a"}, {.id = 2, .summary = "b"}, {.id = 3, .summary = "c"}};

    morph::ladder::gui::AppContext ctx{morph::ladder::gui::Local{}};
    auto handler = std::make_shared<morph::bridge::BridgeHandler<FeedModel>>(ctx.bridge(), ctx.executor());

    std::vector<int> appliedIds;
    bool fatal = false;
    // A one-hour interval never fires on its own for the duration of this
    // test -- pollOnce() below drives every tick manually and
    // deterministically (examples/TESTING.md's "Pumping discipline"; the
    // task brief's own "drive the timer manually rather than sleeping").
    Poller poller{ctx.bridge(),
                  /*startingCursor=*/0,
                  makeDispatch(handler),
                  [&](const FeedEvent& event) { appliedIds.push_back(event.id); },
                  [&](const QString&) { fatal = true; },
                  std::chrono::hours{1}};

    REQUIRE_FALSE(poller.busy());
    poller.pollOnce();
    REQUIRE(poller.busy());
    REQUIRE(morph::ladder::testkit::pumpUntil([&] { return !poller.busy(); }));
    CHECK(appliedIds == std::vector<int>{1, 2, 3});
    CHECK(poller.lastEventId() == 3);
    CHECK_FALSE(fatal);
    CHECK(poller.running());

    // A second tick with nothing new applies nothing and leaves the cursor
    // exactly where it was.
    poller.pollOnce();
    REQUIRE(morph::ladder::testkit::pumpUntil([&] { return !poller.busy(); }));
    CHECK(appliedIds == std::vector<int>{1, 2, 3});
    CHECK(poller.lastEventId() == 3);
}

TEST_CASE("EventPoller survives a ClientTimeoutError -- retries on the next tick, does not stop",
          "[gui][event-poller]") {
    resetFeedControl();
    FeedModel::control.events = {{.id = 1, .summary = "a"}};
    FeedModel::control.blockFirstCall = true;

    morph::ladder::gui::AppContext ctx{morph::ladder::gui::Local{}};
    auto handler = std::make_shared<morph::bridge::BridgeHandler<FeedModel>>(ctx.bridge(), ctx.executor());

    std::vector<int> appliedIds;
    bool fatal = false;
    // A short executeDeadline keeps this test fast; the interval stays an
    // hour so only pollOnce() drives ticks.
    Poller poller{ctx.bridge(),
                  /*startingCursor=*/0,
                  makeDispatch(handler),
                  [&](const FeedEvent& event) { appliedIds.push_back(event.id); },
                  [&](const QString&) { fatal = true; },
                  std::chrono::hours{1},
                  std::chrono::milliseconds{100}};

    poller.pollOnce();
    REQUIRE(poller.busy());
    // The dispatched call is blocked inside FeedModel::execute() on a
    // worker thread; Bridge's own TimeoutScheduler (armed by EventPoller's
    // constructor) resolves the Completion with ClientTimeoutError on its
    // own, independent of that block, once executeDeadline elapses.
    REQUIRE(morph::ladder::testkit::pumpUntil([&] { return !poller.busy(); }));
    CHECK_FALSE(fatal);
    CHECK(poller.running());  // a timeout is not fatal -- still armed
    CHECK(appliedIds.empty());
    CHECK(poller.lastEventId() == 0);  // cursor did not advance

    // Unblock the first call's worker thread now, before this test ends --
    // otherwise ~ThreadPoolExecutor (via ~AppContext) would join a thread
    // that never returns.
    releaseBlockedCall();

    // The next tick genuinely retries and this time succeeds.
    poller.pollOnce();
    REQUIRE(morph::ladder::testkit::pumpUntil([&] { return !poller.busy(); }));
    CHECK(appliedIds == std::vector<int>{1});
    CHECK(poller.lastEventId() == 1);
    CHECK_FALSE(fatal);
}

TEST_CASE("EventPoller stops and reports onFatalError exactly once on a non-timeout failure (e.g. NotFound)",
          "[gui][event-poller]") {
    resetFeedControl();
    FeedModel::control.throwNotFound = true;

    morph::ladder::gui::AppContext ctx{morph::ladder::gui::Local{}};
    auto handler = std::make_shared<morph::bridge::BridgeHandler<FeedModel>>(ctx.bridge(), ctx.executor());

    int fatalCount = 0;
    QString lastMessage;
    Poller poller{ctx.bridge(),
                  /*startingCursor=*/0,
                  makeDispatch(handler),
                  [](const FeedEvent&) { FAIL("onEvent must not run when the dispatch itself failed"); },
                  [&](const QString& message) {
                      ++fatalCount;
                      lastMessage = message;
                  },
                  std::chrono::hours{1}};

    poller.pollOnce();
    REQUIRE(morph::ladder::testkit::pumpUntil([&] { return !poller.busy(); }));
    CHECK(fatalCount == 1);
    CHECK(poller.fatalErrorReported());
    CHECK_FALSE(poller.running());
    CHECK(lastMessage.toStdString().find("NotFound") != std::string::npos);

    // A further tick -- manual here, but equally a real timer tick, if the
    // timer were still armed -- must not dispatch again and must not report
    // onFatalError a second time: pollOnce() itself refuses once _fatal is
    // set, and the timer is already stopped.
    poller.pollOnce();
    CHECK_FALSE(poller.busy());
    CHECK(fatalCount == 1);
}

TEST_CASE("EventPoller destroyed with a tick in flight suppresses the orphaned completion callback",
          "[gui][event-poller]") {
    // Regression test for the use-after-free EventPoller::_liveness fixes.
    //
    // The callbacks pollOnce() hands to Dispatch are delivered through
    // QtExecutor::post -> QMetaObject::invokeMethod(..., Qt::QueuedConnection),
    // so a pending one is an event owned by QCoreApplication -- NOT a
    // connection owned by the poller's own _timer. Destroying the poller
    // (the ordinary case of a user closing a poll view mid-tick) cancels
    // nothing, and without the _liveness weak_ptr guard that queued callback
    // fires into freed memory. Verified to catch the regression: with the
    // two `alive.expired()` checks removed this test reports the applied
    // event (and, under ASan, a heap-use-after-free).
    resetFeedControl();
    FeedModel::control.events = {{.id = 1, .summary = "a"}};
    FeedModel::control.blockFirstCall = true;

    morph::ladder::gui::AppContext ctx{morph::ladder::gui::Local{}};
    auto handler = std::make_shared<morph::bridge::BridgeHandler<FeedModel>>(ctx.bridge(), ctx.executor());

    // Both deliberately outlive the poller. `applied` is what a surviving
    // (i.e. unsuppressed) callback would set; `completionDelivered` proves
    // the completion genuinely did resolve after the poller died -- without
    // it this test could "pass" by simply never delivering anything at all.
    auto applied = std::make_shared<std::atomic<bool>>(false);
    auto completionDelivered = std::make_shared<std::atomic<bool>>(false);

    Poller::Dispatch dispatch = [handler, completionDelivered](int lastEventId, Poller::OnSuccess onSuccess,
                                                               Poller::OnError onError) {
        handler->execute(GetFeedSince{.lastEventId = lastEventId})
            .then([handler, lastEventId, onSuccess, completionDelivered](GetFeedSinceResult result) {
                completionDelivered->store(true);
                const int newLastEventId = result.events.empty() ? lastEventId : result.events.back().id;
                onSuccess(std::move(result.events), newLastEventId);
            })
            .onError([handler, onError, completionDelivered](std::exception_ptr err) {
                completionDelivered->store(true);
                onError(std::move(err));
            });
    };

    // An hour-long executeDeadline as well as an hour-long interval: neither
    // the timer nor Bridge's TimeoutScheduler may resolve this tick on its
    // own -- the test controls exactly when the dispatch completes.
    auto poller = std::make_unique<Poller>(
        ctx.bridge(), /*startingCursor=*/0, dispatch, [applied](const FeedEvent&) { applied->store(true); },
        [](const QString&) { FAIL("onFatalError must not run after the poller is destroyed"); }, std::chrono::hours{1},
        std::chrono::hours{1});

    poller->pollOnce();
    REQUIRE(poller->busy());

    // Destroy while the dispatch is genuinely outstanding: the worker thread
    // is still parked inside FeedModel::execute().
    poller.reset();

    // Now let the model call return. The Completion resolves and posts the
    // now-orphaned success callback as a queued Qt event.
    releaseBlockedCall();
    REQUIRE(morph::ladder::testkit::pumpUntil([&] { return completionDelivered->load(); }));
    // Keep pumping a while longer so any straggler queued event definitely
    // gets its turn (never-true predicate == "pump for this long").
    static_cast<void>(morph::ladder::testkit::pumpUntil([] { return false; }, std::chrono::milliseconds{50}));

    CHECK_FALSE(applied->load());
}

TEST_CASE("EventPoller advances its cursor before applying events and stays busy across the batch",
          "[gui][event-poller]") {
    // Regression test for the success-callback ordering fix. Previously
    // _requestInFlight was cleared *before* the onEvent fan-out and
    // _lastEventId advanced *after* it, so for the whole duration of the
    // caller's callbacks busy() already read false (a reentrant pollOnce()
    // -- e.g. from a modal dialog spinning a nested Qt event loop -- was not
    // blocked) while the cursor still held its pre-batch value (so that
    // reentrant tick refetched and reapplied the same events).
    resetFeedControl();
    FeedModel::control.events = {{.id = 1, .summary = "a"}, {.id = 2, .summary = "b"}};

    morph::ladder::gui::AppContext ctx{morph::ladder::gui::Local{}};
    auto handler = std::make_shared<morph::bridge::BridgeHandler<FeedModel>>(ctx.bridge(), ctx.executor());

    Poller* pollerPtr = nullptr;
    std::vector<int> cursorInsideOnEvent;
    std::vector<bool> busyInsideOnEvent;
    std::vector<int> appliedIds;

    Poller poller{ctx.bridge(),
                  /*startingCursor=*/0,
                  makeDispatch(handler),
                  [&](const FeedEvent& event) {
                      appliedIds.push_back(event.id);
                      cursorInsideOnEvent.push_back(pollerPtr->lastEventId());
                      busyInsideOnEvent.push_back(pollerPtr->busy());
                      // Simulated reentrancy: a nested event loop ticking the
                      // poller again from inside an event handler. Must be
                      // refused outright (see callCount below).
                      pollerPtr->pollOnce();
                  },
                  [](const QString&) { FAIL("no fatal error expected"); },
                  std::chrono::hours{1}};
    pollerPtr = &poller;

    poller.pollOnce();
    REQUIRE(morph::ladder::testkit::pumpUntil([&] { return !poller.busy(); }));

    CHECK(appliedIds == std::vector<int>{1, 2});
    // The cursor is already at its post-batch value for *every* onEvent call,
    // including the first -- not still 0.
    CHECK(cursorInsideOnEvent == std::vector<int>{2, 2});
    // And the poller still reports itself busy throughout, so the reentrant
    // pollOnce() calls above were no-ops...
    CHECK(busyInsideOnEvent == std::vector<bool>{true, true});
    // ...which the model's own call counter confirms: exactly one dispatch
    // reached the backend, not three.
    CHECK(FeedModel::control.callCount.load() == 1);
    CHECK(poller.lastEventId() == 2);
    CHECK_FALSE(poller.busy());
}

TEST_CASE("EventPoller clears its in-flight flag even when onEvent throws", "[gui][event-poller]") {
    // The RAII half of the ordering fix: _requestInFlight is released by a
    // scope guard, not a plain assignment, so a throwing onEvent cannot wedge
    // busy() at true forever (the same hazard gui/presenter.hpp's
    // Presenter::track() guards against).
    resetFeedControl();
    FeedModel::control.events = {{.id = 1, .summary = "a"}};

    morph::ladder::gui::AppContext ctx{morph::ladder::gui::Local{}};
    auto handler = std::make_shared<morph::bridge::BridgeHandler<FeedModel>>(ctx.bridge(), ctx.executor());

    bool onEventThrew = false;
    // A Dispatch that contains the throw rather than letting it escape into
    // the executor (where QtExecutor would let it reach the Qt event loop and
    // std::terminate) -- the point under test is EventPoller's own state
    // after the throw, not the executor's throwing-callback policy.
    Poller::Dispatch dispatch = [handler, &onEventThrew](int lastEventId, Poller::OnSuccess onSuccess,
                                                         Poller::OnError onError) {
        handler->execute(GetFeedSince{.lastEventId = lastEventId})
            .then([handler, lastEventId, onSuccess, &onEventThrew](GetFeedSinceResult result) {
                const int newLastEventId = result.events.empty() ? lastEventId : result.events.back().id;
                try {
                    onSuccess(std::move(result.events), newLastEventId);
                } catch (const std::runtime_error&) {
                    onEventThrew = true;
                }
            })
            .onError([handler, onError](std::exception_ptr err) { onError(std::move(err)); });
    };

    Poller poller{ctx.bridge(),
                  /*startingCursor=*/0,
                  dispatch,
                  [](const FeedEvent&) -> void { throw std::runtime_error{"onEvent blew up"}; },
                  [](const QString&) { FAIL("no fatal error expected"); },
                  std::chrono::hours{1}};

    poller.pollOnce();
    REQUIRE(morph::ladder::testkit::pumpUntil([&] { return onEventThrew; }));
    CHECK_FALSE(poller.busy());
    // The cursor still advanced -- it is written before the fan-out, so a
    // throwing onEvent does not condemn the poller to redelivering the same
    // batch on every subsequent tick.
    CHECK(poller.lastEventId() == 1);
    // ...and the poller genuinely accepts another tick.
    poller.pollOnce();
    CHECK(poller.busy());
    REQUIRE(morph::ladder::testkit::pumpUntil([&] { return !poller.busy(); }));
}

TEST_CASE("EventPoller::start rearms the timer, but refuses once a fatal error has stopped it for good",
          "[gui][event-poller]") {
    // start()/stop() are the pause/resume pair a hidden poll view uses
    // (event_poller.hpp's own doc comments on both) -- distinct from
    // pollOnce() (drives one tick regardless of the timer) and from
    // resume() (also clears _fatal). Nothing else in this file calls either
    // one directly: every other test lets the constructor's own
    // `_timer.start(interval)` and handleError's own `_timer.stop()` be the
    // only callers, so start()'s `if (!_fatal)` guard -- the one branch
    // start() has -- has never actually run either arm from here.
    resetFeedControl();
    FeedModel::control.events = {{.id = 1, .summary = "a"}};

    morph::ladder::gui::AppContext ctx{morph::ladder::gui::Local{}};
    auto handler = std::make_shared<morph::bridge::BridgeHandler<FeedModel>>(ctx.bridge(), ctx.executor());

    int fatalCount = 0;
    Poller poller{
        ctx.bridge(),
        /*startingCursor=*/0, makeDispatch(handler), [](const FeedEvent&) {}, [&](const QString&) { ++fatalCount; },
        std::chrono::hours{1}};

    REQUIRE(poller.running());
    poller.stop();
    REQUIRE_FALSE(poller.running());

    // Not fatal: start() rearms it, same as a poll view coming back into view.
    poller.start();
    CHECK(poller.running());

    // Now drive a genuinely fatal failure, stopping the timer for good.
    poller.stop();
    FeedModel::control.throwNotFound = true;
    poller.pollOnce();
    REQUIRE(morph::ladder::testkit::pumpUntil([&] { return !poller.busy(); }));
    REQUIRE(fatalCount == 1);
    REQUIRE(poller.fatalErrorReported());
    REQUIRE_FALSE(poller.running());

    // start()'s own guard refuses to rearm a fatally-stopped poller -- only
    // resume() can undo that (see the dedicated resume() test below).
    poller.start();
    CHECK_FALSE(poller.running());
}

TEST_CASE("EventPoller::resume clears a fatal error and polls again from a new cursor", "[gui][event-poller]") {
    resetFeedControl();
    FeedModel::control.throwNotFound = true;

    morph::ladder::gui::AppContext ctx{morph::ladder::gui::Local{}};
    auto handler = std::make_shared<morph::bridge::BridgeHandler<FeedModel>>(ctx.bridge(), ctx.executor());

    int fatalCount = 0;
    std::vector<int> appliedIds;
    Poller poller{ctx.bridge(),
                  /*startingCursor=*/0,
                  makeDispatch(handler),
                  [&](const FeedEvent& event) { appliedIds.push_back(event.id); },
                  [&](const QString&) { ++fatalCount; },
                  std::chrono::hours{1}};

    poller.pollOnce();
    REQUIRE(morph::ladder::testkit::pumpUntil([&] { return !poller.busy(); }));
    REQUIRE(fatalCount == 1);
    REQUIRE(poller.fatalErrorReported());
    REQUIRE_FALSE(poller.running());
    // Without resume(), this is terminal: pollOnce() refuses forever.
    poller.pollOnce();
    REQUIRE_FALSE(poller.busy());

    // The GUI's recovery: a full GetPollState-shaped resync hands back a
    // fresh cursor, and polling continues incrementally from there.
    FeedModel::control.throwNotFound = false;
    FeedModel::control.events = {{.id = 1, .summary = "a"}, {.id = 2, .summary = "b"}, {.id = 3, .summary = "c"}};
    poller.resume(2);

    CHECK_FALSE(poller.fatalErrorReported());
    CHECK(poller.lastEventId() == 2);
    CHECK(poller.running());

    // And a tick genuinely dispatches again rather than silently refusing.
    poller.pollOnce();
    REQUIRE(poller.busy());
    REQUIRE(morph::ladder::testkit::pumpUntil([&] { return !poller.busy(); }));
    CHECK(appliedIds == std::vector<int>{3});  // only what is after the new cursor
    CHECK(poller.lastEventId() == 3);
    CHECK(fatalCount == 1);
}

TEST_CASE("EventPoller destroyed with a tick in flight suppresses the orphaned error callback",
          "[gui][event-poller]") {
    // The onError sibling of "EventPoller destroyed with a tick in flight
    // suppresses the orphaned completion callback" above: that test only ever
    // drives the onSuccess lambda's `alive.expired()` check (pollOnce()'s
    // .then() branch), never the onError lambda's own, separate
    // `alive.expired()` check (event_poller.hpp's onError callback) --  a
    // dispatch that is still outstanding when the poller is destroyed, and
    // that goes on to *fail* rather than succeed, is a different code path
    // and was never exercised. throwAfterRelease makes FeedModel::execute()
    // throw only once the blockFirstCall wait is released, so the failure
    // happens strictly after the poller is gone, exactly like the
    // success-side regression test.
    resetFeedControl();
    FeedModel::control.events = {{.id = 1, .summary = "a"}};
    FeedModel::control.blockFirstCall = true;
    FeedModel::control.throwAfterRelease = true;

    morph::ladder::gui::AppContext ctx{morph::ladder::gui::Local{}};
    auto handler = std::make_shared<morph::bridge::BridgeHandler<FeedModel>>(ctx.bridge(), ctx.executor());

    // Both deliberately outlive the poller, matching the success-side test's
    // own rationale for the same two flags.
    auto errorObserved = std::make_shared<std::atomic<bool>>(false);
    auto completionDelivered = std::make_shared<std::atomic<bool>>(false);

    Poller::Dispatch dispatch = [handler, completionDelivered](int lastEventId, Poller::OnSuccess onSuccess,
                                                               Poller::OnError onError) {
        handler->execute(GetFeedSince{.lastEventId = lastEventId})
            .then([handler, lastEventId, onSuccess, completionDelivered](GetFeedSinceResult result) {
                completionDelivered->store(true);
                const int newLastEventId = result.events.empty() ? lastEventId : result.events.back().id;
                onSuccess(std::move(result.events), newLastEventId);
            })
            .onError([handler, onError, completionDelivered](std::exception_ptr err) {
                completionDelivered->store(true);
                onError(std::move(err));
            });
    };

    // Hour-long interval and executeDeadline: only this test's own
    // release/throw sequence resolves the dispatch, never the timer or
    // Bridge's own TimeoutScheduler.
    auto poller = std::make_unique<Poller>(
        ctx.bridge(), /*startingCursor=*/0, dispatch, [](const FeedEvent&) { FAIL("onEvent must not run"); },
        [errorObserved](const QString&) { errorObserved->store(true); }, std::chrono::hours{1}, std::chrono::hours{1});

    poller->pollOnce();
    REQUIRE(poller->busy());

    // Destroy while the dispatch is genuinely outstanding: the worker thread
    // is still parked inside FeedModel::execute().
    poller.reset();

    // Now let the model call return -- it throws, so the Completion resolves
    // via onError and posts the now-orphaned error callback as a queued Qt
    // event.
    releaseBlockedCall();
    REQUIRE(morph::ladder::testkit::pumpUntil([&] { return completionDelivered->load(); }));
    // Keep pumping a while longer so any straggler queued event definitely
    // gets its turn (never-true predicate == "pump for this long").
    static_cast<void>(morph::ladder::testkit::pumpUntil([] { return false; }, std::chrono::milliseconds{50}));

    CHECK_FALSE(errorObserved->load());
}

TEST_CASE("EventPoller::handleError ignores a second failure once already fatal", "[gui][event-poller]") {
    // handleError's own `if (_fatal) return;` guard (event_poller.hpp) is
    // documented as protecting against a Dispatch that violates its "call
    // exactly one of onSuccess/onError, exactly once" contract -- e.g. one
    // wired to a signal that fires twice. pollOnce() itself cannot exercise
    // this: it refuses to start a *new* tick once _fatal is set, so a
    // Dispatch that behaves correctly never reaches handleError a second
    // time. This test drives a Dispatch that double-reports directly on a
    // single pollOnce() call, bypassing that guard, so `_fatal` is genuinely
    // already true the second time handleError runs.
    resetFeedControl();

    morph::ladder::gui::AppContext ctx{morph::ladder::gui::Local{}};

    int fatalCount = 0;
    QString firstMessage;
    // Reports failure twice from the same dispatch call, synchronously --
    // simulating the double-report hazard the class doc comment describes.
    Poller::Dispatch dispatch = [](int /*lastEventId*/, Poller::OnSuccess /*onSuccess*/, Poller::OnError onError) {
        onError(std::make_exception_ptr(std::runtime_error{"first failure"}));
        onError(std::make_exception_ptr(std::runtime_error{"second failure, must be ignored"}));
    };

    Poller poller{ctx.bridge(),
                  /*startingCursor=*/0,
                  dispatch,
                  [](const FeedEvent&) { FAIL("onEvent must not run"); },
                  [&](const QString& message) {
                      ++fatalCount;
                      if (fatalCount == 1) {
                          firstMessage = message;
                      }
                  },
                  std::chrono::hours{1}};

    poller.pollOnce();

    // Both onError calls above run synchronously inside pollOnce() (the
    // dispatch never defers), so no pump is needed: by the time pollOnce()
    // returns, handleError has already run twice.
    CHECK(fatalCount == 1);
    CHECK(poller.fatalErrorReported());
    CHECK(firstMessage.toStdString().find("first failure") != std::string::npos);
}

TEST_CASE("EventPoller tolerates an empty onFatalError callback on a non-timeout failure", "[gui][event-poller]") {
    // handleError's `if (_onFatalError)` guard (event_poller.hpp) exists
    // because OnFatalError is a plain std::function a caller supplies --
    // nothing requires it be non-empty. Every other test in this file passes
    // a real lambda; this is the one that passes a default-constructed
    // (falsy) std::function, so the guard's false arm -- "there is nothing to
    // call" -- is genuinely exercised instead of merely assumed safe.
    resetFeedControl();
    FeedModel::control.throwNotFound = true;

    morph::ladder::gui::AppContext ctx{morph::ladder::gui::Local{}};
    auto handler = std::make_shared<morph::bridge::BridgeHandler<FeedModel>>(ctx.bridge(), ctx.executor());

    Poller poller{
        ctx.bridge(),           /*startingCursor=*/0,
        makeDispatch(handler),  [](const FeedEvent&) { FAIL("onEvent must not run when the dispatch itself failed"); },
        Poller::OnFatalError{},  // deliberately empty
        std::chrono::hours{1}};

    poller.pollOnce();
    // Must not crash calling an empty std::function, and must still record
    // the fatal state and stop the timer exactly as with a real callback.
    REQUIRE(morph::ladder::testkit::pumpUntil([&] { return !poller.busy(); }));
    CHECK(poller.fatalErrorReported());
    CHECK_FALSE(poller.running());
}
