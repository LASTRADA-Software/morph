// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/logger.hpp>

#include <QObject>
#include <QString>
#include <QTimer>

#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

/// @file
/// This rung's framework-level deliverable (Task 15) — "every later rung
/// inherits this helper; get it right here" (this rung's README). A
/// Zulip-pattern event poller: on a fixed interval, ask "everything since my
/// last cursor", apply what comes back, and either keep going or stop.
///
/// @par Design choice: a template, not a `polls`-specific class
/// The task brief explicitly allows either "a fully generic template" or "a
/// narrower, polls-specific-but-easily-generalized type", left to
/// implementation judgment, since a template can be awkward to write cleanly
/// for a first use. This file goes with the template
/// (`EventPoller<EventT, EventIdT>`), for one concrete reason:
/// `examples/common/gui/` cannot depend on `examples/polls/` (rung 3 code
/// building on rung-0/shared infrastructure, never the reverse — see
/// `examples/common/CMakeLists.txt`), so a class living here can never name
/// `polls::PollEvent`/`polls::PollEventId`/`polls::gui::PollPresenter`
/// directly. The two type parameters are the only polls-shaped facts this
/// class actually needs to know about at compile time; everything else —
/// how to dispatch `GetEventsSince`, how to detect the two Bridge error
/// families, what "success" and "one tick" mean operationally — is captured
/// once, here, so kanban's own event feed does not have to re-derive the
/// retry-vs-fatal decision tree from scratch. What kanban supplies per its
/// own rung is a `Dispatch` closure (see below) that knows how to reach
/// *its* presenter; `EventPoller` itself never needs to know that type.
///
/// @par Why dispatch is a caller-supplied closure, not a stored `Presenter&`
/// A `polls::gui::PollPresenter::getEventsSince(GetEventsSince)` call is
/// `void` and reports its outcome through Qt signals shared with every other
/// action that presenter exposes — there is no direct
/// `Completion<GetEventsSinceResult>` handed back to a caller sitting outside
/// the presenter. A concrete `EventPoller` could special-case one presenter's
/// signal shape, but a *generic* one cannot assume any particular presenter's
/// signal surface at all. The `Dispatch` alias below is the seam: it hands
/// the whole "how do I actually reach the backend, and how do I learn what
/// happened" question back to the caller, once per tick.
///
/// @warning Do **not** build a `Dispatch` closure out of a presenter's shared
/// error signal. `polls::gui::PollPresenter::failed(QString)` is *one* signal
/// for all nine `PollModel` actions — every method's `track<T>()` call routes
/// its failure through the same `reportError()`, which emits that same
/// `failed`. A live poll view routinely has `submitVotes`, `addComment` or
/// `finalizePoll` in flight *concurrently* with a poll tick, so a `Dispatch`
/// listening on `failed` cannot tell whose failure it just saw: it will
/// attribute some other action's error to this tick (stopping the poller for
/// an unrelated reason) while this tick's real failure goes to whoever else
/// happened to be listening. Two further defects compound it:
/// `PollPresenter::reportError` catches only `std::exception`, so a
/// non-`std::exception` failure emits nothing at all — `Dispatch` then never
/// calls `onSuccess` *or* `onError`, wedging `_requestInFlight` forever with
/// no recovery — and `failed(QString)` has already stringified the exception,
/// so `ClientTimeoutError` can only be recovered by comparing that `QString`
/// against `ClientTimeoutError{}.what()`, a string comparison standing in for
/// a type check. This route is unsound; do not use it.
///
/// @par The production-safe wiring: one `Dispatch`, one direct dispatch call
/// Build `Dispatch` directly over a dedicated `BridgeHandler<Model,
/// AllowShared>` (or any other API that hands back a
/// `Completion<Result>` per call) and attach `.then()`/`.onError()` to *that
/// call's own* completion. Nothing can be cross-attributed, because the
/// completion belongs to this tick and nothing else; every failure path
/// reaches `onError`, including non-`std::exception` ones; and
/// `ClientTimeoutError` stays a real, catchable C++ type end to end, never
/// stringified. `examples/common/testkit/test_event_poller.cpp`'s
/// `makeDispatch()` is the reference implementation of exactly this shape —
/// read it before wiring a poller into a real GUI shell.
///
/// (If a presenter-mediated route is ever genuinely wanted, the presenter
/// would first have to grow a *dedicated*, typed error signal for the polling
/// action alone — not the shared `failed(QString)` — carrying the
/// `std::exception_ptr` rather than a message. That is out of scope here and
/// no presenter offers it today; the direct-handler route above needs no such
/// change.)
///
/// @par Bridge::setExecuteDeadline is bridge-wide, not per-handler
/// The constructor calls `bridge.setExecuteDeadline(executeDeadline)` itself
/// (the task brief's own instruction: a caller forgetting to configure this
/// is exactly the mistake this helper exists to make impossible — without
/// it, a rate-limited server silently dropping a poll frame hangs the
/// poller's in-flight call forever). This setting lives on the `Bridge`
/// object, not on any one handler, so constructing an `EventPoller` clobbers
/// whatever deadline (if any) was configured on that `Bridge` before, and a
/// second `EventPoller` — or any other code calling `setExecuteDeadline` —
/// against the same `Bridge` clobbers this one's in turn. Fine for this
/// ladder's actual shape (one `Bridge` per `AppContext`, at most one poller
/// per view), but worth knowing before sharing a `Bridge` across components
/// with differing deadline needs.
///
/// That call is also the *only* reason a browser tab would ever need a
/// deadline mechanism at all, and until the final whole-branch review of
/// rung 3 it was a latent WASM abort: `Bridge::setExecuteDeadline` lazily
/// constructs a `morph::async::detail::TimeoutScheduler`, which used to
/// unconditionally spawn a `std::thread` — impossible in the
/// `wasm_singlethread` Qt build this ladder's WASM clients are compiled
/// against. `timeout_scheduler.hpp` now selects a browser-timer
/// (`emscripten_async_call`) build of itself under
/// `__EMSCRIPTEN__ && !__EMSCRIPTEN_PTHREADS__`, so this constructor is
/// safe from a browser tab and deadlines still fire — see that file's
/// `@file` comment and `docs/spec/core/completion.md`. Neither the fix nor
/// the original hazard has been observed on a real Emscripten build; no
/// toolchain for one exists in this repository (the `ladder-wasm` CI job is
/// a compile gate).
///
/// @par Default poll interval and its trade-off
/// `kDefaultInterval` is 3 seconds. This is this class's answer to the
/// README's "Expected strain points" question ("Poll-interval latency: two
/// voters editing simultaneously see each other only on the next tick —
/// measure and document acceptable intervals"): shorter intervals lower that
/// latency but multiply server load and DB read pressure linearly with
/// concurrent viewers (N viewers on one poll = N `GetEventsSince` calls per
/// interval, forever, for as long as the poll stays open); longer intervals
/// do the reverse. 3 seconds sits in the middle of the brief's own suggested
/// 2-3s range: noticeable-but-tolerable staleness for a live vote/comment
/// feed, without turning an open poll page into a request storm. Not a
/// physical constant — override it per call site if a rung's own load
/// profile calls for something else.
///
/// @par Default execute deadline
/// `kDefaultExecuteDeadline` is 5 seconds — generous enough to absorb a real,
/// loaded round trip (matching the order of magnitude `pumpUntil`'s own 5s
/// default budget uses elsewhere in this codebase) while still bounding how
/// long one silently-dropped frame can wedge a poll tick. It deliberately
/// exceeds `kDefaultInterval`: `EventPoller` never lets two dispatches race
/// (see `busy()`), so an in-flight call that outlives one interval simply
/// makes the next timer tick a no-op rather than piling up concurrent calls;
/// the deadline's only job is to guarantee that "no-op" state cannot last
/// forever.
///
/// @note Unlike every other wait budget in the ladder testkit
/// (`examples/common/testkit/pump.hpp`'s `pumpUntil`/`awaitQt`, scaled by the
/// `MORPH_LADDER_DEADLINE_MS` env var via `deadlineScale()`), this constant
/// cannot be scaled the same way: `examples/common/gui/` is production code
/// shipped to real clients and must not depend on `examples/common/testkit/`.
/// A production adapter that constructs an `EventPoller` with no override
/// (e.g. `polls::gui::PollBridge::startPolling`) therefore always arms the
/// unscaled 5s value, even under a test run where `MORPH_LADDER_DEADLINE_MS`
/// has deliberately raised every *other* wait budget for a slow/loaded CI
/// runner or sanitizer build. On such a runner, a test that opens a real
/// adapter and dispatches further actions on the same `Bridge` races those
/// actions against this fixed deadline underneath a scaled test budget meant
/// to give them slack — a real, if currently unobserved, source of spurious
/// CI flakiness. Deliberately not "fixed" by adding a test-only override
/// parameter to `PollBridge`'s constructor: that adapter's own design
/// explicitly avoids exposing internals a test could drive around production
/// wiring (see its own class doc comment). If this ever causes a real,
/// reproduced flake, the right fix is likely a dedicated, clearly-named
/// test-only constructor overload on the adapter (not on this class, which
/// has no test-only knowledge to begin with), not a change here.
///
/// @par Thread affinity
/// Like every other `examples/common/gui/` type, this class owns a `QTimer`
/// and must be constructed and used on the Qt event-loop thread.
namespace morph::ladder::gui {

/// @brief Free functions the template below delegates to — pulled out of the
///        class body (and into `event_poller.cpp`, not header-inlined) for
///        the same reason `examples/common/testkit/pump.hpp`'s
///        `computeDeadlineScale` is factored out of `deadlineScale()`: pure
///        exception-classification logic that has nothing to do with
///        `EventT`/`EventIdT`, and is worth compiling once rather than once
///        per `EventPoller` instantiation.
namespace detail {

/// @brief Whether @p err is a `morph::backend::ClientTimeoutError` — the one
///        error `EventPoller` treats as transient.
///
/// Exactly one `rethrow_exception` and catch: a `ClientTimeoutError` nested
/// inside some other exception (`std::throw_with_nested`) is *not* detected
/// and is treated as fatal. Nothing on this class's paths produces one —
/// `Bridge`'s deadline machinery sets the timeout as the completion's
/// exception directly — so there is no nested walk here to go stale.
/// @param err The exception captured from a dispatch's `onError` callback;
///        `nullptr` is treated as "not a timeout".
/// @return `true` if rethrowing @p err lands in a `ClientTimeoutError` catch.
[[nodiscard]] bool isClientTimeout(const std::exception_ptr& err) noexcept;

/// @brief Renders @p err as the message `onFatalError` receives.
/// @param err The exception captured from a dispatch's `onError` callback.
/// @return `std::exception::what()` if @p err rethrows into one, otherwise a
///         canned "non-std::exception" message; never empty.
[[nodiscard]] QString describeFailure(const std::exception_ptr& err);

}  // namespace detail

/// @brief Periodic "GetEventsSince"-shaped poller — this rung's
///        framework-level deliverable. See this file's own top-of-file
///        comment for the full design rationale.
/// @tparam EventT   One event as the caller's dispatch layer returns it
///         (e.g. `polls::PollEvent`). Never interpreted by this class —
///         only forwarded, one at a time and in order, to `onEvent`.
/// @tparam EventIdT The cursor type (e.g. `polls::PollEventId`). Copied,
///         never compared or arithmetic'd on — advancing it is entirely the
///         `Dispatch` closure's job (it reports back the new value).
template <typename EventT, typename EventIdT>
class EventPoller {
  public:
    /// @brief Applies one event, in the order `Dispatch` returned it.
    ///
    /// @warning Must not destroy the `EventPoller` it belongs to. It is
    /// called from inside `pollOnce()`'s success callback, underneath the
    /// RAII `FlagGuard` that clears `_requestInFlight` when that frame
    /// unwinds — destroying the poller from here leaves that guard writing
    /// to freed storage. (`onFatalError` is the one callback for which
    /// self-destruction *is* supported; see `handleError`.) A view that
    /// wants to close itself in reaction to an event should schedule it —
    /// `QTimer::singleShot(0, …)`, `deleteLater()` — not do it inline.
    using ApplyEvent = std::function<void(const EventT&)>;

    /// @brief Reports the one fatal (non-timeout) failure this poller will
    ///        ever surface — see the class doc comment's retry-vs-fatal rule.
    using OnFatalError = std::function<void(const QString&)>;

    /// @brief One tick's success outcome: every event since the cursor this
    ///        tick dispatched with, oldest first, plus the cursor's new
    ///        value (ordinarily the last event's id; the `Dispatch` closure
    ///        decides, so a batch of zero events can still report the same
    ///        cursor back unchanged).
    using OnSuccess = std::function<void(std::vector<EventT> events, EventIdT newLastEventId)>;

    /// @brief One tick's failure outcome. Whatever `Dispatch` observed —
    ///        typically whatever a `Completion<...>::onError` handed it, or
    ///        (see the class doc comment) whatever a presenter's own
    ///        string-only error signal was translated back into.
    using OnError = std::function<void(std::exception_ptr)>;

    /// @brief One tick's dispatch. Called with the current cursor; must call
    ///        exactly one of `onSuccess`/`onError`, synchronously or later,
    ///        exactly once. Never called again (`pollOnce()` is a no-op)
    ///        until the previous call's outcome has been reported.
    using Dispatch = std::function<void(EventIdT lastEventId, OnSuccess onSuccess, OnError onError)>;

    /// @brief See the class doc comment's "Default poll interval" section.
    static constexpr std::chrono::milliseconds kDefaultInterval{3000};

    /// @brief See the class doc comment's "Default execute deadline" section.
    static constexpr std::chrono::milliseconds kDefaultExecuteDeadline{5000};

    /// @param bridge         The `Bridge` `dispatch` ultimately calls
    ///        through. Used here only to call `setExecuteDeadline` — see the
    ///        class doc comment's "Bridge::setExecuteDeadline is bridge-wide"
    ///        section for why that is the *only* thing this class does with
    ///        it, and why that alone is still worth a reference parameter.
    /// @param startingCursor The cursor to dispatch the first tick with
    ///        (e.g. a freshly opened poll's own `GetPollStateResult`'s
    ///        `lastEventId`).
    /// @param dispatch       One tick's real work — see `Dispatch`'s own doc
    ///        comment.
    /// @param onEvent        Applies one event; called once per event
    ///        returned by a successful tick, in order.
    /// @param onFatalError   Called exactly once, the first time a
    ///        non-`ClientTimeoutError` failure stops this poller.
    /// @param interval       How often to tick. Defaults to
    ///        `kDefaultInterval`.
    /// @param executeDeadline Forwarded to `bridge.setExecuteDeadline()` on
    ///        construction. Defaults to `kDefaultExecuteDeadline`.
    EventPoller(::morph::bridge::Bridge& bridge, EventIdT startingCursor, Dispatch dispatch, ApplyEvent onEvent,
                OnFatalError onFatalError, std::chrono::milliseconds interval = kDefaultInterval,
                std::chrono::milliseconds executeDeadline = kDefaultExecuteDeadline)
        : _lastEventId{std::move(startingCursor)},
          _dispatch{std::move(dispatch)},
          _onEvent{std::move(onEvent)},
          _onFatalError{std::move(onFatalError)} {
        bridge.setExecuteDeadline(executeDeadline);
        // `&_timer` as the connection's context object, not `this`: this
        // class is not itself a `QObject` (see the class doc comment's
        // "template, not a polls-specific class" note — a template cannot
        // carry `Q_OBJECT`/moc output), so `_timer`, a member that is always
        // destroyed before `this`'s storage is freed, stands in as the
        // lifetime anchor Qt's auto-disconnect-on-destruction machinery
        // needs.
        //
        // This covers the *periodic-timer signal* path only, and nothing
        // else. It does not, and cannot, protect the *completion-callback*
        // path: the lambdas `pollOnce()` hands to `_dispatch` are delivered
        // by whatever executor the `Bridge` completes on — in practice
        // `QtExecutor::post`, i.e. `QMetaObject::invokeMethod(...,
        // Qt::QueuedConnection)`, which makes the pending callback an event
        // owned by `QCoreApplication`, not a connection owned by `_timer`.
        // Destroying `_timer` disconnects nothing of the sort. The
        // `_liveness` token (last member; see its declaration) is what
        // guards that path instead.
        QObject::connect(&_timer, &QTimer::timeout, &_timer, [this] { pollOnce(); });
        _timer.start(interval);
    }

    ~EventPoller() = default;
    EventPoller(const EventPoller&) = delete;
    EventPoller& operator=(const EventPoller&) = delete;
    EventPoller(EventPoller&&) = delete;
    EventPoller& operator=(EventPoller&&) = delete;

    /// @brief Runs one tick right now, synchronously dispatching (though the
    ///        outcome may resolve later, asynchronously).
    ///
    /// A no-op if a fatal error has already stopped this poller, or if a
    /// previously dispatched tick has not yet reported its outcome — ticks
    /// never overlap. This is what the owned `QTimer` calls on every
    /// `interval`; it is public so a caller (or a test) can drive a tick
    /// deterministically instead of waiting on the real timer — see
    /// `examples/common/testkit/test_event_poller.cpp`'s own "drive the
    /// timer manually" tests.
    void pollOnce() {
        if (_fatal || _requestInFlight) {
            return;
        }
        _requestInFlight = true;
        _dispatch(
            _lastEventId,
            [this, alive = std::weak_ptr<const void>{_liveness}](std::vector<EventT> events,
                                                                 EventIdT newLastEventId) {
                // Liveness check first, before touching any member: this
                // callback outlives `this` whenever the poller is destroyed
                // with a tick in flight. See `_liveness`'s declaration.
                if (alive.expired()) {
                    return;
                }
                // Cursor first, in-flight flag last. The window between them
                // is exactly the window in which `_onEvent` runs, and
                // `_onEvent` is caller code that may spin a nested Qt event
                // loop (a modal dialog is ordinary GUI behaviour) and
                // reenter `pollOnce()`. Advancing `_lastEventId` up front
                // means such a reentrant tick asks for events *after* this
                // batch rather than replaying it; keeping `_requestInFlight`
                // set until this frame unwinds means it is refused outright,
                // so this frame's later writes cannot rewind whatever a
                // nested frame already advanced to.
                _lastEventId = std::move(newLastEventId);
                // RAII, not a plain assignment after the loop: a throwing
                // `_onEvent` must still clear the flag, or `busy()` stays
                // true forever and the poller never ticks again — the same
                // hazard (and the same rule) as
                // `examples/common/gui/presenter.hpp`'s `Presenter::track()`.
                // A local guard struct, matching this codebase's existing
                // idiom (`include/morph/net/socket_server.hpp`'s
                // `ScopeGuard`); there is no shared scope-guard type here.
                struct FlagGuard {
                    explicit FlagGuard(bool& target) : flag{target} {}
                    ~FlagGuard() { flag = false; }
                    FlagGuard(const FlagGuard&) = delete;
                    FlagGuard& operator=(const FlagGuard&) = delete;
                    FlagGuard(FlagGuard&&) = delete;
                    FlagGuard& operator=(FlagGuard&&) = delete;
                    bool& flag;
                };
                const FlagGuard guard{_requestInFlight};
                for (const auto& event : events) {
                    _onEvent(event);
                }
            },
            [this, alive = std::weak_ptr<const void>{_liveness}](std::exception_ptr err) {
                if (alive.expired()) {
                    return;
                }
                _requestInFlight = false;
                handleError(err);
            });
    }

    /// @brief (Re)arms the periodic timer at its configured interval. Already
    ///        running on construction; this is for a caller that previously
    ///        called `stop()` (e.g. a hidden poll view pausing its own
    ///        polling). A no-op once a fatal error has stopped this poller
    ///        for good.
    void start() {
        if (!_fatal) {
            _timer.start();
        }
    }

    /// @brief Disarms the periodic timer without treating this as a fatal
    ///        error — `onFatalError` is not called. Idempotent
    ///        (`QTimer::stop()` on a stopped timer is a no-op).
    void stop() { _timer.stop(); }

    /// @brief Clears a fatal error, resets the cursor, and rearms the timer.
    ///
    /// The supported way back from `onFatalError`. A fatal error is normally
    /// permanent: `start()` refuses to rearm and `_fatal` never clears, so
    /// without this method a caller's only recovery would be destroying and
    /// reconstructing the whole poller — which also re-runs the constructor's
    /// `bridge.setExecuteDeadline()` call and so clobbers whatever deadline
    /// anything else on that same `Bridge` had set since (see the class doc
    /// comment's "bridge-wide, not per-handler" section).
    ///
    /// This exists because the fatal errors this class reports are exactly
    /// the ones a GUI recovers from by *resyncing*: a stale cursor whose
    /// events the server has already pruned fails the tick, the view falls
    /// back to a full `GetPollState`, and that result carries a fresh
    /// `lastEventId` to resume incremental polling from. Pass that value
    /// here.
    ///
    /// Calling this on a poller that never went fatal is still meaningful —
    /// it repoints the cursor and rearms — but note it does *not* cancel a
    /// tick already in flight: if `busy()` is true, that tick's own success
    /// callback will overwrite @p newCursor with whatever it reports. Resume
    /// once the poller is idle.
    ///
    /// @param newCursor The cursor the next tick dispatches with, ordinarily
    ///        obtained from the full-state resync that followed the fatal
    ///        error.
    void resume(EventIdT newCursor) {
        _fatal = false;
        _lastEventId = std::move(newCursor);
        _timer.start();
    }

    /// @brief Whether the periodic timer is currently armed.
    /// @return `true` if a tick will fire on the next `interval` elapsing.
    [[nodiscard]] bool running() const noexcept { return _timer.isActive(); }

    /// @brief Whether a dispatched tick's outcome has not yet been reported.
    /// @return `true` while `pollOnce()` would be a no-op because a previous
    ///         tick is still outstanding.
    [[nodiscard]] bool busy() const noexcept { return _requestInFlight; }

    /// @brief Whether `onFatalError` has already fired.
    /// @return `true` once a non-timeout dispatch failure has stopped this
    ///         poller for good.
    [[nodiscard]] bool fatalErrorReported() const noexcept { return _fatal; }

    /// @brief The cursor the next tick will dispatch with.
    /// @return The cursor the most recent successful tick reported, or the
    ///         constructor's `startingCursor` if no tick has yet succeeded.
    ///         Advanced *before* that tick's `onEvent` fan-out, not after it
    ///         (see `pollOnce()`'s "cursor first, in-flight flag last"
    ///         note), so a value read from inside `onEvent` already names the
    ///         batch being applied — and a throwing `onEvent` does not rewind
    ///         it. A failed tick leaves it untouched; `resume()` sets it
    ///         outright.
    [[nodiscard]] const EventIdT& lastEventId() const noexcept { return _lastEventId; }

  private:
    /// @brief Routes one tick's failure: retry (log, stay armed) for
    ///        `ClientTimeoutError`, stop-and-report-once for anything else.
    /// @param err The exception a `Dispatch` call's `onError` reported.
    void handleError(const std::exception_ptr& err) {
        if (detail::isClientTimeout(err)) {
            ::morph::log::logError(
                "EventPoller: GetEventsSince timed out waiting for a reply (Bridge::setExecuteDeadline); "
                "retrying on the next tick");
            return;
        }
        if (_fatal) {
            // Load-bearing, not merely defensive. `pollOnce()` refuses to
            // dispatch a *new* tick once `_fatal` is set, but nothing
            // mechanically enforces `Dispatch`'s "call exactly one of
            // onSuccess/onError, exactly once" contract — it is a
            // caller-supplied `std::function`, and a closure that
            // double-reports (e.g. one wired to a signal that fires twice,
            // or one whose `.onError` is also reached by a second failure
            // path) lands here with `_fatal` already set. This is the check
            // that keeps `onFatalError`'s "exactly once" promise true
            // regardless.
            return;
        }
        _fatal = true;
        _timer.stop();
        const QString message = detail::describeFailure(err);
        ::morph::log::logError("EventPoller: dispatch failed non-recoverably, polling stopped: " +
                                message.toStdString());
        if (_onFatalError) {
            // Deliberately the last statement of this function, and it must
            // stay that way: `onFatalError` destroying the `EventPoller` is a
            // natural GUI reaction ("the poll is gone, close this view"), and
            // it is safe today only because (a) nothing here touches a member
            // after this call returns, and (b) the callback that reached
            // `handleError` is owned by the `Completion`'s own
            // `CompletionState`, which is reference-counted independently of
            // this object — so the lambda frame itself survives its own
            // `this` being freed. Appending any member access after this
            // line, or ever invoking `_onFatalError` from a lambda that the
            // `EventPoller` itself owns, breaks that and reintroduces a
            // use-after-free.
            //
            // One caveat the two conditions above do not cover: `_onFatalError`
            // is itself a member, so this very call expression reads storage
            // that the callback it invokes may free. A `std::function`'s
            // invocation does not copy its target, and a callback that
            // destroys the poller destroys the `std::function` frame it is
            // running inside. It is safe today only because no callback wired
            // anywhere in this repository does that — the one real callback,
            // `PollBridge`'s (`examples/polls/gui_lib/poll_qml_bridges.cpp`),
            // emits `pollingStopped`, which nothing in this rung's QML is
            // even connected to, let alone tears the poll view down from. A
            // future callback that really must destroy the poller should be
            // given a local copy to invoke (`auto callback = _onFatalError;
            // callback(message);`) rather than relying on this member
            // surviving its own invocation.
            _onFatalError(message);
        }
    }

    // Member declaration order below is load-bearing in two places; do not
    // reorder without reading both.
    //  - `_timer` must remain a *member* (not, say, a `unique_ptr` released
    //    early or an object owned elsewhere), because it is the context
    //    object of the `timeout` connection the constructor makes: being a
    //    member is what guarantees it is destroyed — and so the connection
    //    auto-disconnected — before this object's storage goes away. That
    //    covers the timer signal path, and only that path.
    //  - `_liveness` must stay **last**. Members are destroyed in reverse
    //    declaration order, so the last-declared member is destroyed first:
    //    the token expires before anything a completion callback might touch
    //    (`_requestInFlight`, `_onEvent`, `_lastEventId`, `_dispatch`, …) has
    //    been torn down, which is precisely what makes the `alive.expired()`
    //    checks in `pollOnce()` correct rather than racy. Same reasoning, and
    //    the same placement, as `morph::bridge::Bridge::_liveness`
    //    (`include/morph/core/bridge.hpp`).
    EventIdT _lastEventId;
    Dispatch _dispatch;
    ApplyEvent _onEvent;
    OnFatalError _onFatalError;
    QTimer _timer;
    bool _requestInFlight = false;
    bool _fatal = false;
    /// @brief Weak-observable proof this object still exists.
    ///
    /// The callbacks `pollOnce()` hands to `_dispatch` capture a
    /// `std::weak_ptr` to this and bail out if it has expired. They cannot
    /// capture `this` alone: a completion callback is delivered through
    /// `QtExecutor::post` → `QMetaObject::invokeMethod(...,
    /// Qt::QueuedConnection)`, making it a queued event owned by
    /// `QCoreApplication` — nothing about destroying an `EventPoller` (its
    /// `_timer` included) cancels it. Destroying a poller while `busy()` is
    /// true is the *ordinary* case (a user closes a poll view mid-tick), not
    /// an edge case, and without this token that queued callback fires into
    /// freed memory. Same pattern, for the same reason, as
    /// `morph::bridge::Bridge::_liveness` and
    /// `examples/common/testkit/backend_rig.hpp`'s
    /// `QtDrivenMainThreadExecutor`. **Must remain the last declared member**
    /// — see the note above.
    std::shared_ptr<const void> _liveness{std::make_shared<char>()};
};

}  // namespace morph::ladder::gui
