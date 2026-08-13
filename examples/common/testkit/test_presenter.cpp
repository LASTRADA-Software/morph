// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "gui/app_context.hpp"
#include "gui/presenter.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/pump.hpp"

#include <morph/core/bridge.hpp>

#include <stdexcept>

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

    void bump(int value) {
        track<int>(_handler.execute(PresenterProbeAction{value}), [this](int result) { lastResult = result; });
    }

    /// @brief Drives the model that always throws, so track()'s .onError
    ///        branch (and therefore finishOne() called from there) actually
    ///        runs — the plain success path above never reaches it.
    void bumpAndFail() {
        track<int>(_failHandler.execute(PresenterProbeFailAction{}), [](int) {
            FAIL("onOk must not run for a failed action");
        });
    }

    /// @brief Drives the (successful) probe action, but with an onOk callback
    ///        that itself throws — track()'s catch-block must still call
    ///        finishOne() before rethrowing (presenter.hpp's documented
    ///        exception-safety contract), or busy() would stay true forever.
    void bumpAndThrowFromOnOk() {
        track<int>(_handler.execute(PresenterProbeAction{0}),
                   [](int) -> void { throw std::runtime_error{"presenter probe: onOk threw"}; });
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

    int lastResult = -1;
    bool errorHandlerFired = false;

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

TEST_CASE("Presenter::track() calls finishOne() even when onOk itself throws",
          "[ladder][testkit][gui][presenter]") {
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

TEST_CASE("Presenter::track() calls finishOne() even when onErr itself throws",
          "[ladder][testkit][gui][presenter]") {
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
    // asynchronous and no event-loop turn has run yet. Constructing a
    // BridgeHandler here is exactly the permanent registration failure
    // docs/findings/017-async-registration-fails-before-connect.md describes.
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
