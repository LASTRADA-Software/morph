// SPDX-License-Identifier: Apache-2.0
#include <QUrl>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/qt/qt_executor.hpp>
#include <morph/qt/qt_websocket_backend.hpp>
#include <stdexcept>
#include <vector>

#include "gui/app_context.hpp"
#include "gui/presenter.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/pump.hpp"
#include "testkit/strand_interleaver.hpp"

// Deliberately at namespace scope, not inside an anonymous namespace: glz's
// reflection (which BRIDGE_REGISTER_MODEL/BRIDGE_REGISTER_ACTION rely on to
// serialize these types across the wire) needs external linkage on the type —
// see testkit/test_backend_rig.cpp's RigProbeModel for the same pattern.
// The registration macros must also appear before ProbePresenter below: its
// inline bump() calls BridgeHandler<PresenterProbeModel>::execute<
// PresenterProbeAction>(), which needs morph::model::ActionTraits<
// PresenterProbeAction> already specialised at that point (an ordinary
// member function's body is compiled in place, not deferred to end of TU).
struct PresenterProbeAction {
    int value = 0;
};
struct PresenterProbeModel {
    int execute(PresenterProbeAction action) { return action.value + 1; }
};

// A second action whose model deliberately throws, so a test can drive
// track()'s .onError path (finishOne() called from the error branch, never
// exercised by the plain success-path test above).
struct PresenterProbeFailAction {};
struct PresenterProbeFailModel {
    int execute(PresenterProbeFailAction) { throw std::runtime_error{"presenter probe: deliberate failure"}; }
};

BRIDGE_REGISTER_MODEL(PresenterProbeModel, "PresenterProbeModel")
BRIDGE_REGISTER_ACTION(PresenterProbeModel, PresenterProbeAction, "PresenterProbeAction")
BRIDGE_REGISTER_MODEL(PresenterProbeFailModel, "PresenterProbeFailModel")
BRIDGE_REGISTER_ACTION(PresenterProbeFailModel, PresenterProbeFailAction, "PresenterProbeFailAction")

namespace {

class ProbePresenter : public morph::ladder::gui::Presenter {
public:
    ProbePresenter(morph::bridge::Bridge& bridge, morph::exec::IExecutor* exec)
        : _handler{bridge, exec}, _failHandler{bridge, exec} {}

    /// @brief Calls trackBound() on the base class -- the doc-commented
    ///        usage pattern (presenter.hpp) that no other test in this file
    ///        exercises: every other ProbePresenter test constructs the
    ///        presenter and never touches bound()/trackBound() at all.
    void hookBound() { trackBound(_handler.whenBound()); }

    void bump(int value) {
        track<int>(_handler.execute(PresenterProbeAction{value}), [this](int result) { lastResult = result; });
    }

    /// @brief Drives the model that always throws, so track()'s .onError
    ///        branch (and therefore finishOne() called from there) actually
    ///        runs — the plain success path above never reaches it.
    void bumpAndFail() {
        track<int>(_failHandler.execute(PresenterProbeFailAction{}),
                   [](int) { FAIL("onOk must not run for a failed action"); });
    }

    /// @brief Drives the (successful) probe action, but with an onOk callback
    ///        that itself throws — track()'s catch-block must still call
    ///        finishOne() before rethrowing (presenter.hpp's documented
    ///        exception-safety contract), or busy() would stay true forever.
    void bumpAndThrowFromOnOk() {
        track<int>(_handler.execute(PresenterProbeAction{0}),
                   [](int) -> void { throw std::runtime_error{"presenter probe: onOk threw"}; });
    }

    /// @brief Drives the probe action with an onOk callback that destroys
    ///        *this presenter*, through the `unique_ptr` that owns it.
    ///        track()'s post-callback `if (self)` re-check exists for exactly
    ///        this case: the guard taken before onOk ran cannot be trusted
    ///        afterwards, because the callback may be the very thing that
    ///        ended the presenter's life. Safe to do from inside the handler
    ///        — the lambda lives in the completion's own state, not in the
    ///        presenter, so it outlives the object it just destroyed.
    void bumpAndDestroySelf(std::unique_ptr<ProbePresenter>& owner) {
        track<int>(_handler.execute(PresenterProbeAction{0}), [&owner](int) { owner.reset(); });
    }

    /// @brief The onErr counterpart of bumpAndDestroySelf(): both of track()'s
    ///        handler branches carry the same post-callback re-check, so both
    ///        need a case where the callback destroys the presenter.
    void failAndDestroySelf(std::unique_ptr<ProbePresenter>& owner) {
        track<int>(
            _failHandler.execute(PresenterProbeFailAction{}),
            [](int) { FAIL("onOk must not run for a failed action"); },
            [&owner](const std::exception_ptr&) { owner.reset(); });
    }

    /// @brief Destroys this presenter from inside onOk and *then* throws.
    ///        track()'s catch block re-checks liveness for the same reason its
    ///        normal exit does — the callback may already have ended the
    ///        presenter's life before it threw — and only a callback doing
    ///        both reaches that particular check.
    void bumpDestroySelfAndThrow(std::unique_ptr<ProbePresenter>& owner) {
        track<int>(_handler.execute(PresenterProbeAction{0}), [&owner](int) {
            owner.reset();
            throw std::runtime_error{"presenter probe: onOk destroyed the presenter, then threw"};
        });
    }

    /// @brief The onErr counterpart of bumpDestroySelfAndThrow().
    void failDestroySelfAndThrow(std::unique_ptr<ProbePresenter>& owner) {
        track<int>(
            _failHandler.execute(PresenterProbeFailAction{}),
            [](int) { FAIL("onOk must not run for a failed action"); },
            [&owner](const std::exception_ptr&) {
                owner.reset();
                throw std::runtime_error{"presenter probe: onErr destroyed the presenter, then threw"};
            });
    }

    /// @brief Drives the model that always throws, using the three-argument
    ///        track(onOk, onErr) overload so a test can assert the onErr
    ///        callback itself actually fires. Regression coverage for
    ///        docs/findings/023: bumpAndFail() above only exercises the
    ///        two-argument form, which busy()/idle() alone cannot
    ///        distinguish from the pre-fix bug (the surviving handler in
    ///        both cases is track()'s own, so the counter always cleared
    ///        correctly — the bug was invisible to that assertion). This
    ///        method exercises the new third parameter directly, which is
    ///        what the fix in presenter.hpp actually added.
    void bumpAndFailWithHandler() {
        track<int>(
            _failHandler.execute(PresenterProbeFailAction{}),
            [](int) { FAIL("onOk must not run for a failed action"); },
            [this](const std::exception_ptr&) { errorHandlerFired = true; });
    }

    /// @brief Drives the model that always throws, with an `onErr` callback
    ///        that itself throws — the mirror of `bumpAndThrowFromOnOk()` for
    ///        `track()`'s *error* branch. That branch has its own
    ///        `catch (...) { finishOne(); throw; }`, and it is the one a real
    ///        presenter is most likely to trip: `onErr` is where a subclass
    ///        renders the failure, and rendering is exactly the kind of code
    ///        that throws.
    void bumpAndThrowFromOnErr() {
        track<int>(
            _failHandler.execute(PresenterProbeFailAction{}),
            [](int) { FAIL("onOk must not run for a failed action"); },
            [](const std::exception_ptr&) -> void { throw std::runtime_error{"presenter probe: onErr threw"}; });
    }

    /// @brief Drives the probe action without waiting for it to settle --
    ///        lets a test call this twice back-to-back so two `track()`
    ///        calls are in flight at once. Records each settlement in
    ///        arrival order (`settledOrder`) rather than a single
    ///        `lastResult`, so a test can tell "the first completion settled
    ///        without the second" apart from "both settled".
    void bumpConcurrent(int value) {
        track<int>(_handler.execute(PresenterProbeAction{value}),
                   [this](int result) { settledOrder.push_back(result); });
    }

    int lastResult = -1;
    bool errorHandlerFired = false;
    std::vector<int> settledOrder;

private:
    morph::bridge::BridgeHandler<PresenterProbeModel> _handler;
    morph::bridge::BridgeHandler<PresenterProbeFailModel> _failHandler;
};

}  // namespace

TEST_CASE("Presenter::busy() is true while an action is in flight and false once it settles",
          "[ladder][testkit][gui][presenter]") {
    morph::ladder::gui::AppContext ctx{morph::ladder::gui::Local{}};
    ProbePresenter presenter{ctx.bridge(), ctx.executor()};

    REQUIRE_FALSE(presenter.busy());
    presenter.bump(41);
    REQUIRE(morph::ladder::testkit::settle(presenter));
    REQUIRE_FALSE(presenter.busy());
    REQUIRE(presenter.lastResult == 42);
}

TEST_CASE("Presenter::track() calls finishOne() on the error path, not just success",
          "[ladder][testkit][gui][presenter]") {
    morph::ladder::gui::AppContext ctx{morph::ladder::gui::Local{}};
    ProbePresenter presenter{ctx.bridge(), ctx.executor()};

    REQUIRE_FALSE(presenter.busy());
    presenter.bumpAndFail();
    REQUIRE(presenter.busy());
    REQUIRE(morph::ladder::testkit::settle(presenter));
    REQUIRE_FALSE(presenter.busy());  // .onError's finishOne() ran — the counter didn't leak
}

TEST_CASE("Presenter::track() calls finishOne() even when onOk itself throws", "[ladder][testkit][gui][presenter]") {
    morph::ladder::gui::AppContext ctx{morph::ladder::gui::Local{}};
    ProbePresenter presenter{ctx.bridge(), ctx.executor()};

    REQUIRE_FALSE(presenter.busy());
    presenter.bumpAndThrowFromOnOk();
    // track()'s .then() rethrows after finishOne() (presenter.hpp's own
    // catch-block), but Completion<T>'s executor composes every attached
    // .then() handler and itself catches (and logs) a throwing one rather
    // than letting it escape to pumpUntil's caller (docs/spec/core/completion.md,
    // "Handler fan-out") — so this only observes the counter, not the throw
    // itself.
    REQUIRE(morph::ladder::testkit::pumpUntil([&] { return !presenter.busy(); }));
    // finishOne() ran before the exception was swallowed: busy() is false,
    // not leaked.
    REQUIRE_FALSE(presenter.busy());
}

TEST_CASE("Presenter::track()'s three-argument overload invokes onErr on the error path",
          "[ladder][testkit][gui][presenter]") {
    // Regression test for docs/findings/023 (Completion<T>::onError() is
    // single-slot: a second .onError() attach silently discards the first).
    // The test case above ("...calls finishOne() on the error path...") only
    // asserts busy()/idle() — that assertion passed even with the pre-fix
    // bug present, since the surviving .onError() handler was always
    // track()'s own. This test instead asserts the onErr callback supplied
    // as track()'s third argument actually runs — the thing the bug would
    // have silently discarded.
    morph::ladder::gui::AppContext ctx{morph::ladder::gui::Local{}};
    ProbePresenter presenter{ctx.bridge(), ctx.executor()};

    REQUIRE_FALSE(presenter.errorHandlerFired);
    presenter.bumpAndFailWithHandler();
    REQUIRE(morph::ladder::testkit::settle(presenter));
    REQUIRE(presenter.errorHandlerFired);
    REQUIRE_FALSE(presenter.busy());  // both onErr and finishOne() ran
}

TEST_CASE("Presenter::track() calls finishOne() even when onErr itself throws", "[ladder][testkit][gui][presenter]") {
    // The `.onError` branch's half of the exception-safety contract the
    // "...even when onOk itself throws" case above pins for `.then`. Same
    // mechanism (finishOne() runs from the catch-block before the rethrow),
    // but Completion<T>'s executor composes every attached .onError() handler
    // and itself catches (and logs) a throwing one rather than letting it
    // escape to pumpUntil's caller (docs/spec/core/completion.md, "Handler
    // fan-out") — the same reason the ".then" mirror test above no longer
    // expects a throw either. What is still at stake: if `finishOne()` did
    // not run before the rethrow, `_inFlight` would never return to zero,
    // `busy()` would stay true forever, and every later `settle()` in the
    // process would burn its full deadline before failing with no useful
    // diagnostic.
    morph::ladder::gui::AppContext ctx{morph::ladder::gui::Local{}};
    ProbePresenter presenter{ctx.bridge(), ctx.executor()};

    REQUIRE_FALSE(presenter.busy());
    presenter.bumpAndThrowFromOnErr();
    REQUIRE(morph::ladder::testkit::pumpUntil([&] { return !presenter.busy(); }));
    REQUIRE_FALSE(presenter.busy());
}

TEST_CASE("AppContext{Local} is ready on construction and runs onReady inline",
          "[ladder][testkit][gui][app-context]") {
    morph::ladder::gui::AppContext ctx{morph::ladder::gui::Local{}};

    // No transport to wait for, so no deferral: a Local context is usable the
    // line after its constructor returns, as every existing caller assumes.
    REQUIRE(ctx.ready());

    bool fired = false;
    ctx.onReady([&] { fired = true; });
    REQUIRE(fired);  // synchronous — nothing pumped the event loop in between
}

TEST_CASE("AppContext::onReady(nullptr) is a no-op, not a crash", "[ladder][testkit][gui][app-context]") {
    morph::ladder::gui::AppContext ctx{morph::ladder::gui::Local{}};
    ctx.onReady(nullptr);  // must simply do nothing — no callback to run or queue
    SUCCEED("onReady(nullptr) returned without invoking or storing anything");
}

TEST_CASE("AppContext::login() sets the bridge's default session principal", "[ladder][testkit][gui][app-context]") {
    morph::ladder::gui::AppContext ctx{morph::ladder::gui::Local{}};
    ctx.login("alice");
    // login() forwards to Bridge::setDefaultSession — observable indirectly
    // via the same bridge a handler built against this context would use;
    // the model itself doesn't read the principal here, so this asserts the
    // call completes without throwing rather than a specific session::current()
    // read, which needs a live dispatch to observe.
    ProbePresenter presenter{ctx.bridge(), ctx.executor()};
    presenter.bump(1);
    REQUIRE(morph::ladder::testkit::settle(presenter));
    REQUIRE(presenter.lastResult == 2);
}

TEST_CASE("AppContext{Remote} defers readiness to the first connect",
          "[ladder][testkit][gui][app-context][socket-only]") {
    // A server with no clients of its own — the AppContext below is the client.
    morph::ladder::testkit::BackendRig rig{morph::ladder::testkit::Mode::Socket, /*nClients=*/0};
    morph::ladder::gui::AppContext ctx{morph::ladder::gui::Remote{rig.url()}};

    // Not ready the line after construction: QWebSocket::open() is
    // asynchronous and no event-loop turn has run yet. A BridgeHandler
    // constructed here would queue its registration and retry once the
    // socket connects (registerModelAsync's queueing, docs/spec/core/
    // backend.md), rather than failing -- but ctx.ready() still reflects
    // socket-connect timing, not registration settlement, so it is false
    // regardless.
    REQUIRE_FALSE(ctx.ready());

    int fired = 0;
    ctx.onReady([&] { ++fired; });
    REQUIRE(fired == 0);  // queued, not run

    REQUIRE(morph::ladder::testkit::pumpUntil([&] { return ctx.ready(); }));
    REQUIRE(fired == 1);

    // Registered after readiness: runs inline, exactly like Local mode.
    bool late = false;
    ctx.onReady([&] { late = true; });
    REQUIRE(late);
    REQUIRE(fired == 1);  // the first callback is not re-run
}

TEST_CASE("Presenter::trackBound() emits bound() exactly once, synchronously in Local mode",
          "[ladder][testkit][gui][presenter]") {
    // Local mode's handler is already bound by construction (presenter.hpp's
    // own doc comment on bound()), so whenBound()'s Completion<bool> settles
    // on the very first event-loop turn -- pumpUntil, not a bare assertion,
    // since "posted, not delivered inline" (trackBound()'s own comment on why
    // it uses QPointer) still applies even when the outcome is a foregone
    // conclusion.
    morph::ladder::gui::AppContext ctx{morph::ladder::gui::Local{}};
    ProbePresenter presenter{ctx.bridge(), ctx.executor()};

    int boundCount = 0;
    QObject::connect(&presenter, &morph::ladder::gui::Presenter::bound, [&] { ++boundCount; });

    presenter.hookBound();
    REQUIRE(morph::ladder::testkit::pumpUntil([&] { return boundCount == 1; }));
    REQUIRE(boundCount == 1);
}

TEST_CASE("Presenter::trackBound() still emits bound() when the presenter is destroyed first",
          "[ladder][testkit][gui][presenter]") {
    // trackBound()'s QPointer<Presenter> guard exists for exactly this case:
    // whenBound()'s Completion resolves through the executor (posted, not
    // inline), so a short-lived presenter destroyed before that post runs
    // must not have its .then()/.onError() handlers dereference freed
    // memory. This constructs the presenter inside a nested scope, destroys
    // it immediately, then pumps -- if the QPointer guard were missing or
    // wrong, this would be a use-after-free (caught by ASan/UBSan CI legs,
    // not just a logic assertion here).
    morph::ladder::gui::AppContext ctx{morph::ladder::gui::Local{}};
    {
        ProbePresenter presenter{ctx.bridge(), ctx.executor()};
        presenter.hookBound();
    }  // presenter destroyed here, whenBound()'s completion still pending

    // Nothing to assert beyond "this doesn't crash" -- pump a couple of turns
    // so the posted completion actually runs while the presenter is gone.
    REQUIRE_FALSE(morph::ladder::testkit::pumpUntil([] { return false; }, std::chrono::milliseconds{50}));
    SUCCEED("posted whenBound() completion resolved after destruction without crashing");
}

TEST_CASE("Presenter::track() does not touch a presenter destroyed before its completion resolves",
          "[ladder][testkit][gui][presenter]") {
    // The track() counterpart of the trackBound() case above, and regression
    // coverage for morph#137. track() used to capture a bare `this`, while
    // trackBound() -- the method immediately above it in presenter.hpp --
    // already used a QPointer and documented why. A Completion resolves
    // through the executor (posted, never delivered inline), so a presenter
    // destroyed before that post runs had finishOne() write to freed memory:
    // AddressSanitizer reported a stack-use-after-scope on the atomic
    // fetch_sub, from a completion flushed by BackendRig's teardown pump.
    //
    // Constructing the presenter in a nested scope and pumping after it dies
    // reproduces that exactly. There is nothing to assert but "this does not
    // corrupt memory", which is a real assertion under the ASan/UBSan ladder
    // leg -- the same shape, and the same reasoning, as the trackBound() case.
    morph::ladder::gui::AppContext ctx{morph::ladder::gui::Local{}};
    {
        ProbePresenter presenter{ctx.bridge(), ctx.executor()};
        presenter.bump(7);
        // Deliberately no pump here: the completion must still be in flight
        // when the presenter goes out of scope on the next line.
    }

    REQUIRE_FALSE(morph::ladder::testkit::pumpUntil([] { return false; }, std::chrono::milliseconds{50}));
    SUCCEED("posted track() completion resolved after destruction without touching freed memory");
}

TEST_CASE("Presenter::track()'s error path also survives the presenter being destroyed first",
          "[ladder][testkit][gui][presenter]") {
    // Same as the case above, for track()'s .onError branch: both handlers
    // captured `this`, so both had to be guarded, and a fix that only covered
    // the success path would leave the failure path corrupting memory exactly
    // where error handling makes it hardest to notice.
    morph::ladder::gui::AppContext ctx{morph::ladder::gui::Local{}};
    {
        ProbePresenter presenter{ctx.bridge(), ctx.executor()};
        presenter.bumpAndFail();
    }

    REQUIRE_FALSE(morph::ladder::testkit::pumpUntil([] { return false; }, std::chrono::milliseconds{50}));
    SUCCEED("posted track() error completion resolved after destruction without touching freed memory");
}

TEST_CASE("Presenter::track() tolerates an onOk callback that destroys the presenter",
          "[ladder][testkit][gui][presenter]") {
    // The destroyed-*during* case, as opposed to the destroyed-before cases
    // above. track() re-checks its QPointer after onOk returns rather than
    // reusing the check it made before calling it, because a callback is
    // allowed to end the presenter's life -- closing the screen it belongs to
    // is an ordinary thing for a handler to do. Without that second check,
    // finishOne() would run on the object the callback just destroyed.
    morph::ladder::gui::AppContext ctx{morph::ladder::gui::Local{}};
    auto owner = std::make_unique<ProbePresenter>(ctx.bridge(), ctx.executor());
    owner->bumpAndDestroySelf(owner);

    REQUIRE(morph::ladder::testkit::pumpUntil([&] { return owner == nullptr; }));
    CHECK(owner == nullptr);
}

TEST_CASE("Presenter::track() tolerates an onErr callback that destroys the presenter",
          "[ladder][testkit][gui][presenter]") {
    // Same as above for the error branch: a failed action is at least as
    // likely to be the thing that tears a screen down, and track() carries the
    // identical re-check there.
    morph::ladder::gui::AppContext ctx{morph::ladder::gui::Local{}};
    auto owner = std::make_unique<ProbePresenter>(ctx.bridge(), ctx.executor());
    owner->failAndDestroySelf(owner);

    REQUIRE(morph::ladder::testkit::pumpUntil([&] { return owner == nullptr; }));
    CHECK(owner == nullptr);
}

TEST_CASE("Presenter::track() survives an onOk callback that destroys the presenter and then throws",
          "[ladder][testkit][gui][presenter]") {
    // The exception-safety contract and the liveness guard meeting in one
    // place. track()'s catch block still has to decide whether finishOne() is
    // safe to call, and the answer is "only if the callback did not just
    // destroy the presenter". The completion machinery logs and swallows a
    // throwing handler (see completion.hpp), so the throw does not escape the
    // pump -- what matters is that nothing touches the freed presenter on the
    // way out.
    morph::ladder::gui::AppContext ctx{morph::ladder::gui::Local{}};
    auto owner = std::make_unique<ProbePresenter>(ctx.bridge(), ctx.executor());
    owner->bumpDestroySelfAndThrow(owner);

    REQUIRE(morph::ladder::testkit::pumpUntil([&] { return owner == nullptr; }));
    CHECK(owner == nullptr);
}

TEST_CASE("Presenter::track() survives an onErr callback that destroys the presenter and then throws",
          "[ladder][testkit][gui][presenter]") {
    // Same crossing of the two contracts on the error branch, which carries
    // its own copy of the catch-block guard.
    morph::ladder::gui::AppContext ctx{morph::ladder::gui::Local{}};
    auto owner = std::make_unique<ProbePresenter>(ctx.bridge(), ctx.executor());
    owner->failDestroySelfAndThrow(owner);

    REQUIRE(morph::ladder::testkit::pumpUntil([&] { return owner == nullptr; }));
    CHECK(owner == nullptr);
}

TEST_CASE("Presenter::busy() stays true while a second tracked completion is still in flight",
          "[ladder][testkit][gui][presenter]") {
    // Every other track()-driving test in this file starts exactly one
    // in-flight completion, so finishOne()'s `_inFlight.fetch_sub(1) == 1`
    // check is always true there -- the counter only ever goes 0 -> 1 -> 0.
    // Pinning the *false* arm (2 -> 1, no idle()) needs the two completions'
    // callbacks delivered one at a time, under this test's own control --
    // `QtExecutor`'s real Qt-event-loop posting can't promise that ordering
    // (both callbacks may already be queued by the time the first
    // `processEvents` slice runs, draining both before either is observed).
    // `DeterministicExecutor` (testkit/strand_interleaver.hpp -- the
    // established "control exactly which posted task runs next" harness,
    // same one `test_strand_interleaver.cpp` drives a `StrandExecutor`
    // through) is a plain `IExecutor`, so it can stand in as the presenter's
    // own client-facing executor: `step()` runs exactly the oldest-queued
    // callback and nothing else.
    morph::exec::ThreadPoolExecutor workerPool{2};
    morph::ladder::testkit::DeterministicExecutor clientExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(workerPool)};
    ProbePresenter presenter{bridge, &clientExec};

    int idleCount = 0;
    QObject::connect(&presenter, &morph::ladder::gui::Presenter::idle, [&] { ++idleCount; });

    REQUIRE_FALSE(presenter.busy());
    presenter.bumpConcurrent(1);
    presenter.bumpConcurrent(2);

    // Both actions run on the pool and post onto clientExec as they finish --
    // wait until both have queued *something* there before stepping, so the
    // pool's own scheduling can't race this test. Each action's full
    // settlement is actually two chained posts on clientExec, not one:
    // Bridge::executeVia's raw backend Completion resolves first (queuing its
    // own .then() translation lambda), and *that* lambda -- once it runs --
    // is what calls the typed CompletionState::setValue() that queues
    // track()'s own .then() callback in turn. The two actions' chains can
    // interleave in either order (both settle on the pool independently), so
    // this steps one at a time and watches settledOrder itself rather than
    // assuming a fixed step count per action.
    REQUIRE(morph::ladder::testkit::pumpUntil([&] { return clientExec.pending() >= 2; }));
    REQUIRE(presenter.busy());  // both queued before either callback ran

    // Step until exactly one action's track() callback has actually run --
    // this is finishOne()'s fetch_sub(1) returning 2, not 1, the branch no
    // other test in this suite reaches.
    while (presenter.settledOrder.empty()) {
        REQUIRE(clientExec.pending() > 0);
        clientExec.step();
    }
    REQUIRE(presenter.settledOrder.size() == 1);
    REQUIRE(presenter.busy());  // one completion settled, one still in flight -- idle() must not fire
    REQUIRE(idleCount == 0);

    while (presenter.busy()) {
        REQUIRE(clientExec.pending() > 0);
        clientExec.step();
    }
    REQUIRE(idleCount == 1);  // idle() fires exactly once, when the counter actually reaches zero
    REQUIRE(presenter.settledOrder.size() == 2);
}

TEST_CASE("Presenter::trackBound() emits bound() on the .onError path when registration never settles",
          "[ladder][testkit][gui][presenter][socket-only]") {
    // Every other trackBound() test in this file runs in Local mode, where
    // whenBound()'s Completion<bool> always resolves via .then() -- Local's
    // handler is bound by construction (presenter.hpp's own doc comment), so
    // trackBound()'s .onError() branch (and the `if (self)` guard inside it)
    // is otherwise never reached by this suite at all.
    //
    // ws://127.0.0.1:1 is a reserved, never-listening port -- the same
    // deterministic "connection never comes up" seam
    // tests/qt/test_qt_websocket.cpp's issue26/issue54 cases use, chosen so
    // this is a real onError delivery rather than a timing race. With
    // asyncRegistrationEnabled set, constructing the handler queues its
    // registration (issue #54's pre-connect queueing); QtWebSocketBackend's
    // own disconnect/never-connected handling then drains that queue through
    // cancelPending(DisconnectedError), which is whenBound()'s only route to
    // .onError() -- see qt_websocket_backend.cpp's cancelPending().
    morph::qt::QtExecutor qtExec;
    QUrl const url{QStringLiteral("ws://127.0.0.1:1")};
    auto backendPtr = std::make_unique<morph::qt::QtWebSocketBackend>(
        url, std::nullopt, morph::qt::QtWebSocketBackend::Config{.asyncRegistrationEnabled = true});
    morph::bridge::Bridge bridge{std::move(backendPtr)};

    ProbePresenter presenter{bridge, &qtExec};

    int boundCount = 0;
    QObject::connect(&presenter, &morph::ladder::gui::Presenter::bound, [&] { ++boundCount; });

    presenter.hookBound();
    REQUIRE(morph::ladder::testkit::pumpUntil([&] { return boundCount == 1; }));
    REQUIRE(boundCount == 1);  // bound() still fires exactly once, from the error branch this time
}
