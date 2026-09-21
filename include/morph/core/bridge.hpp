// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <any>
#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../attributes.hpp"
#include "../forms/forms.hpp"
#include "../session/session.hpp"
#include "backend.hpp"
#include "callback_scope.hpp"
#include "completion.hpp"
#include "detail/subscription_registry.hpp"
#include "model_key.hpp"
#include "registry.hpp"
#include "timeout_scheduler.hpp"

namespace morph::bridge {

/// @brief Type-erased, JSON-in/JSON-out execute path for actions whose
///        concrete C++ type is only known by its registered string id at
///        the call site (e.g. a schema-driven GUI that reads action names
///        out of a JSON Schema at runtime).
///
/// Populated automatically by `BRIDGE_REGISTER_ACTION` — no action-specific
/// code is required at any call site. Every entry calls through the real
/// `BridgeHandler<Model>::execute<Action>()` (so sessions, backend
/// switches, and completions all behave exactly as they do for hand-written
/// call sites), unlike `morph::model::detail::ActionDispatcher`, which
/// calls `Model::execute` directly against an already-owned model holder
/// and is only ever used server-side.
///
/// HARD REQUIREMENT (other direction of the same constraint documented on
/// `morph::model::detail::registerActionExecutorOnce` in `registry.hpp`): registration into
/// this registry happens via `registerActionExecutorOnce<Model, Action>`, which
/// `BRIDGE_REGISTER_ACTION` calls unconditionally but which is only defined here, in
/// `bridge.hpp`. Every translation unit that calls `BRIDGE_REGISTER_ACTION` must therefore
/// include this header (directly or transitively), or that translation unit's static
/// initializer will fail to link.
class ActionExecuteRegistry {
public:
    /// @brief Deserialises `bodyJson`, dispatches through the handler's
    ///        `Bridge`, and resolves with the JSON-encoded result.
    using Executor = std::function<::morph::async::Completion<std::string>(void*, std::string_view)>;

    /// @brief Registers the executors for `(Model, Action)` under the given string ids.
    ///
    /// Populates one `Executor` per `Sharing` policy the framework defines
    /// (`NoSharing` and `AllowShared`), keyed by
    /// `(modelId, actionId, typeid(Sharing))`, rather than a single executor
    /// that unconditionally assumes `NoSharing`. A handler whose `Sharing` is
    /// anything else must be dispatched through the *matching* executor: the
    /// wrong one `static_cast`s `handlerVoid` to the wrong `BridgeHandler<Model,
    /// Sharing>` instantiation, so `kShared` resolves incorrectly inside it and
    /// a payload-/result-keyed action's attach-or-promote step silently never
    /// runs. Both executors' bodies are otherwise byte-for-byte identical
    /// (built once, from the same generic lambda template, instantiated for
    /// `NoSharing` and again for `AllowShared` — the only two sharing tags
    /// `morph::bridge` defines), so this costs one extra closure per
    /// registered action, not per call.
    /// Defined out-of-line after BridgeHandler to avoid forward reference issues.
    /// @tparam Model  Model type whose handler will execute the action.
    /// @tparam Action Action type to register.
    /// @param modelId  String id the model is registered under.
    /// @param actionId String id the action is registered under.
    template <typename Model, typename Action>
    void registerAction(std::string_view modelId, std::string_view actionId);

    /// @brief Looks up and invokes the executor for `(modelId, actionId)`,
    ///        specialised for the caller's own `Sharing` policy.
    /// @tparam Sharing `NoSharing` or `AllowShared` — the caller's own sharing policy.
    /// @param modelId  String id of the target model.
    /// @param actionId String id of the action to execute.
    /// @param handler  Type-erased `BridgeHandler<Model, Sharing>*` matching `modelId`.
    /// @param bodyJson JSON-encoded action payload.
    /// @return Completion that resolves with the JSON-encoded action result.
    /// @throws std::runtime_error if no executor was registered for that triple.
    template <typename Sharing>
    [[nodiscard]] ::morph::async::Completion<std::string> execute(std::string_view modelId, std::string_view actionId,
                                                                  void* handler, std::string_view bodyJson) const {
        auto iter =
            _executors.find(Key{std::string{modelId}, std::string{actionId}, std::type_index{typeid(Sharing)}});
        if (iter == _executors.end()) {
            throw std::runtime_error("unknown action for executeJson: " + std::string{modelId} + "/" +
                                     std::string{actionId});
        }
        return iter->second(handler, bodyJson);
    }

    /// @brief Returns the process-level singleton registry.
    /// @return Reference to the singleton `ActionExecuteRegistry`.
    static ActionExecuteRegistry& instance();

private:
    struct Key {
        std::string modelId;
        std::string actionId;
        std::type_index sharing;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& key) const noexcept {
            std::size_t const modelHash = ::morph::model::detail::PairKeyHash{}({key.modelId, key.actionId});
            return modelHash ^ (key.sharing.hash_code() + 0x9e3779b9U + (modelHash << 6) + (modelHash >> 2));
        }
    };
    std::unordered_map<Key, Executor, KeyHash> _executors;
};

inline ActionExecuteRegistry& ActionExecuteRegistry::instance() {
    static ActionExecuteRegistry inst;
    return inst;
}

}  // namespace morph::bridge

namespace morph::model::detail {

template <typename Model, typename Action>
inline bool registerActionExecutorOnce(std::string_view modelId, std::string_view actionId) noexcept {
    ::morph::bridge::ActionExecuteRegistry::instance().registerAction<Model, Action>(modelId, actionId);
    return true;
}

}  // namespace morph::model::detail

namespace morph::bridge {

namespace detail {

/// @brief Compile-time decomposition of a pointer-to-data-member type.
///
/// Recovers both the class and the member type from a single non-type template
/// parameter, so callers name a field as `&MyAction::c` with no redundant type
/// arguments.
///
/// @tparam T The pointer-to-member type (e.g. `double MyAction::*`).
template <typename T>
struct MemberPointerTraits;

template <typename V, typename A>
struct MemberPointerTraits<V A::*> {
    /// @brief The class the member belongs to.
    using ClassType = A;
    /// @brief The type of the member itself.
    using ValueType = V;
};

/// @brief Internal linkage record between a model type and its active backend id.
///
/// Shared between `Bridge` and `BridgeHandler`. `Bridge::switchBackend()` updates
/// `currentId` atomically under its mutex so that concurrent `executeVia()` calls
/// always see a consistent value.
struct HandlerBinding {
    /// @brief String type-id of the model.
    std::string typeId;

    /// @brief Factory used to re-register the model on a backend switch.
    std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> modelFactory;

    /// @brief Stable identity of this model instance (e.g. an account id).
    ///
    /// Empty by default — the common local-mode case needs nothing here, since
    /// `modelFactory` can already call `IModelHolder::attachActionLog` directly
    /// with whatever identity it captures. Set this when the active backend may
    /// be remote (`SimulatedRemoteBackend`, `QtWebSocketBackend`): it travels in
    /// the `register` wire envelope so a server-side `RemoteServer::LogProvider`
    /// can attach a log to the instance it creates. See `IBackend::registerModelWithContext`.
    std::string contextKey;

    /// @brief Canonical string encoding of this instance's primary key.
    ///
    /// Empty until a keyed action or an explicit `attach` supplies one. Only
    /// meaningful when `shared` is set; a private binding never consults it.
    /// Mutated only under `Bridge::_attachMtx` (or, during `switchBackend()`
    /// and the reconnect handler, both `_attachMtx` and `_mtx` together).
    std::string primary;

    /// @brief Whether this binding participates in the shared instance directory.
    ///
    /// Set once at construction from `BridgeHandler`'s `Sharing` template
    /// argument and never changed. A shared binding defers its backend
    /// registration until it has a primary, so `currentId` stays `0` — and
    /// `executeVia` fails fast on "handler not bound" — until then.
    bool shared = false;

    /// @brief Current `ModelId` value in the active backend (0 = unbound).
    std::atomic<uint64_t> currentId{0};

    /// @brief Registration-settled seam (see `Bridge::whenBound`).
    ///
    /// `registrationInFlight` is `true` from just *before* `registerHandlerImpl`
    /// calls the backend until that call's continuation resolves. It is set
    /// unconditionally on every path, the blocking one included — see the
    /// comment at the assignment for why it must be set before the backend call
    /// rather than after. A backend that settles inside the dispatch frame does
    /// not *leave* it set: the continuation runs before `registerHandlerImpl`
    /// returns and resolves the waiters (clearing the flag). `whenBound()`
    /// checks it to distinguish "an async reply is coming, queue a waiter"
    /// from "nothing is in flight, resolve false now". `registrationWaiters`
    /// holds callbacks queued by `whenBound()` while `registrationInFlight` is
    /// `true`; the resolving callback invokes and clears every one of them
    /// exactly once, in the same call that clears `registrationInFlight`.
    /// Guarded by `registrationMtx`, deliberately separate from
    /// `Bridge::_mtx`/`_attachMtx`: a waiter can be queued or resolved from
    /// either the registering thread or the backend's reply-delivering
    /// thread, and must never block on the bridge's own locks.
    std::mutex registrationMtx;
    bool registrationInFlight = false;
    std::vector<std::pair<std::function<void(bool)>, std::function<void(std::exception_ptr)>>> registrationWaiters;
};

/// @brief Renders @p failure as the diagnostic string a log line wants.
///
/// The `Completion` surface carries an `exception_ptr`; the one remaining place
/// that needs text rather than a rethrowable failure is a log message. Kept
/// here rather than inline so "what does a rejected control call read like"
/// has one answer.
///
/// @param failure Rejection to describe; may be null.
/// @return `what()` for a `std::exception`, or a generic stand-in otherwise.
inline std::string describeFailure(const std::exception_ptr& failure) {
    if (!failure) {
        return "unknown error";
    }
    try {
        std::rethrow_exception(failure);
    } catch (const std::exception& exc) {
        return exc.what();
    } catch (...) {
        return "unknown error";
    }
}

/// @brief Outcome a backend's inline completion parked for its dispatcher.
struct ParkedOutcome {
    /// @brief `true` when the parked outcome is a success.
    bool succeeded = false;
    /// @brief Instance id the success callback reported.
    ::morph::exec::detail::ModelId modelId{};
    /// @brief Diagnostic the error callback reported; null on success.
    std::exception_ptr failure;
};

/// @brief What `Bridge::switchBackend`'s staging phase accumulated before it
///        committed or rolled back.
///
/// Its own type rather than four parallel locals because the rollback and the
/// commit each need a different subset of it, and a subset silently missed is
/// how a waiter ends up hanging on a registration that will never report.
struct StagedRebinds {
    /// @brief Bindings still live, in `_handlers` order; replaces `_handlers` on commit.
    std::vector<std::weak_ptr<HandlerBinding>> live;

    /// @brief `(binding, newId)` for every bind that settled successfully in
    ///        the staging frame. Published on commit, deregistered on rollback.
    std::vector<std::pair<std::shared_ptr<HandlerBinding>, std::uint64_t>> staged;

    /// @brief Bindings whose bind is still in flight; published as unbound and
    ///        bound later by their own reply. Only ever non-empty for a
    ///        `kCallerMustNotBlock` backend.
    std::vector<std::shared_ptr<HandlerBinding>> deferred;

    /// @brief Every binding this staging phase marked `registrationInFlight`.
    ///        Settled whichever way the phase ends.
    std::vector<std::shared_ptr<HandlerBinding>> armed;
};

/// @brief Handoff slot between an async attach/bind dispatch and its callback.
///
/// `Bridge::attachHandlerAsync`/`ensureBoundAsync` dispatch to the backend while
/// holding `_attachMtx`, and both promise to release it before invoking their
/// `onDone`. A backend whose `bindModel` settles its `Completion` *inline* —
/// synchronously, before the dispatch call returned, as `QtWebSocketBackend`
/// does on its `!_connected` error branch — would otherwise break that promise
/// from inside the dispatch frame, with the lock still held (the executor
/// those call sites name is `inlineExecutor()`, so an inline settle runs the
/// continuation right there).
///
/// So instead of acting, such a callback parks its outcome here and returns; the
/// dispatching frame picks it up after the dispatch call returns, publishes it
/// under the lock it already holds, releases the lock, and only then reports.
/// `mtx` makes the handover race-free even for a backend that replies from
/// another thread *while* its own dispatch call is still on this stack, and the
/// `fired` flag keeps `onDone` invoked exactly once on every interleaving.
struct AsyncDispatchHandoff {
    /// @brief Guards every other field; never held across `onDone` or `_attachMtx`.
    std::mutex mtx;
    /// @brief Signalled once `fired` is set, for `awaitHandoff`'s benefit.
    ///
    /// Only a dispatcher that deliberately waits (`awaitHandoff`) ever blocks on
    /// this; the three asynchronous dispatch sites never do, so for them the
    /// `notify_all` in `parkIfInFrame` is an uncontended no-op.
    std::condition_variable settled;
    /// @brief `true` while the backend's dispatch call is still on the caller's stack.
    bool inFrame = true;
    /// @brief Set once either callback has claimed the outcome.
    bool fired = false;
    /// @brief `true` when the parked outcome is a success, `false` for a failure.
    bool succeeded = false;
    /// @brief Instance id the success callback reported.
    ::morph::exec::detail::ModelId modelId{};
    /// @brief Diagnostic the error callback reported; null on success.
    std::exception_ptr failure;
};

/// @brief Records a backend callback's outcome in @p handoff.
///
/// Called first thing by both completion callbacks of an async attach/bind
/// dispatch.
///
/// @param handoff   Handoff slot created by the dispatching frame.
/// @param succeeded `true` for the success callback, `false` for the error one.
/// @param modelId   Instance id, for the success callback; ignored otherwise.
/// @param failure   Diagnostic, for the error callback; null otherwise.
/// @return `true` if the caller must **not** act — either because the dispatch
///         call is still on the dispatcher's stack (which owns the outcome from
///         here on) or because another callback already claimed this dispatch.
///         `false` if the caller owns the outcome and should deliver it itself.
///
/// @par Reachability of the double-claim arm
/// No backend can reach it today, and that is a property of the callers rather
/// than of this function (morph#648). Every one of the eight call sites below
/// is a `.then`/`.onError` on one `Completion`, and a `CompletionState` settles
/// once — the second `resolve`/`reject` is a documented no-op — so exactly one
/// of the two lambdas runs, exactly once, and `fired` is always `false` on
/// entry. Measured, not assumed: replacing the arm's `return true` with an
/// `abort()` runs the whole suite (1556 cases) plus `morph_net_tests` (191)
/// without firing. The arm is kept because the invariant that makes it dead is
/// every *current* caller's, and a ninth site that does not park a single
/// `Completion`'s outcome would need it again; `tests/test_async_registration.cpp`
/// calls this function directly so the arm is pinned by a test rather than
/// merely unreached.
inline bool parkIfInFrame(AsyncDispatchHandoff& handoff, bool succeeded, ::morph::exec::detail::ModelId modelId,
                          std::exception_ptr failure) {
    bool inFrame = false;
    {
        std::scoped_lock const guard{handoff.mtx};
        if (handoff.fired) {
            // A backend is contractually allowed exactly one callback per dispatch;
            // swallow a second one rather than reporting twice. Unreachable from
            // a backend since morph#571 put every dispatch behind one
            // `Completion` -- see @par Reachability above for what that rests on
            // and why the arm stays.
            return true;
        }
        handoff.fired = true;
        handoff.succeeded = succeeded;
        handoff.modelId = modelId;
        handoff.failure = std::move(failure);
        inFrame = handoff.inFrame;
    }
    // Outside the lock: a waiter in `awaitHandoff` re-acquires `mtx` the moment
    // it wakes, and notifying while still holding it makes it wake only to block
    // again.
    handoff.settled.notify_all();
    return inFrame;
}

/// @brief Closes the inline window and takes whatever a callback parked.
///
/// Called by the dispatching frame immediately after the backend's dispatch call
/// returns. After this, a callback that has not yet run delivers its own outcome.
///
/// @param handoff Handoff slot created by the dispatching frame.
/// @return The parked outcome if the backend completed inline (or concurrently,
///         before this frame closed the window); `std::nullopt` if the frame won
///         the race and the reply, if any, is still to come.
inline std::optional<ParkedOutcome> claimHandoff(AsyncDispatchHandoff& handoff) {
    std::scoped_lock const guard{handoff.mtx};
    handoff.inFrame = false;
    if (!handoff.fired) {
        return std::nullopt;
    }
    return ParkedOutcome{.succeeded = handoff.succeeded, .modelId = handoff.modelId, .failure = handoff.failure};
}

/// @brief Holds the inline window open until a callback parks an outcome in it,
///        then closes it and takes that outcome.
///
/// `claimHandoff`'s blocking twin, and the whole of what
/// `BindWait::kCallerMayBlock` buys: the dispatching frame stops and waits, so a
/// reply that lands on the backend's own thread is still delivered *by this
/// frame*, on this thread, before the dispatching call returns — which is
/// exactly what the frame does for a backend that settled inline. The outcome
/// therefore takes the same path in both cases (published here, rethrown here),
/// instead of one path for "settled in frame" and another for "settled later".
///
/// Only ever called on a backend that returned `BindWait::kCallerMayBlock`,
/// whose contract is that the completion settles without this thread doing
/// anything. There is deliberately no timeout: a bounded wait would make
/// "is the handler bound when `registerHandler` returns" depend on how fast the
/// network was, which is the non-determinism the synchronous contract exists to
/// exclude. A backend that violates its own settle-exactly-once contract hangs
/// here rather than silently returning an unbound handler, and a hang is the
/// diagnosable failure of those two.
///
/// @param handoff Handoff slot created by the dispatching frame.
/// @return The outcome a callback parked; never `std::nullopt`.
inline std::optional<ParkedOutcome> awaitHandoff(AsyncDispatchHandoff& handoff) {
    std::unique_lock guard{handoff.mtx};
    handoff.settled.wait(guard, [&handoff] { return handoff.fired; });
    handoff.inFrame = false;
    return ParkedOutcome{.succeeded = handoff.succeeded, .modelId = handoff.modelId, .failure = handoff.failure};
}

/// @brief Delivers a registration continuation that arrived *after* its
///        dispatching frame closed the handoff window.
///
/// The counterpart to `parkIfInFrame` returning `false`: nobody is left on the
/// dispatching stack to publish this outcome, so the callback publishes it
/// itself — and, until morph#588, published it on whichever thread the backend
/// happened to settle the `Completion` on, because the executor every dispatch
/// site names is `exec::detail::inlineExecutor()`. That is the thread the
/// morph#486 use-after-free is about: each of these callbacks asks "is the
/// `Bridge` still alive" and then touches it, and a `~Bridge` running
/// concurrently on another thread can land between the two steps.
///
/// Routing only the late delivery through an executor the `Bridge` was given
/// closes that window structurally for an embedder whose executor runs tasks
/// on the thread that also runs `~Bridge`: the continuation and the destructor
/// are then two tasks on one thread and cannot interleave at all. The in-frame
/// case deliberately does not go through @p exec — see `Bridge`'s
/// `bridgeExec` constructor parameter for why posting it would turn
/// synchronous registration asynchronous and deadlock a caller that waits on
/// `awaitHandoff` from the executor's own thread.
///
/// A template rather than a `std::function` parameter, and that is not
/// incidental: with a null @p exec the callable is invoked **in place**, so the
/// default path type-erases nothing and allocates nothing. Taking a
/// `std::function` would have put a heap allocation — sometimes a large one,
/// since these closures carry a primary key — on the path that existed before
/// morph#588, which `tests/test_async_registration.cpp`'s morph#108
/// allocation-failure case detects by catching the wrong allocation.
///
/// @tparam Action Callable of no arguments; convertible to `std::function` only
///                on the posting path.
/// @param exec   Executor to deliver on. Borrowed, and may be null, which is
///               the default a `Bridge` constructed without one carries: a
///               null @p exec runs @p action inline, exactly where it ran
///               before morph#588. Non-null, it must outlive every in-flight
///               registration, because a reply can land after `~Bridge`.
/// @param action Work to run. Must be safe to run after `~Bridge` — every
///               caller here gates on a `CallbackToken` or a
///               `detail::BridgeLifetime` before touching the bridge.
template <typename Action>
void deliverLate(::morph::exec::IExecutor* exec, Action&& action) {
    if (exec == nullptr) {
        std::forward<Action>(action)();
        return;
    }
    exec->post(std::function<void()>{std::forward<Action>(action)});
}

/// @brief The gate that makes "the `Bridge` is still there" and "call into it"
///        a single, indivisible step.
///
/// `morph::async::CallbackToken` answers only *"was the scope active at the
/// moment of the check"* — advisory across threads by design
/// (docs/spec/core/callback_scope.md, "Boundary of the guarantee"). That is
/// enough to gate **delivery** of a callback, because a suppressed callback is
/// simply not run and the check being stale costs nothing. It is *not* enough
/// to gate a **member call on the `Bridge`**: the bridge can be destroyed in
/// the instructions between the check and the call, and the call then runs on
/// destroyed memory. That is issue #486 — a `~BridgeHandler` running on a
/// worker thread saw an active token, and `Bridge::deregisterHandler` then
/// iterated a `_handlers` vector whose `Bridge` the owning thread had already
/// finished destroying.
///
/// This type closes that window structurally rather than per call site: the
/// answer is only ever read while `mtx` is held, and `~Bridge` flips it while
/// holding the same mutex exclusively. A caller that observes `alive == true`
/// therefore *keeps* the bridge alive for as long as it stays inside the
/// shared lock, because `~Bridge` cannot get past its own first statement
/// until that lock is released.
///
/// Owned through a `shared_ptr`, so it outlives the `Bridge` for exactly as
/// long as some handler still holds a reference to it and can ask the
/// question.
///
/// @par Why a blocking gate is acceptable here
/// `~Bridge` waits only for work that is provably non-blocking and bounded.
/// There are three guarded regions: `~BridgeHandler`'s `deregisterHandler`
/// call, `executeVia`'s `.then` continuation around `onResult`, and
/// `registerHandlerImpl`'s async `onRegistered` callback. The argument below is
/// about the first; each of the other two carries its own at its call site.
/// `IBackend::deregisterModel` never blocks on another thread on any shipped
/// backend — `LocalBackend` erases map entries under its own mutex,
/// `SimulatedRemoteBackend` runs the envelope inline via
/// `RemoteServer::handleInline`, and `QtWebSocketBackend`/`SocketBackend` are
/// documented fire-and-forget sends precisely so that destruction never spins
/// a nested event loop. A destructor that blocks on a bounded predicate is
/// also the framework's existing idiom for exactly this class of hazard —
/// `~StrandExecutor` blocks until `_inFlight == 0`
/// (docs/spec/concurrency_and_lifetimes.md, "Destruction ordering").
struct BridgeLifetime {
    /// Held shared by a caller for the whole of its call into the `Bridge`,
    /// and exclusively by `~Bridge` while it retires the bridge.
    std::shared_mutex mtx;
    /// `true` until `~Bridge` begins. Read and written only under `mtx`.
    bool alive = true;
};

}  // namespace detail

/// @brief Central dispatcher that routes typed actions to an `IBackend`.
///
/// `Bridge` owns exactly one active backend at a time. It tracks all registered
/// `HandlerBinding` instances and re-registers them automatically when
/// `switchBackend()` is called, enabling seamless local ↔ remote transitions.
///
/// @par Thread safety
/// All public methods are thread-safe. `executeVia()` takes a short snapshot of
/// the backend `shared_ptr` under the dedicated `_backendMtx` (never `_mtx`), so
/// it does not block `switchBackend()`.
class Bridge {
public:
    /// @brief Constructs a bridge that dispatches through @p backend.
    ///
    /// Installs a reconnect handler so backends with a recoverable transport
    /// (e.g. `QtWebSocketBackend`) can ask the bridge to re-register every live
    /// handler against the freshly reconnected peer.
    ///
    /// @par The bridge's own executor (morph#588)
    /// Every other completion in the framework is delivered on an executor its
    /// caller named — `BridgeHandler` supplies `guiExec`, `executeVia` takes a
    /// `cbExec`. The registrations the bridge issues *on its own behalf* had no
    /// such executor: their five dispatch sites name
    /// `exec::detail::inlineExecutor()`, which is "deliver wherever the backend
    /// settled" written as a value rather than as a sentence in a doc comment.
    /// @p bridgeExec is where that decision now lives.
    ///
    /// It is used for **one** thing: a registration reply that arrives after
    /// its dispatching frame has gone (`detail::deliverLate`). That is the only
    /// case with a thread to choose — a reply that settles while the dispatch
    /// call is still on the stack is parked in a
    /// `detail::AsyncDispatchHandoff` and published by the dispatching frame
    /// itself, on the dispatching thread, whatever @p bridgeExec says.
    ///
    /// The `bindModel`/`promoteModel` calls therefore keep naming
    /// `inlineExecutor()`, and that is deliberate rather than an omission.
    /// Naming a posting executor there would break two things at once:
    /// `registerHandler()` would stop being synchronous for every backend that
    /// binds inline (the settle would become a queued task, so the handler
    /// would be unbound on return), and a `kCallerMayBlock` backend would
    /// deadlock outright whenever the dispatching thread *is* the executor's
    /// thread — `detail::awaitHandoff` would sit waiting for a task only that
    /// same thread can run. A GUI embedder passing its GUI executor is exactly
    /// that case.
    ///
    /// **What the caller must guarantee, and what it buys.** @p bridgeExec must
    /// outlive this bridge and every registration still in flight when it is
    /// destroyed, because a late reply can land after `~Bridge` (the same
    /// requirement `BridgeHandler`'s `guiExec` already carries). The window
    /// morph#486 describes is closed only if @p bridgeExec runs its tasks on a
    /// thread that cannot run `~Bridge` concurrently — for a Qt embedder, the
    /// GUI thread that both owns the `Bridge` and pumps the executor. Supplying
    /// an executor on some *other* thread satisfies the type and does not close
    /// the window; it is not made worse than the default either, since the
    /// callbacks' existing `CallbackToken`/`detail::BridgeLifetime` gates are
    /// unchanged. Left null — the default — delivery is inline and behaviour is
    /// byte-for-byte what it was before morph#588.
    ///
    /// @param backend Initial backend. Ownership is transferred.
    /// @param bridgeExec Executor for late registration continuations, or null
    ///                   (the default) to keep delivering them inline.
    ///                   Borrowed: it must outlive this bridge.
    explicit Bridge(std::unique_ptr<::morph::backend::detail::IBackend> backend,
                    ::morph::exec::IExecutor* bridgeExec MORPH_LIFETIMEBOUND = nullptr)
        : _backend{std::shared_ptr<::morph::backend::detail::IBackend>(std::move(backend))}, _bridgeExec{bridgeExec} {
        installReconnectHandler(_backend);
        // A null initial backend is tolerated (see installReconnectHandler's own
        // null check just above) — there is nothing yet to stamp a session onto.
        if (_backend) {
            _backend->setSession(_defaultSession);
        }
    }

    /// @brief Cancels every still-pending completion on the active backend.
    ///
    /// Each `.onError(...)` callback receives a `BridgeDestroyedError`. In-flight
    /// server replies that arrive after destruction are no-ops because each
    /// `CompletionState::setValue`/`setException` is idempotent.
    ///
    /// Before cancelling, the active backend's reconnect handler is cleared: the
    /// installed handler captures `this`, and a co-owned backend that outlives the
    /// `Bridge` could otherwise fire it after destruction and dereference freed
    /// memory. Clearing it here (outside `_mtx` — `setReconnectHandler` only stores
    /// a callback and never re-enters the bridge) closes the common case; the
    /// handler additionally guards on `_callbacks` so a reconnect already in flight
    /// on the transport thread becomes a no-op too. See docs/spec/concurrency_and_lifetimes.md.
    ///
    /// The **first** statement retires the lifetime gate (`detail::BridgeLifetime`),
    /// and it has to stay first: everything below it — clearing the reconnect
    /// handler, cancelling pending completions, and then the implicit destruction
    /// of every member — is state a concurrently-destroyed `BridgeHandler` would
    /// otherwise still be walking. `closeLifetime()` both publishes "this bridge is
    /// retired" and **waits out** any deregistration already inside the gate, so
    /// this destructor never overlaps one. That wait is bounded and cannot
    /// deadlock: see `detail::BridgeLifetime`. Issue #486.
    ~Bridge() {
        closeLifetime();
        if (auto active = loadBackend()) {
            active->setReconnectHandler(nullptr);
            active->cancelPending(std::make_exception_ptr(::morph::backend::BridgeDestroyedError{}));
        }
    }

    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;
    Bridge(Bridge&&) = delete;
    Bridge& operator=(Bridge&&) = delete;

    /// @brief Creates and registers a new `HandlerBinding` for `Model`.
    ///
    /// Uses the default `ModelFactory::create<Model>()` factory.
    /// @tparam Model Concrete model type. Must have a registered `ModelTraits` specialisation.
    /// @return Shared pointer to the new binding.
    template <typename Model>
    std::shared_ptr<detail::HandlerBinding> registerHandler() {
        auto binding = std::make_shared<detail::HandlerBinding>();
        binding->typeId = std::string{::morph::model::ModelTraits<Model>::typeId()};
        binding->modelFactory = [] { return ::morph::model::detail::ModelFactory::create<Model>(); };
        registerHandlerImpl(binding);
        return binding;
    }

    /// @brief Registers a pre-built binding with a custom factory.
    ///
    /// Use this overload when the model factory needs to capture external
    /// dependencies that the type-erasing default factory cannot carry, or when
    /// `binding->contextKey` needs to be set before registration (see `HandlerBinding::contextKey`).
    /// @param binding Pre-constructed binding. Its `typeId` and `modelFactory` must be set.
    void registerHandler(const std::shared_ptr<detail::HandlerBinding>& binding) { registerHandlerImpl(binding); }

    /// @brief Creates a shared, initially **unattached** binding for `Model`.
    ///
    /// Unlike `registerHandler<Model>()`, this registers nothing on the backend:
    /// a shared handler has no instance until a keyed action or an explicit
    /// `attach` tells it which one it wants. The binding is tracked from the
    /// start so `switchBackend()` knows about it, but it stays unbound
    /// (`currentId == 0`) until then.
    ///
    /// @tparam Model Concrete model type. Must have a registered `ModelTraits`.
    /// @return Shared pointer to the new, unattached binding.
    template <typename Model>
    std::shared_ptr<detail::HandlerBinding> registerSharedHandler() {
        auto binding = std::make_shared<detail::HandlerBinding>();
        binding->typeId = std::string{::morph::model::ModelTraits<Model>::typeId()};
        binding->modelFactory = [] { return ::morph::model::detail::ModelFactory::create<Model>(); };
        binding->shared = true;
        std::scoped_lock const lock{_mtx};
        _handlers.push_back(binding);
        return binding;
    }

    /// @brief Attaches (or re-points) @p binding to the shared instance for @p primary.
    ///
    /// Idempotent: attaching to the primary a binding already holds is a no-op,
    /// so a keyed action repeated against the same instance costs nothing. A
    /// different primary re-points the binding — the previous instance is
    /// released and survives only if another handler still holds it.
    ///
    /// The binding's `contextKey` is set to @p primary as well, so a keyed
    /// model's journal entries carry the entity key without the caller
    /// arranging it separately.
    ///
    /// @tparam Model Concrete model type.
    /// @param binding Shared binding, as returned by `registerSharedHandler<Model>()`.
    /// @param primary Canonical string encoding of the primary key to attach to.
    template <typename Model>
    void attachHandler(const std::shared_ptr<detail::HandlerBinding>& binding, std::string primary) {
        std::scoped_lock const lock{_attachMtx};
        if (binding->primary == primary && binding->currentId.load() != 0U) {
            return;
        }
        auto const previous = ::morph::exec::detail::ModelId{binding->currentId.load()};
        // IBackend::attachModel's default (local) implementation and
        // RemoteServer's wire-level attach handling both acquire the
        // replacement before releasing `previous`, so a throwing acquire
        // leaves `previous` untouched except in one narrow, harmless case: a
        // remote re-point whose connection scope closes concurrently with
        // the attach, where `previous` may already be released by the time
        // the reply reports failure -- but no further request on this
        // binding will ever run at that point either, so nothing is actually
        // lost from the caller's perspective. Unbind first is therefore
        // unnecessary: publish only on success.
        auto newId = loadBackend()->attachModel(binding->typeId, binding->modelFactory,
                                                {.contextKey = primary, .primary = primary}, previous);
        binding->contextKey = primary;
        binding->primary = std::move(primary);
        binding->currentId.store(newId.v);
    }

    /// @brief Async counterpart to `attachHandler`: dispatches the attach and
    ///        invokes @p onDone once attached (or failed), instead of blocking.
    ///
    /// Reaches the backend through `IBackend::bindModel` — the structural
    /// registration surface, and since morph#571 the only one. There is
    /// exactly one dispatch and exactly one continuation: a
    /// backend with no non-blocking attach settles inside the `bindModel` call,
    /// having blocked for the same round trip the synchronous `attachHandler`
    /// would have, and @p onDone is then invoked from this thread before this
    /// method returns — so a caller that always goes through this method
    /// behaves identically to calling `attachHandler` directly on such a
    /// backend.
    ///
    /// @par Locking
    /// `_attachMtx` is held around the guard check and the *dispatch* — matching
    /// `attachHandler`'s existing lock scope — but is **released before
    /// @p onDone is ever invoked**, on every path, unconditionally. That is not
    /// a nicety: what `execute()` does from inside @p onDone is dispatch the
    /// action, and a result-keyed dispatch promotes its binding through
    /// `assignHandlerPrimary`, which takes `_attachMtx` itself. Invoking
    /// @p onDone under the lock therefore self-deadlocks the moment the
    /// completion is delivered on the calling thread — which is exactly what a
    /// blocking backend's `bindModel` does, since the executor this call site
    /// names runs the continuation inline. This is `registerHandlerImpl`'s
    /// existing rule ("the backend call must not run under `_mtx`") applied to
    /// `_attachMtx`.
    ///
    /// The guarantee holds even for a backend that settles *inline*, from inside
    /// the dispatch call itself, while this frame still holds the lock: such a
    /// continuation parks its outcome in a `detail::AsyncDispatchHandoff` and
    /// returns without acting, and this frame applies it after the dispatch call
    /// has returned and the lock is gone. See that struct's doc comment.
    ///
    /// An out-of-frame success callback re-acquires `_attachMtx` for the two
    /// `std::string` fields it publishes (`contextKey`/`primary`, which
    /// `HandlerBinding` documents as readable only under that lock) and drops it
    /// again before calling @p onDone. That re-acquisition is safe precisely
    /// because the inline case never reaches it.
    ///
    /// @par Known gap
    /// Two calls for the same key issued before the first one's reply arrives
    /// are **not** deduplicated: the guard below reads `binding->primary`/
    /// `currentId`, neither of which is updated until the reply lands, so both
    /// calls pass it and both dispatch an `attach`. This is a real behaviour
    /// difference from the synchronous `attachHandler` it replaces, not merely
    /// something inherent to asynchrony — `attachHandler` held `_attachMtx`
    /// across the whole blocking round trip, which serialised concurrent
    /// callers for free. It needs no second thread to hit: two `execute()`
    /// calls in one event-loop turn are enough. The server answers both with
    /// the same `ModelId` but counts two attachments, so one attach reference
    /// leaks; the leak is bounded, not unbounded — the connection scope
    /// releases every reference it holds when it closes. Closing this properly
    /// needs in-flight tracking on the binding (coalescing the second caller
    /// onto the first dispatch's completion); tracked as a follow-up, not fixed
    /// here. Same gap, same reasoning, on `ensureBoundAsync`.
    ///
    /// @tparam Model Concrete model type.
    /// @param binding Shared binding, as returned by `registerSharedHandler<Model>()`.
    /// @param primary Canonical string encoding of the primary key to attach to.
    /// @param onDone  Invoked with `nullptr` on success, or a non-null
    ///                `exception_ptr` on failure — always exactly once,
    ///                synchronously if the fallback path is taken.
    template <typename Model>
    void attachHandlerAsync(const std::shared_ptr<detail::HandlerBinding>& binding, std::string primary,
                            const std::function<void(std::exception_ptr)>& onDone) {
        std::unique_lock lock{_attachMtx};
        if (binding->primary == primary && binding->currentId.load() != 0U) {
            lock.unlock();
            onDone(nullptr);
            return;
        }
        auto const previous = ::morph::exec::detail::ModelId{binding->currentId.load()};
        auto backend = loadBackend();
        auto primaryCopy = std::move(primary);
        std::weak_ptr<::morph::backend::detail::IBackend> const weakBackend{backend};
        auto const weakLiveness = _callbacks.token();
        std::weak_ptr<detail::HandlerBinding> const weakBinding{binding};
        auto handoff = std::make_shared<detail::AsyncDispatchHandoff>();
        auto* const bridgeExec = _bridgeExec;
        auto onAttached = [this, weakBackend, weakLiveness, weakBinding, primaryCopy, onDone, handoff,
                           bridgeExec](::morph::exec::detail::ModelId newId) mutable {
            if (detail::parkIfInFrame(*handoff, true, newId, nullptr)) {
                return;  // Completed inline: the dispatching frame will finish this.
            }
            // Nobody is left on the dispatching stack, so this callback
            // publishes the outcome itself -- on `bridgeExec` if the embedder
            // named one, inline otherwise. See `detail::deliverLate`.
            //
            // `mutable`, so `primaryCopy` is *moved* into the body's closure
            // rather than copied: this callback runs exactly once (one
            // `Completion`, settled once), and copying the primary key here
            // would put a fresh allocation on a path that had none before
            // morph#588 -- which is both a cost and, for
            // `tests/test_async_registration.cpp`'s morph#108 case, the wrong
            // allocation for its injector to catch. `onDone` cannot be moved
            // the same way: it is captured from a `const` reference parameter,
            // so the capture itself is const.
            detail::deliverLate(bridgeExec, [this, weakBackend, weakLiveness, weakBinding,
                                             primaryCopy = std::move(primaryCopy), onDone, newId] {
                // This check and the `this` touch below it are two steps --
                // the morph#486 shape. Closed not by a gate but by the thread
                // this body runs on. Before morph#571 that was a prose
                // contract on every backend author; morph#568 made it the
                // executor the dispatch site names, which is
                // `inlineExecutor()` and so left the window unchanged;
                // morph#588 moved the choice here, where a non-null
                // `bridgeExec` running `~Bridge`'s own thread closes it, and
                // the null default keeps the pre-morph#588 thread exactly.
                // Gating instead would block `~Bridge` behind `_attachMtx`,
                // which `attachHandler` holds across a full `attachModel`
                // round trip. See morph#489.
                if (!weakLiveness.active()) {
                    return;  // The Bridge is gone; publishing this id would be pointless.
                }
                auto strongBinding = weakBinding.lock();
                if (!strongBinding) {
                    return;  // The BridgeHandler (and its binding) is gone.
                }
                std::exception_ptr failure;
                {
                    // contextKey/primary are plain std::strings that every
                    // other site reads under `_attachMtx`; publishing them
                    // without it would be a data race, not just a stale
                    // read. (`registerHandlerImpl`'s read during
                    // registration is the one documented carve-out -- see
                    // its own comment, and morph#505.)
                    std::scoped_lock const guard{_attachMtx};
                    auto pinned = weakBackend.lock();
                    if (!pinned || pinned != loadBackend()) {
                        // A switchBackend() already moved past this attach
                        // (see registerHandlerImpl's identical guard) and
                        // its own re-registration loop already handled
                        // `binding` on the *new* backend -- applying this
                        // stale reply now would overwrite that with a
                        // dangling id from a backend nothing uses any
                        // more. Unlike registerHandlerImpl's fire-and-
                        // forget re-registration, a real execute() call is
                        // synchronously waiting on `onDone` here, so the
                        // stale reply must still be reported -- silently
                        // dropping it would hang that caller forever.
                        failure = std::make_exception_ptr(std::runtime_error(
                            "attach reply arrived from a backend switchBackend() already replaced"));
                    } else {
                        try {
                            strongBinding->contextKey = primaryCopy;
                            strongBinding->primary = primaryCopy;
                            strongBinding->currentId.store(newId.v);
                        } catch (...) {
                            failure = std::current_exception();
                        }
                    }
                }
                onDone(failure);  // Outside the lock -- see @par Locking.
            });
        };
        auto onFailed = [onDone, handoff, bridgeExec](const std::exception_ptr& failure) {
            if (detail::parkIfInFrame(*handoff, false, {}, failure)) {
                return;
            }
            // Reported on the bridge's executor for the same reason the
            // success path is: `onDone` is the caller's continuation, and a
            // bridge that named an executor wants it delivered there.
            detail::deliverLate(bridgeExec, [onDone, failure] { onDone(failure); });
        };
        try {
            // The structural surface (`IBackend::bindModel`). A backend with a
            // genuinely non-blocking attach settles the returned `Completion`
            // when its reply lands; one without settles it from inside this
            // call, having blocked exactly as the synchronous `attachModel`
            // this replaces did. Either way the continuation exists, so there
            // is no second path here.
            //
            // `inlineExecutor()` because that is where the continuation ran
            // before morph#568: on whichever thread the backend settled the
            // reply on, which the removed `*Async` twins could only ask for in
            // prose. What is new is that this call site *names* it; see
            // `exec::detail::InlineExecutor`. An inline settle therefore
            // reaches `onAttached`/`onFailed` while `_attachMtx` is still
            // held, which is precisely the case `handoff` exists for.
            auto completion =
                backend->bindModel(::morph::backend::detail::BindRequest{.typeId = binding->typeId,
                                                                         .factory = binding->modelFactory,
                                                                         .contextKey = primaryCopy,
                                                                         .primary = primaryCopy,
                                                                         .current = previous},
                                   ::morph::exec::detail::inlineExecutor());
            completion.then(onAttached).onError(onFailed);
        } catch (...) {
            // `IBackend::bindModel`'s own default converts a throwing
            // synchronous verb into a rejection, and every backend in the tree
            // does the same. This guard is for an out-of-tree override that
            // throws out of the dispatch call itself (e.g. a `wire::encode()`
            // failing before send): report it like any other failure rather
            // than let it escape execute()'s documented never-throws contract.
            lock.unlock();
            onDone(std::current_exception());
            return;
        }
        if (auto parked = detail::claimHandoff(*handoff)) {
            // The backend answered on this very stack, with `_attachMtx` still
            // held -- switchBackend() cannot have run concurrently (it takes
            // the same lock), so no staleness check is needed here. Publish
            // under the lock we already own, then release it and report --
            // @p onDone never runs inside the dispatch frame.
            std::exception_ptr failure = parked->failure;
            if (parked->succeeded) {
                try {
                    binding->contextKey = primaryCopy;
                    binding->primary = std::move(primaryCopy);
                    binding->currentId.store(parked->modelId.v);
                } catch (...) {
                    failure = std::current_exception();
                }
            }
            lock.unlock();
            onDone(failure);
            return;
        }
        // Nothing parked: the reply is still to come, and `onAttached`/
        // `onFailed` will deliver it themselves once it does.
    }

    /// @brief Gives @p binding an anonymous instance if it does not have one yet.
    ///
    /// Used before a result-keyed action: such an action generates the key it
    /// will be filed under, so it has to run *somewhere* first. The instance it
    /// gets is private (empty primary, invisible to the directory) until
    /// `assignHandlerPrimary` promotes it in place once the key is known.
    /// @param binding Shared binding to bind.
    void ensureBound(const std::shared_ptr<detail::HandlerBinding>& binding) {
        std::scoped_lock const lock{_attachMtx};
        if (binding->currentId.load() != 0U) {
            return;
        }
        auto newId = loadBackend()->registerModelShared(binding->typeId, binding->modelFactory,
                                                        {.contextKey = binding->contextKey, .primary = {}});
        binding->currentId.store(newId.v);
    }

    /// @brief Async counterpart to `ensureBound`. See `attachHandlerAsync`'s
    ///        doc comment for the fallback and locking contract, including the
    ///        inline-completion handling and the in-flight dedup gap, both of
    ///        which apply here identically (two result-keyed `execute()` calls
    ///        on the same still-unbound handler each bind their own anonymous
    ///        instance; the first is then stranded until the connection scope
    ///        closes).
    ///
    /// The one difference: this method's success callback publishes only
    /// `currentId`, which is a `std::atomic`, so — unlike `attachHandlerAsync`'s
    /// — it needs no `_attachMtx` of its own to do it.
    /// @param binding Shared binding to bind.
    /// @param onDone  Invoked exactly once: `nullptr` on success, or a
    ///                non-null `exception_ptr` on failure.
    void ensureBoundAsync(const std::shared_ptr<detail::HandlerBinding>& binding,
                          const std::function<void(std::exception_ptr)>& onDone) {
        std::unique_lock lock{_attachMtx};
        if (binding->currentId.load() != 0U) {
            lock.unlock();
            onDone(nullptr);
            return;
        }
        auto backend = loadBackend();
        std::weak_ptr<::morph::backend::detail::IBackend> const weakBackend{backend};
        auto const weakLiveness = _callbacks.token();
        std::weak_ptr<detail::HandlerBinding> const weakBinding{binding};
        auto handoff = std::make_shared<detail::AsyncDispatchHandoff>();
        auto* const bridgeExec = _bridgeExec;
        auto onBound = [this, weakBackend, weakLiveness, weakBinding, onDone, handoff,
                        bridgeExec](::morph::exec::detail::ModelId newId) {
            if (detail::parkIfInFrame(*handoff, true, newId, nullptr)) {
                return;  // Completed inline: the dispatching frame will finish this.
            }
            // Late: published by this callback, on the bridge's executor when
            // it has one. `mutable` plus moved captures for the reason
            // `attachHandlerAsync`'s identical shape spells out. See
            // `detail::deliverLate`.
            detail::deliverLate(bridgeExec, [this, weakBackend, weakLiveness, weakBinding, onDone, newId] {
                // This check and the `this` touch below it are two steps --
                // the morph#486 shape. Closed not by a gate but by the thread
                // this body runs on, which morph#588 made the bridge's own
                // choice; the null default is the pre-morph#588 thread,
                // whichever one the backend settled on, so the window is
                // unchanged there rather than closed. Gating instead would
                // block `~Bridge` behind `_attachMtx`, which `attachHandler`
                // holds across a full `attachModel` round trip. See morph#489.
                if (!weakLiveness.active()) {
                    return;  // The Bridge is gone; publishing this id would be pointless.
                }
                auto strongBinding = weakBinding.lock();
                if (!strongBinding) {
                    return;  // The BridgeHandler (and its binding) is gone.
                }
                std::exception_ptr failure;
                {
                    // Brief `_attachMtx` window purely to serialise this
                    // check against a concurrent switchBackend() (which
                    // takes the same lock) -- `currentId` itself is an
                    // atomic and needs no lock to store.
                    std::scoped_lock const guard{_attachMtx};
                    auto pinned = weakBackend.lock();
                    if (!pinned || pinned != loadBackend()) {
                        // See attachHandlerAsync's identical guard: a
                        // stale reply from a backend switchBackend()
                        // already replaced must still resolve `onDone`
                        // (a real execute() call is waiting), not be
                        // silently dropped.
                        failure = std::make_exception_ptr(std::runtime_error(
                            "attach reply arrived from a backend switchBackend() already replaced"));
                    } else {
                        strongBinding->currentId.store(newId.v);
                    }
                }
                onDone(failure);
            });
        };
        auto onFailed = [onDone, handoff, bridgeExec](const std::exception_ptr& failure) {
            if (detail::parkIfInFrame(*handoff, false, {}, failure)) {
                return;
            }
            detail::deliverLate(bridgeExec, [onDone, failure] { onDone(failure); });
        };
        try {
            // The structural surface; see `attachHandlerAsync`'s identical
            // dispatch for why `inlineExecutor()` is the executor this call
            // site names. An empty `primary` with a zero `current` is the
            // request shape that means `registerModelShared` -- the very
            // verb the synchronous `ensureBound` calls.
            auto completion =
                backend->bindModel(::morph::backend::detail::BindRequest{.typeId = binding->typeId,
                                                                         .factory = binding->modelFactory,
                                                                         .contextKey = binding->contextKey,
                                                                         .primary = {},
                                                                         .current = {}},
                                   ::morph::exec::detail::inlineExecutor());
            completion.then(onBound).onError(onFailed);
        } catch (...) {
            // See attachHandlerAsync's identical guard: an out-of-tree
            // `bindModel` override can throw out of the dispatch call itself.
            lock.unlock();
            onDone(std::current_exception());
            return;
        }
        if (auto parked = detail::claimHandoff(*handoff)) {
            // Completed on this stack, under `_attachMtx`: publish here, then
            // release the lock before reporting (see `attachHandlerAsync`).
            if (parked->succeeded) {
                binding->currentId.store(parked->modelId.v);
            }
            lock.unlock();
            onDone(parked->failure);
            return;
        }
        // Nothing parked: the reply is still to come, and `onBound`/`onFailed`
        // will deliver it themselves once it does.
    }

    /// @brief Files @p binding's current instance under @p primary, in place.
    ///
    /// The instance keeps everything the creating action just did — nothing is
    /// re-created and nothing is stranded. A no-op if @p binding already holds
    /// a different real primary (the locally cached primary must not race
    /// ahead of the backend's own refusal to re-key an already-keyed
    /// instance — see `IBackend::assignPrimary`).
    ///
    /// Known gap: if @p binding was still anonymous but the *target* key is
    /// already held by a different instance, the backend's `assignPrimary`
    /// silently declines to promote (the existing holder always wins), but
    /// this method has no way to learn that and still caches @p primary as
    /// though the promotion succeeded — `binding->primary()` can then report
    /// a key the backend never actually filed this instance under, and a
    /// same-key `attach()` after that becomes a silent no-op (the "already
    /// primary == primary" guard in `attachHandler`), so the binding can
    /// never reach the instance actually holding that key. Closing this
    /// requires `assignPrimary`'s outcome to become observable (e.g. a
    /// `bool` return threaded across every `IBackend` implementation and, for
    /// wire backends, a reply field) — tracked as a follow-up, not fixed
    /// here.
    /// Reaches the backend through `IBackend::promoteModel` — the structural
    /// registration surface, and since morph#571 the only one.
    /// The same "avoid a nested-event-loop block that aborts a WASM
    /// main thread" rationale `Bridge::registerHandler()` follows for the
    /// initial bind step applies here: this method is invoked from inside the
    /// result `Completion`'s callback chain (`BridgeHandler::execute`'s
    /// `onResult`), not from the original call stack, so there is no caller
    /// left blocked waiting on it either way — a non-blocking promote simply
    /// avoids parking the Qt event loop for the round trip.
    /// `binding->contextKey`/`primary` are only published once the reply
    /// confirms the call, so a caller reading `binding->primary()` never sees a
    /// promotion the backend has not actually completed. A backend with no
    /// non-blocking promote settles inside the `promoteModel` call, publishing
    /// before this method returns; a failure is logged rather than thrown,
    /// since `promoteModel` reports through its `Completion` by contract.
    ///
    /// `_attachMtx` is released *before* the dispatch — not held across it —
    /// mirroring `registerHandlerImpl`'s discipline: the success callback
    /// below re-acquires
    /// `_attachMtx`, so a backend that ever invoked it synchronously (none
    /// documented here do, but nothing prevents one from doing so) would
    /// otherwise self-deadlock re-acquiring a mutex this same call stack
    /// still held.
    /// @tparam Model Concrete model type.
    /// @param binding Shared binding whose instance is being promoted.
    /// @param primary Canonical string encoding of the key to file it under.
    template <typename Model>
    void assignHandlerPrimary(const std::shared_ptr<detail::HandlerBinding>& binding, std::string primary) {
        std::shared_ptr<::morph::backend::detail::IBackend> backend;
        uint64_t raw = 0;
        {
            std::scoped_lock const lock{_attachMtx};
            raw = binding->currentId.load();
            if (raw == 0U || primary.empty() || !binding->primary.empty()) {
                return;
            }
            backend = loadBackend();
        }
        auto const weakLiveness = _callbacks.token();
        std::weak_ptr<::morph::backend::detail::IBackend> const weakBackend{backend};
        std::weak_ptr<detail::HandlerBinding> const weakBinding{binding};
        auto onPromoted = [this, weakLiveness, weakBackend, weakBinding, primary] {
            // This check and the `this` touch below it are two steps -- the
            // morph#486 shape. Closed not by a gate but by the thread this
            // body runs on. Before morph#571 that thread was the backend's
            // choice, asked for in prose; morph#568 made it the executor the
            // `promoteModel` call names, which is `inlineExecutor()` and so
            // left the window unchanged; morph#588 moved the choice to the
            // bridge's own executor for a reply that arrives after this
            // frame, and left the in-frame reply published by this frame.
            // Gating instead would block `~Bridge` behind `_attachMtx`, which
            // `attachHandler` holds across a full `attachModel` round trip.
            // See morph#489.
            if (!weakLiveness.active()) {
                return;  // The Bridge is gone; do not touch `this`.
            }
            auto strongBinding = weakBinding.lock();
            if (!strongBinding) {
                return;  // The BridgeHandler (and its binding) is gone.
            }
            std::scoped_lock const attachLock{_attachMtx};
            auto pinned = weakBackend.lock();
            if (!pinned || pinned != loadBackend()) {
                // A switchBackend() already moved past this promotion;
                // applying a stale reply now would overwrite whatever
                // state the new backend's re-registration already
                // established -- same reasoning as registerHandlerImpl's
                // async callback.
                return;
            }
            // A binding whose primary is already set (by a concurrent
            // attach/assign that raced ahead of this async reply, or
            // simply already promoted) must not be overwritten here.
            if (!strongBinding->primary.empty()) {
                return;
            }
            strongBinding->contextKey = primary;
            strongBinding->primary = primary;
        };
        auto onFailed = [typeId = binding->typeId](const std::string& message) {
            ::morph::log::logError("[assignHandlerPrimary] async promotion of '" + typeId + "' failed: " + message);
        };
        // The structural surface (`IBackend::promoteModel`). A backend with
        // no non-blocking promote settles the returned `Completion` from
        // inside this call, having run the same synchronous `assignPrimary`
        // this replaces; `inlineExecutor()` then delivers the reply on this
        // thread, and the `claimHandoff` below publishes it before this
        // method returns -- exactly where the synchronous call used to
        // publish, which `BridgeHandler::execute`'s `onResult` relies on ("the
        // binding is already promoted by the time user code sees the result").
        // Unlike that call, a failure is logged rather than thrown: this runs
        // inside the result `Completion`'s callback chain, where an escaping
        // exception is swallowed by `CompletionState` anyway, and
        // `promoteModel` reports through the `Completion` by contract.
        //
        // The handoff is what separates the two cases morph#588 treats
        // differently, and is why this site has one at all: a reply that
        // lands in this frame is published by this frame, while one that
        // lands later goes to the bridge's executor. Without it the callback
        // could not tell the two apart, and posting *both* would make an
        // inline promote asynchronous.
        auto handoff = std::make_shared<detail::AsyncDispatchHandoff>();
        auto* const bridgeExec = _bridgeExec;
        auto completion = backend->promoteModel(
            ::morph::backend::detail::PromoteRequest{
                .mid = ::morph::exec::detail::ModelId{raw}, .typeId = binding->typeId, .primary = primary},
            ::morph::exec::detail::inlineExecutor());
        completion
            .then([onPromoted, handoff, bridgeExec](::morph::exec::detail::ModelId newId) {
                if (detail::parkIfInFrame(*handoff, true, newId, nullptr)) {
                    return;  // Settled inline: the dispatching frame owns the outcome.
                }
                detail::deliverLate(bridgeExec, onPromoted);
            })
            .onError([onFailed, handoff, bridgeExec](const std::exception_ptr& failure) mutable {
                if (detail::parkIfInFrame(*handoff, false, {}, failure)) {
                    return;
                }
                detail::deliverLate(bridgeExec, [onFailed = std::move(onFailed), failure] {
                    onFailed(detail::describeFailure(failure));
                });
            });
        if (auto parked = detail::claimHandoff(*handoff)) {
            if (parked->succeeded) {
                onPromoted();
            } else {
                onFailed(detail::describeFailure(parked->failure));
            }
        }
    }

    /// @brief Returns @p binding's current primary key, or empty if unattached.
    /// @param binding Binding to inspect.
    /// @return Canonical key string, or an empty string when unattached.
    [[nodiscard]] std::string bindingPrimary(const std::shared_ptr<detail::HandlerBinding>& binding) {
        std::scoped_lock const lock{_attachMtx};
        return binding->primary;
    }

    /// @brief Whether @p binding currently has a live `ModelId`.
    ///
    /// A binding constructed against a backend that answers
    /// `BindWait::kCallerMustNotBlock` (see `backend.md`, "Waiting for a
    /// bind") starts unbound and becomes
    /// bound only once `onRegistered` fires — this is the synchronous,
    /// point-in-time check; `whenBound()` below is the awaitable counterpart.
    /// @param binding Binding to inspect.
    /// @return `true` if `currentId != 0`.
    [[nodiscard]] static bool isBound(const std::shared_ptr<detail::HandlerBinding>& binding) noexcept {
        return binding->currentId.load() != 0U;
    }

    /// @brief Resolves once @p binding's initial registration settles.
    ///
    /// Closes the gap `executeVia`'s fast-fail leaves for a caller that
    /// constructs a `BridgeHandler` against a backend that answers
    /// `BindWait::kCallerMustNotBlock` (see `backend.md`, "Waiting for a
    /// bind") and wants to dispatch the
    /// moment registration completes, rather than failing fast with "handler
    /// not bound" or polling `isBound()` in a loop of its own devising.
    ///
    /// - If @p binding is already bound, resolves immediately with `true`.
    /// - If an async registration is still in flight, resolves with `true`
    ///   once `onRegistered` fires, or with the registration's error via
    ///   `.onError(...)` if it fails.
    /// - If @p binding was never handed an async registration at all (the
    ///   synchronous fallback path, or a binding still awaiting its very
    ///   first `registerHandlerImpl` call to even attempt one) and is not yet
    ///   bound, resolves immediately with `false` — there is nothing in
    ///   flight to wait for. This can only happen for a `shared` binding that
    ///   has not yet been given a primary (`ensureBound`/`attachHandler`
    ///   never ran) or in the narrow window before `registerHandlerImpl`
    ///   itself has run; ordinary (non-shared) bindings are always either
    ///   bound or mid-registration by the time a caller can observe them.
    /// @param binding Binding to wait on.
    /// @param cbExec  Executor the resolution is delivered on, exactly like
    ///                every other `Completion`-returning call in this class.
    /// @return `Completion<bool>` resolving `true` once bound, `false` if
    ///         nothing is in flight, or an error if registration failed.
    [[nodiscard]] ::morph::async::Completion<bool> whenBound(const std::shared_ptr<detail::HandlerBinding>& binding,
                                                             ::morph::exec::IExecutor* cbExec) {
        auto state = std::make_shared<::morph::async::detail::CompletionState<bool>>();
        ::morph::async::Completion<bool> comp{state, cbExec};
        if (isBound(binding)) {
            state->setValue(true);
            return comp;
        }
        std::scoped_lock const lock{binding->registrationMtx};
        // Re-check under the lock: registerHandlerImpl's callback may have
        // resolved (and bound) between the lock-free check above and here.
        if (isBound(binding)) {
            state->setValue(true);
            return comp;
        }
        if (!binding->registrationInFlight) {
            // Nothing to wait for: no async registration was ever started for
            // this binding (synchronous fallback already ran to completion,
            // or a shared binding with no primary yet), and it is still
            // unbound. Resolve false rather than hang forever.
            state->setValue(false);
            return comp;
        }
        binding->registrationWaiters.emplace_back([state](bool ok) { state->setValue(ok); },
                                                  [state](std::exception_ptr err) { state->setException(err); });
        return comp;
    }

    /// @brief Lists the live shared primary keys of `Model` on the active backend.
    /// @tparam Model Concrete model type.
    /// @return Canonical key strings, in unspecified order.
    template <typename Model>
    [[nodiscard]] std::vector<std::string> listInstancesOf() {
        return loadBackend()->listInstances(std::string{::morph::model::ModelTraits<Model>::typeId()});
    }

    /// @brief Registers a result-type subscription for @p binding.
    ///
    /// The subscription is stored against the *binding*, not against a fixed
    /// instance id, and is matched at publish time by comparing the binding's
    /// current instance. Re-pointing a handler therefore moves its subscriptions
    /// with it, which is what makes "tell me about the account I am looking at"
    /// keep working when the user switches accounts.
    ///
    /// Forwards to `_subscriptions` (`detail::SubscriptionRegistry`); see that
    /// class for the implementation.
    /// @param binding Handler binding that owns the subscription.
    /// @param type    Result type being subscribed to.
    /// @param sink    Type-erased delivery callback; receives the boxed result.
    /// @param exec    Executor the callback is delivered on.
    void addSubscription(const std::shared_ptr<detail::HandlerBinding>& binding, std::type_index type,
                         std::function<void(const std::any&)> sink, ::morph::exec::IExecutor* exec) {
        _subscriptions->addSubscription(binding, type, std::move(sink), exec);
    }

    /// @brief Removes @p binding's subscription for @p type, if any.
    ///
    /// Forwards to `_subscriptions` (`detail::SubscriptionRegistry`).
    /// @param binding Handler binding that owns the subscription.
    /// @param type    Result type to stop hearing about.
    void removeSubscription(const std::shared_ptr<detail::HandlerBinding>& binding, std::type_index type) {
        _subscriptions->removeSubscription(binding, type);
    }

    /// @brief Whether any subscription is currently registered on this bridge.
    ///
    /// A single relaxed atomic load, so the overwhelmingly common case — a
    /// process with no subscribers at all — pays nothing per result. Without
    /// this, every successful action would build a `std::type_index`, copy its
    /// result into a `std::any`, take a lock and walk the (empty) subscription
    /// list before its `Completion` could resolve: a throughput regression for
    /// every existing caller, on the hot path, to serve a feature they are not
    /// using.
    ///
    /// Forwards to `_subscriptions` (`detail::SubscriptionRegistry`).
    /// @return `true` if at least one subscription exists.
    [[nodiscard]] bool hasSubscribers() const noexcept { return _subscriptions->hasSubscribers(); }

    /// @brief Delivers @p value to every subscriber attached to instance @p mid.
    ///
    /// Called for every successful action result. Subscribers are matched on
    /// *the instance the result was produced on*, so a handler hears about work
    /// another handler — or, with a shared instance, another screen entirely —
    /// did on the model it is attached to.
    ///
    /// The producing handler is notified too: suppressing the echo would force
    /// every subscriber to special-case "was this mine", which is exactly the
    /// bookkeeping the feature exists to remove.
    ///
    /// Forwards to `_subscriptions` (`detail::SubscriptionRegistry`), which
    /// snapshots matching sinks under its lock and invokes them outside it, so
    /// a subscriber that re-enters the bridge cannot deadlock.
    ///
    /// @param mid   Instance the result was produced on.
    /// @param type  Result type produced.
    /// @param value Boxed result.
    void publishResult(::morph::exec::detail::ModelId mid, std::type_index type, const std::any& value) {
        _subscriptions->publishResult(mid, type, value);
    }

    /// @brief Installs a default session context that `executeVia` stamps onto the
    ///        `ActionCall` of every subsequent call.
    ///
    /// There is no per-call session override — this default is applied to all calls
    /// until replaced or cleared.
    ///
    /// Typical pattern: call this once after login to bind the user's principal
    /// and locale, then every subsequent `handler.execute(action)` carries the
    /// session automatically. Thread-safe.
    ///
    /// @param session The new default. Pass `{}` to clear.
    void setDefaultSession(::morph::session::Context session) {
        std::shared_ptr<::morph::backend::detail::IBackend> backend;
        {
            std::scoped_lock const lock{_sessionMtx};
            _defaultSession = session;
            backend = loadBackend();
        }
        // Pushed to the backend outside _sessionMtx: IBackend::setSession's
        // default implementation just stores a copy, but a concrete override
        // must not run under this bridge's own lock. Mirrors defaultSession()'s
        // copy-out-then-release pattern.
        if (backend) {
            backend->setSession(std::move(session));
        }
    }

    /// @brief Sets (or disables) the client-side execute deadline.
    ///
    /// Every `executeVia()` call after this point races the real reply against
    /// @p deadline; whichever settles first wins (`CompletionState::setValue`/
    /// `setException` are idempotent — see `completion.hpp`). If @p deadline
    /// elapses first, the pending `Completion` fails with
    /// `::morph::backend::ClientTimeoutError`; the real reply, if it arrives
    /// later, is silently discarded exactly like any other late write to an
    /// already-resolved `CompletionState`.
    ///
    /// The clock starts inside `executeVia()`, immediately before the backend's
    /// own `execute()` is dispatched, so @p deadline covers the whole round trip
    /// — serialisation, transport, server-side work, and the reply's journey
    /// back — not just the time spent waiting after dispatch.
    ///
    /// Disabled (`std::chrono::milliseconds{0}`, the default): a dropped
    /// frame or a hung server leaves the `Completion` pending forever.
    ///
    /// The backing `TimeoutScheduler` (and its one background thread) is
    /// created lazily on the first call that enables a deadline, so a `Bridge`
    /// that never opts in spawns no extra thread. Once created it lives until
    /// `~Bridge()`; setting the deadline back to `0` stops new calls from
    /// arming it but does not tear the thread down. Thread-safe.
    ///
    /// @param deadline Maximum time to wait for any reply. `0` disables the
    ///                 deadline.
    void setExecuteDeadline(std::chrono::milliseconds deadline) {
        std::scoped_lock const lock{_executeDeadlineMtx};
        _executeDeadline = deadline;
        if (_executeDeadline.count() > 0 && !_timeoutScheduler) {
            _timeoutScheduler = std::make_shared<::morph::async::detail::TimeoutScheduler>();
        }
    }

    /// @brief Returns the currently installed client-side execute deadline.
    /// @return The deadline; `std::chrono::milliseconds{0}` when disabled.
    [[nodiscard]] std::chrono::milliseconds executeDeadline() const {
        std::scoped_lock const lock{_executeDeadlineMtx};
        return _executeDeadline;
    }

    /// @brief Returns a copy of the currently installed default session. Thread-safe.
    /// @return Snapshot of the default `Context`.
    [[nodiscard]] ::morph::session::Context defaultSession() const {
        std::scoped_lock const lock{_sessionMtx};
        return _defaultSession;
    }

    /// @brief Installs the verified `Principal` for this `Bridge`. Thread-safe.
    ///
    /// Typically called once right after a successful login dispatch, from
    /// data the server actually returned (see `session::Principal`'s doc
    /// comment on the trust model). Distinct from `setDefaultSession`: that
    /// installs the per-call `Context` forwarded with every dispatch; this
    /// installs the longer-lived identity UI code reads *outside* a dispatch
    /// via `currentPrincipal()` to shape itself. Guarded by its own mutex
    /// (`_principalMtx`, not `_sessionMtx`) since it is read far more
    /// frequently, by UI code, than the per-call session snapshot.
    /// @param principal Verified identity to install. Pass a
    ///        default-constructed `Principal{}` (or call this from a sign-out
    ///        handler) to clear it.
    void setPrincipal(::morph::session::Principal principal) {
        std::scoped_lock const lock{_principalMtx};
        _principal = std::move(principal);
    }

    /// @brief Returns a copy of the currently installed `Principal`. Thread-safe.
    ///
    /// Default-constructed (empty `id`, no `roles`) if `setPrincipal` was
    /// never called or the application signed out by clearing it.
    /// @return Snapshot of the installed `Principal`.
    [[nodiscard]] ::morph::session::Principal currentPrincipal() const {
        std::scoped_lock const lock{_principalMtx};
        return _principal;
    }

    /// @brief Returns the number of actions dispatched via `executeVia()` that
    ///        have not yet resolved.
    ///
    /// Incremented once per call, right before the backend dispatch inside
    /// `executeVia()`; decremented exactly once when the returned `Completion`
    /// settles on success or on error (including a cancellation error from
    /// `switchBackend()`/`~Bridge()`/a dropped transport). The synchronous
    /// "handler not bound" early return — the one case `executeVia()` resolves
    /// before ever dispatching — is never counted in the first place, so it
    /// needs no matching decrement; it never nudges this counter either way. A
    /// client can poll this (or watch it drop to zero) to build a "still
    /// loading" indicator or gate a feature on quiescence, without
    /// hand-rolling its own counter around every `execute()` call site. A
    /// single relaxed atomic load/increment/decrement: cheap enough to read on
    /// every frame of a UI loading spinner.
    ///
    /// @return Count of actions dispatched but not yet resolved.
    [[nodiscard]] std::size_t pendingCalls() const noexcept { return _pendingCalls->load(std::memory_order_relaxed); }

    /// @brief Atomically replaces the active backend with @p newBackend.
    ///
    /// All live bindings are re-registered on the new backend and their
    /// `currentId` values are updated. The old backend is released after the
    /// swap. Any in-flight `Completion` objects targeting the old backend will
    /// fail naturally.
    ///
    /// @note Lock ordering: `Bridge::_mtx` is acquired before
    ///       `LocalBackend::_regMtx`. `notifyBackendChanged()` runs under `_mtx`
    ///       but only **posts** each model's `onBackendChanged()` onto that
    ///       model's strand (see `LocalBackend::notifyBackendChanged`); the model
    ///       body runs later on a pool thread, off `_mtx`. So an
    ///       `onBackendChanged()` implementation **may** re-enter the bridge
    ///       (`switchBackend`/`registerHandler`/`deregisterHandler`) — it acquires
    ///       `_mtx` freshly on the strand thread rather than deadlocking on the
    ///       switch caller's still-held lock. It is also strand-serialised against
    ///       `execute` on the same model, so it needs no locking of model state.
    ///
    /// @note Templated on the concrete @p Backend (rather than taking
    ///       `unique_ptr<IBackend>` directly) so it is an exact match for a
    ///       `unique_ptr<Concrete>` argument (e.g. `std::make_unique<LocalBackend>(...)`)
    ///       and is preferred over the `shared_ptr<IBackend>` overload during overload
    ///       resolution — both would otherwise require an equally-ranked user-defined
    ///       conversion (unique_ptr<Concrete> -> unique_ptr<IBackend> vs. unique_ptr<Concrete>
    ///       -> shared_ptr<IBackend>), making every existing call site ambiguous.
    /// @tparam Backend Concrete backend type; must derive from `IBackend`.
    /// @param newBackend Replacement backend. Ownership is transferred.
    template <typename Backend>
        requires std::derived_from<Backend, ::morph::backend::detail::IBackend>
    void switchBackend(std::unique_ptr<Backend> newBackend) {
        switchBackend(std::shared_ptr<::morph::backend::detail::IBackend>{std::move(newBackend)});
    }

    /// @brief Atomically replaces the active backend with @p newBackend.
    ///
    /// Identical to the `unique_ptr` overload, except the caller keeps shared
    /// ownership of @p newBackend — the backend can be re-installed later
    /// (e.g. switching back to a long-lived remote backend after a temporary
    /// fallback to a local one) without reconstructing it.
    ///
    /// @param newBackend Replacement backend, shared with the caller.
    void switchBackend(std::shared_ptr<::morph::backend::detail::IBackend> newBackend) {
        auto newShared = std::move(newBackend);
        // Stamp the current default session onto the new backend before phase 1
        // below builds a single `register`/`registerShared` envelope per live
        // binding — otherwise every control envelope re-registering handlers on
        // the new backend would carry a default-constructed (unauthenticated)
        // session, exactly the #63 gap this hook closes. Read under
        // `_sessionMtx` alone (a leaf mutex never held while calling into this
        // backend), mirroring `executeVia`'s copy-then-release pattern.
        {
            std::scoped_lock const lock{_sessionMtx};
            newShared->setSession(_defaultSession);
        }
        std::shared_ptr<::morph::backend::detail::IBackend> previous;
        // Settled after the mutexes are released, never under them: resolving a
        // waiter runs consumer code, which is free to call back into this
        // `Bridge`.
        std::vector<std::shared_ptr<detail::HandlerBinding>> settleBound;
        std::vector<std::shared_ptr<detail::HandlerBinding>> settleFailed;
        std::exception_ptr staging;
        // Whether this frame may stop and wait for each bind to settle. A
        // backend that answers `kCallerMustNotBlock` delivers its replies
        // through the calling thread's own event loop, so waiting here is a
        // deadlock rather than a delay — on a WASM main thread, a page abort.
        // Until morph#615 this site did not ask at all: it called the blocking
        // `registerModelShared`/`registerModelWithContext` directly, so a
        // backend that had just been given a way to say "do not do this to me"
        // was blocked here anyway. See `IBackend::bindWaitPolicy`.
        bool const mayBlock = newShared->bindWaitPolicy() == ::morph::backend::detail::BindWait::kCallerMayBlock;
        {
            // Both mutexes: this phase reads/writes every live binding's
            // `primary`/`contextKey` (via `_attachMtx`'s ownership of those
            // fields) as well as `_handlers` itself (via `_mtx`), and must not
            // race a concurrent attachHandler()/ensureBound()/
            // assignHandlerPrimary() call on any one of them.
            std::scoped_lock const lock{_mtx, _attachMtx};

            detail::StagedRebinds rebinds;
            staging = stageRebinds(newShared, mayBlock, rebinds);
            if (staging) {
                rollbackStaged(*newShared, rebinds.staged);
                settleFailed = std::move(rebinds.armed);
            } else {
                previous = commitRebinds(newShared, mayBlock, rebinds, settleBound);
            }
        }
        for (const auto& binding : settleFailed) {
            resolveRegistrationWaiters(*binding, /*ok=*/false, staging);
        }
        for (const auto& binding : settleBound) {
            resolveRegistrationWaiters(*binding, /*ok=*/true, nullptr);
        }
        if (staging) {
            // Rethrown here rather than from inside the locked block, so the
            // waiters above are settled outside `_mtx`/`_attachMtx`. Everything
            // below stays unreached on this path, exactly as before: a staging
            // failure leaves the outgoing backend's reconnect handler and its
            // pending completions alone, because the switch did not happen.
            std::rethrow_exception(staging);
        }
        if (previous && previous != newShared) {
            previous->setReconnectHandler(nullptr);
        }
        // Drain the outgoing backend outside the bridge mutex: cancelPending
        // delivers callbacks through the caller's gui executor, and we never want
        // to hold _mtx while user code runs.
        if (previous && previous != newShared) {
            previous->cancelPending(std::make_exception_ptr(::morph::backend::BackendChangedError{}));
        }
    }

    /// @brief Deregisters @p binding from the active backend and removes it from tracking.
    ///
    /// After this call, any future `executeVia()` using this binding will
    /// complete with an error.
    /// @param binding Binding to remove. Must have been returned by `registerHandler()`.
    void deregisterHandler(const std::shared_ptr<detail::HandlerBinding>& binding) {
        std::scoped_lock const lock{_mtx};
        uint64_t const raw = binding->currentId.load();
        if (raw != 0U) {
            loadBackend()->deregisterModel(::morph::exec::detail::ModelId{raw});
        }
        // Reset to the "0 = unbound" sentinel so a late/concurrent executeVia on
        // this binding fails fast on the documented guard rather than sending a
        // now-destroyed ModelId to the backend.
        binding->currentId.store(0);
        auto iter = std::ranges::find_if(_handlers, [&](auto& weak) {
            auto sptr = weak.lock();
            return sptr && sptr.get() == binding.get();
        });
        if (iter != _handlers.end()) {
            _handlers.erase(iter);
        }
    }

    /// @brief Dispatches @p action against the model identified by @p binding.
    ///
    /// Snapshots the backend `shared_ptr` under `_backendMtx` and reads the
    /// binding's `currentId` lock-free, so the call does not block
    /// `switchBackend()`. If a backend switch happens concurrently,
    /// the old backend still exists (its `shared_ptr` refcount is > 0) and the
    /// call either succeeds or fails with "model not found" — both are safe.
    ///
    /// On `LocalBackend`, the `localOp` this method builds first overwrites any
    /// declared computed fields from their inputs (`morph::forms::recomputeAll`,
    /// a no-op for actions with no `computedFields`), then enforces
    /// `morph::model::ActionValidator<Action>::ready(action)` before calling
    /// `Model::execute`, mirroring `ActionDispatcher::registerAction`'s runner
    /// (`registry.hpp`) for the in-process path. A `false` result resolves the
    /// returned `Completion` through `onError` with a `morph::model::ValidationError`
    /// instead of executing the action — see docs/spec/core/registry.md and
    /// docs/spec/forms/forms.md.
    ///
    /// @tparam Model  Model type that owns the handler.
    /// @tparam Action Action type to dispatch.
    /// @param binding Binding returned by `registerHandler<Model>()`.
    /// @param action  Action to execute (moved in).
    /// @param cbExec  Executor on which the `Completion` callbacks are posted.
    /// @param onResult Optional observer run on the typed result *before* it is
    ///                 moved into the returned `Completion` and before the
    ///                 caller's own `.then`. Used to adopt a result-sourced
    ///                 primary key so the binding is already promoted by the time
    ///                 user code sees the result; empty for every other call.
    /// @return Completion that resolves with the typed result or an exception
    ///         (including `ValidationError` on `LocalBackend` when the action
    ///         fails its validator).
    template <typename Model, typename Action>
    // A dispatch function with a carefully reasoned safety argument threaded through its branches (see
    // the .then/.onError comments below); extracting helpers would separate that reasoning from the code
    // it justifies more than it would simplify either half. Same treatment as remote.hpp's
    // comparably-shaped dispatch functions.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    ::morph::async::Completion<typename ::morph::model::ActionTraits<Action>::Result> executeVia(
        const std::shared_ptr<detail::HandlerBinding>& binding, Action action, ::morph::exec::IExecutor* cbExec,
        std::function<void(const typename ::morph::model::ActionTraits<Action>::Result&)> onResult = {}) {
        using R = ::morph::model::ActionTraits<Action>::Result;

        auto backend = loadBackend();
        uint64_t const raw = binding->currentId.load();

        auto typedState = std::make_shared<::morph::async::detail::CompletionState<R>>();
        ::morph::async::Completion<R> typed{typedState, cbExec};
        if (raw == 0U) {
            // Resolved synchronously, before any dispatch -- never counted as
            // pending, so nothing to decrement here (see pendingCalls()).
            typedState->setException(std::make_exception_ptr(std::runtime_error("handler not bound")));
            return typed;
        }
        // Counted from here on: every path below either reaches the backend
        // dispatch or falls through to one of the two mutually-exclusive
        // resolution continuations attached to anyCompletion further down,
        // each of which decrements exactly once (setValue/setException are
        // first-result-wins, so only one of the two ever actually fires).
        _pendingCalls->fetch_add(1, std::memory_order_relaxed);
        // Arm the client-side deadline (setExecuteDeadline) only for real
        // dispatches -- the fast-failed "handler not bound" completion above is
        // already resolved and needs no timer. Reading the deadline and arming
        // it happen under one lock so a concurrent setExecuteDeadline() cannot
        // interleave between the two.
        std::optional<::morph::async::detail::TimeoutScheduler::Handle> deadlineHandle;
        // A private shared_ptr copy, obtained under the same lock as the
        // schedule() call -- not `this->_timeoutScheduler`, and not gated on
        // `alive` -- so the two `.then()`/`.onError()` callbacks below can
        // call `cancel()` on a scheduler that is provably still alive,
        // regardless of whether ~Bridge() has run or is running concurrently
        // on another thread. `_executeDeadlineMtx` alone does not establish
        // that: ~Bridge()'s own body never acquires it, so a plain
        // `!alive.active()` check followed by `_timeoutScheduler->cancel()`
        // a few instructions later is a check-then-use race against
        // ~Bridge()'s implicit member destruction (which joins
        // TimeoutScheduler's thread). Holding a shared_ptr for the
        // callback's own lifetime turns that into a non-issue by
        // construction: while any copy of it is alive, ~TimeoutScheduler()
        // cannot run at all.
        std::shared_ptr<::morph::async::detail::TimeoutScheduler> schedulerRef;
        {
            std::scoped_lock const lock{_executeDeadlineMtx};
            if (_executeDeadline.count() > 0 && _timeoutScheduler) {
                schedulerRef = _timeoutScheduler;
                // The callback captures `typedState` alone -- never `this` -- so
                // it stays safe to fire even while ~Bridge() is running, and
                // equally safe to fire *after* its own `cancel()`:
                // `TimeoutScheduler::cancel` stops a callback that has not
                // started but returns without waiting for one that already has
                // (see that function's comment), so the disarms in the two
                // continuations below are best-effort by contract and not only
                // when `cancel()` throws. A deadline callback that is already
                // mid-flight when the real reply lands still runs its
                // `setException`, which `CompletionState` discards on an
                // already-ready state -- first result wins. That, not the
                // disarm, is what makes the race harmless (morph#620).
                deadlineHandle = schedulerRef->schedule(_executeDeadline, [typedState] {
                    typedState->setException(std::make_exception_ptr(::morph::backend::ClientTimeoutError{}));
                });
            }
        }
        ::morph::backend::detail::ActionCall call;
        call.modelTypeId = std::string{::morph::model::ModelTraits<Model>::typeId()};
        call.actionTypeId = std::string{::morph::model::ActionTraits<Action>::typeId()};
        auto sharedAction = std::make_shared<Action>(std::move(action));
        call.serializeAction = [sharedAction] { return ::morph::model::ActionTraits<Action>::toJson(*sharedAction); };
        call.deserializeResult = [](std::string_view jsonStr) -> std::shared_ptr<void> {
            return std::make_shared<R>(::morph::model::ActionTraits<Action>::resultFromJson(jsonStr));
        };
        call.localOp = [sharedAction](::morph::model::detail::IModelHolder& holder) -> std::shared_ptr<void> {
            // Enforce the action's validator on the local execution path too, so
            // a caller that constructs an Action by hand and calls
            // BridgeHandler<Model>::execute<Action>() directly is rejected the
            // same way a hand-built wire envelope is rejected by
            // ActionDispatcher::registerAction's runner (registry.hpp). No JSON is
            // involved on this path, so there is no declared-precision
            // reconciliation step here (that only applies to decoded wire
            // payloads); the Quantity fields carry whatever precision the caller
            // constructed them with. ActionValidator<Action>::ready defaults to
            // `true` for actions with no validator, so this is a no-op for
            // unvalidated actions (zero behavior change). The thrown exception is
            // caught by LocalBackend::execute's strand task (backend.hpp) and
            // resolves this Completion through onError.
            //
            // Overwrite any computed fields from their declared inputs before the
            // validator runs and the model ever sees the action -- the same
            // authoritative recompute ActionDispatcher::registerAction's runner
            // performs for remote topologies (registry.hpp), applied here for the
            // in-process LocalBackend path (every execute<Action>()/executeJson
            // call). Recompute must run before the validator check so a validator
            // inspecting a computed field sees the authoritative value, not
            // whatever the caller constructed the action with. No-op for actions
            // with no computedFields. See docs/spec/forms/forms.md.
            ::morph::forms::recomputeAll(*sharedAction);
            if (!::morph::model::ActionValidator<Action>::ready(*sharedAction)) {
                throw ::morph::model::ValidationError{::morph::model::ModelTraits<Model>::typeId(),
                                                      ::morph::model::ActionTraits<Action>::typeId()};
            }
#ifdef MORPH_CLIENT_ONLY
            // A MORPH_CLIENT_ONLY build never links Model::execute's definition
            // (see docs/spec/core/registry.md, "MORPH_CLIENT_ONLY") -- this
            // #ifdef, not just the registration macros, is what actually makes
            // that true: ActionCall::localOp is constructed unconditionally
            // here regardless of which backend ends up installed, so the
            // `model.execute(...)` call below would otherwise still force the
            // linker to resolve it even for a build that only ever installs a
            // remote backend. LocalBackend must not be used in such a build;
            // reaching this point means it was anyway.
            static_cast<void>(holder);
            throw std::logic_error(
                "Bridge::executeVia: localOp invoked in a MORPH_CLIENT_ONLY build -- LocalBackend must not be "
                "used");
#else
            auto& model = holder.template into<Model>();
            // Local mode has no client/server split, so this is the same execution
            // site `ActionDispatcher::registerAction`'s runner is for remote modes
            // (registry.hpp) — see that overload's doc comment for the full story,
            // including why both the success and failure paths below record a
            // journal entry (a rejected/throwing execute must not leave the audit
            // trail silent) and why the exception is rethrown unchanged either way.
            try {
                auto result = std::make_shared<R>(model.execute(*sharedAction));
                if constexpr (::morph::model::detail::actionLoggable<Action>() == ::morph::model::Loggable::Yes) {
                    if (holder.hasActionLog()) {
                        // entityKey/principal/timestampMs are filled in by recordIfAttached.
                        ::morph::model::detail::recordActionSuccess(
                            holder, std::string{::morph::model::ModelTraits<Model>::typeId()},
                            std::string{::morph::model::ActionTraits<Action>::typeId()},
                            ::morph::model::ActionTraits<Action>::toJson(*sharedAction),
                            ::morph::model::detail::actionPayloadSchema<Action>(),
                            ::morph::model::ActionTraits<Action>::resultToJson(*result));
                    }
                }
                return result;
            } catch (const std::exception& exc [[maybe_unused]]) {
                if constexpr (::morph::model::detail::actionLoggable<Action>() == ::morph::model::Loggable::Yes) {
                    if (holder.hasActionLog()) {
                        ::morph::model::detail::recordActionFailure(
                            holder, std::string{::morph::model::ModelTraits<Model>::typeId()},
                            std::string{::morph::model::ActionTraits<Action>::typeId()},
                            ::morph::model::ActionTraits<Action>::toJson(*sharedAction),
                            ::morph::model::detail::actionPayloadSchema<Action>(), exc.what());
                    }
                }
                throw;
            }
#endif
        };
        {
            std::scoped_lock const lock{_sessionMtx};
            call.session = _defaultSession;
        }
        // `backend->execute` is not `noexcept` and genuinely throws: for
        // `QtWebSocketBackend` it runs `call.serializeAction()` (user `toJson`
        // and glaze) and `wire::encode(env)`. A throw here escaped
        // `BridgeHandler::execute` with `_pendingCalls` already incremented and
        // the deadline already armed, permanently inflating `pendingCalls()` --
        // which the class documents as a quiescence gate -- and stranding the
        // timer entry. Undo both, then let the exception continue to the caller
        // (morph#502).
        ::morph::async::Completion<std::shared_ptr<void>> anyCompletion = [&] {
            try {
                return backend->execute(::morph::exec::detail::ModelId{raw}, std::move(call), cbExec);
            } catch (...) {
                _pendingCalls->fetch_sub(1, std::memory_order_relaxed);
                if (deadlineHandle && schedulerRef) {
                    schedulerRef->cancel(*deadlineHandle);
                }
                throw;
            }
        }();
        anyCompletion
            .then([typedState, onResult = std::move(onResult), raw, deadlineHandle, schedulerRef,
                   pendingCalls = _pendingCalls, subscriptions = _subscriptions,
                   lifetime = _lifetime](const std::shared_ptr<void>& vAny) {
                // Disarm the client-side deadline first, before any of the
                // forwarding work below: a slow onResult/publishResult callback
                // must not give the timer a window to fire concurrently and
                // resolve this completion with ClientTimeoutError while the real
                // result is already in hand. Uses the `schedulerRef` copy
                // captured above, not `this->_timeoutScheduler` -- see that
                // capture's own comment for why: this callback can in principle
                // run after ~Bridge(), and `schedulerRef` (not a liveness check)
                // is what makes `cancel()` safe in that case, by keeping the
                // scheduler alive for exactly as long as this callback needs it.
                // Leaving the entry armed if it were never cancelled would be
                // harmless (~TimeoutScheduler drops pending entries without
                // firing them), but a thrown cancel() must not prevent the real
                // result from resolving the completion below either.
                if (deadlineHandle && schedulerRef) {
                    try {
                        schedulerRef->cancel(*deadlineHandle);
                    } catch (...) {  // NOLINT(bugprone-empty-catch)
                        // Best-effort: a failed cancel leaves the deadline's own
                        // entry to fire later and find nothing (setValue/
                        // setException below are idempotent), which is exactly
                        // what an uncancelled entry already does.
                    }
                }
                // A backend completion can in principle resolve after the
                // Bridge is gone (see liveness()'s doc comment): the backend
                // may be co-owned and outlive this Bridge, or this callback
                // may already be running when ~Bridge() runs concurrently on
                // another thread. This dispatch is resolving either way, so
                // `pendingCalls` -- pinned above, independent of the Bridge --
                // is decremented unconditionally (morph#489, site 1): one of
                // the two mutually-exclusive resolution continuations for this
                // dispatched call (the other is the .onError below), so exactly
                // one of them decrements per call, whether the value-forwarding
                // below then succeeds or itself throws and routes to
                // setException.
                pendingCalls->fetch_sub(1, std::memory_order_relaxed);
                // Guard the value-forwarding: if R's move/copy throws (or the cast
                // is somehow wrong), route the exception to the typed completion's
                // error sink instead of letting it escape the callback executor —
                // where ThreadPoolExecutor swallows it (the outer completion would
                // then hang forever) and QtExecutor lets it reach the event loop
                // and std::terminate. Mirrors the forwarding guard in remote.hpp's
                // SimulatedRemoteBackend::execute. See docs/spec/core/bridge.md.
                try {
                    auto* const typedResult = static_cast<R*>(vAny.get());
                    // `bridgeAlive` is read once, fresh, under `lifetime`'s own
                    // gate -- the same `detail::BridgeLifetime` `~BridgeHandler`
                    // uses (see its doc comment for why a `CallbackToken` cannot
                    // carry this weight) -- and reused below to decide whether
                    // to publish, so both decisions come from one consistent
                    // snapshot, matching this function's behaviour before this
                    // fix. Only `onResult` actually runs inside the gate: it is
                    // the one piece of this continuation that is not pinned
                    // above and genuinely needs the *current* bridge/backend
                    // (for a result-keyed action, it calls
                    // `assignHandlerPrimary`, which touches `_attachMtx` and
                    // `loadBackend()`) -- runs before the value is moved out and
                    // before the caller's own .then, so a result-sourced
                    // primary key is adopted before any user code observes the
                    // result. `assignHandlerPrimary`'s own synchronous path is
                    // non-blocking by construction (its doc comment: it prefers
                    // the backend's async registration precisely to avoid a
                    // nested-event-loop block), so holding the gate across it
                    // does not expose `~Bridge` to the unbounded-block hazard
                    // morph#489 names for the sites this fix does not
                    // mechanically apply to (installReconnectHandler, site 4).
                    bool bridgeAlive = false;
                    {
                        std::shared_lock const gate{lifetime->mtx};
                        bridgeAlive = lifetime->alive;
                        if (bridgeAlive && onResult) {
                            onResult(*typedResult);
                        }
                    }
                    // Fan the result out to everything attached to this
                    // instance before the value is moved away. `subscriptions`
                    // is pinned above, so this call itself is safe regardless
                    // of the Bridge's lifetime (`SubscriptionRegistry` snapshots
                    // its own sinks under its own lock and invokes them outside
                    // it -- see that class -- so it cannot deadlock against a
                    // subscriber that re-enters, which is exactly the hazard
                    // that rules out gating this call the way `onResult` above
                    // is gated). Still conditioned on `bridgeAlive`, unchanged
                    // from before this fix: a Bridge already torn down should
                    // not go on fanning results out to its instance
                    // subscribers.
                    if constexpr (std::is_copy_constructible_v<R>) {
                        if (bridgeAlive && subscriptions->hasSubscribers()) {
                            subscriptions->publishResult(::morph::exec::detail::ModelId{raw},
                                                         std::type_index{typeid(R)}, std::any{*typedResult});
                        }
                    }
                    typedState->setValue(std::move(*typedResult));
                } catch (...) {
                    typedState->setException(std::current_exception());
                }
            })
            .onError([typedState, deadlineHandle, schedulerRef,
                      pendingCalls = _pendingCalls](const std::exception_ptr& err) {
                // Same disarm-first reasoning (and the same schedulerRef-based
                // safety) as the success branch above: a real error reply
                // settles the completion, so the deadline must not also fire.
                if (deadlineHandle && schedulerRef) {
                    try {
                        schedulerRef->cancel(*deadlineHandle);
                    } catch (...) {  // NOLINT(bugprone-empty-catch)
                        // Best-effort: a failed cancel leaves the deadline's own
                        // entry to fire later and find nothing (setValue/
                        // setException below are idempotent), which is exactly
                        // what an uncancelled entry already does.
                    }
                }
                // The other of the two mutually-exclusive resolution paths --
                // see the .then continuation above. `pendingCalls` is pinned
                // there too, so this needs no liveness check at all (morph#489,
                // site 2) -- nothing else in this branch touches the Bridge.
                pendingCalls->fetch_sub(1, std::memory_order_relaxed);
                typedState->setException(err);
            });
        return typed;
    }

private:
    template <typename, typename>
    friend class BridgeHandler;

    /// @brief Weak observer of this bridge's lifetime, handed to each handler.
    ///
    /// Note `~BridgeHandler` does **not** use this: gating a *call into* the
    /// bridge needs `lifetimeGate()`'s `detail::BridgeLifetime`, because a token
    /// answers only advisorily and a member call made a few instructions after a
    /// stale "active" runs on destroyed memory (morph#486). A token is the right
    /// tool for *declining work*, not for keeping an object alive across a call.
    /// The bridge must still outlive its handlers for normal `execute`/`set`
    /// calls; the gate only makes *teardown* order-independent.
    ///
    /// The bridge is the framework's own first consumer of the primitive every
    /// caller now gets (docs/spec/core/callback_scope.md). It uses only the
    /// liveness half: `_callbacks` is never stopped explicitly, so its tokens go
    /// inactive exactly when the `Bridge` is destroyed.
    [[nodiscard]] ::morph::async::CallbackToken liveness() const { return _callbacks.token(); }

    /// @brief The lifetime gate every handler holds, so its destructor can call
    ///        back into this bridge without racing `~Bridge`.
    ///
    /// Unlike `liveness()`, the answer this carries is not advisory: see
    /// `detail::BridgeLifetime` for what it guarantees and why the bridge needs
    /// both (delivery gating stays on the token; a *call into the bridge* needs
    /// the gate).
    /// @return The shared gate; never null, and outlives the `Bridge`.
    [[nodiscard]] std::shared_ptr<detail::BridgeLifetime> lifetimeGate() const { return _lifetime; }

    /// @brief Retires the lifetime gate, waiting out any deregistration inside it.
    ///
    /// Called as `~Bridge`'s first statement and nowhere else. After it returns,
    /// no `~BridgeHandler` will enter `deregisterHandler` on this bridge, and none
    /// is still inside one.
    void closeLifetime() noexcept {
        std::unique_lock const lock{_lifetime->mtx};
        _lifetime->alive = false;
    }

    std::shared_ptr<::morph::backend::detail::IBackend> loadBackend() const {
        std::scoped_lock const lock{_backendMtx};
        return _backend;
    }

    /// @brief Shared body of both `registerHandler()` overloads: binds through
    ///        `IBackend::bindModel`, the structural registration surface, and
    ///        since morph#571 the only one.
    ///
    /// A backend with a non-blocking bind returns an unsettled `Completion` and
    /// the binding is returned unbound (see `IBackend::bindModel`'s doc comment
    /// for why that matters — a nested-event-loop block aborts a WASM main
    /// thread). One with only the blocking default settles inside the call,
    /// having run exactly the `registerModelWithContext` this used to call
    /// directly, so the binding is bound before this returns and a failure is
    /// **rethrown** to the caller, as that call used to throw.
    ///
    /// @p binding is added to `_handlers` *before* the backend call, so a
    /// concurrently-running `switchBackend()`/reconnect can already see and
    /// re-register it even while this registration is still in flight (see
    /// the continuation's comment for why that race is harmless). This also
    /// means the backend call must not run under `_mtx`: a backend that settles
    /// from inside the dispatch call would otherwise self-deadlock re-acquiring
    /// `_mtx` in the continuation below.
    /// @param binding Binding to register; its `typeId`/`modelFactory`/`contextKey` must be set.
    void registerHandlerImpl(const std::shared_ptr<detail::HandlerBinding>& binding) {
        auto backend = loadBackend();
        {
            std::scoped_lock const lock{_mtx};
            _handlers.push_back(binding);
        }

        // Set before the backend call, not after: a backend could in
        // principle invoke onRegistered/onError synchronously (none
        // documented here do, but whenBound() must still be correct if one
        // ever did), and a whenBound() call racing in from another thread
        // must see "in flight" for the whole window the registration could
        // resolve in, not a window that starts one statement too late.
        {
            std::scoped_lock const lock{binding->registrationMtx};
            binding->registrationInFlight = true;
        }

        // `binding->contextKey` is read here without `_attachMtx`, and that is a
        // deliberate carve-out from the "read only under `_attachMtx`" rule the
        // member comment states -- not an oversight. Taking the lock here is
        // *ruled out by test*: "Bridge: an in-flight shared attach does not
        // block unrelated handler registration"
        // (tests/test_shared_instances.cpp) exists precisely because
        // `attachHandler` holds `_attachMtx` across a full backend round trip,
        // and having registration contend for the same lock is the regression
        // that test was written to catch. Measured: acquiring it here, even
        // briefly, fails that case.
        //
        // What makes the unlocked read safe is ordering, not locking: the
        // writers (`attachHandler`, `ensureBound`, `assignHandlerPrimary`) all
        // operate on a binding that is already registered, and this runs during
        // registration. The pre-built-binding `registerHandler()` overload hands
        // the caller the binding first, so the requirement is on the caller:
        // **set `contextKey` before calling `registerHandler()`, and do not
        // mutate it concurrently with that call.** After registration returns,
        // every access goes under `_attachMtx` as documented. morph#505.
        //
        // Both continuations come from `makeBindCallbacks`, shared with
        // `switchBackend`'s phase 1 and the reconnect handler: a stale reply
        // is discarded rather than allowed to overwrite the id one of those
        // two just installed, and `whenBound()` waiters are settled either
        // way, because a discarded reply still means this binding's own
        // registration attempt is over.
        auto [onRegistered, onFailed] = makeBindCallbacks(binding, backend, "[registerHandler]");

        // The structural surface (`IBackend::bindModel`). An empty `primary`
        // with a zero `current` is the request shape that means
        // `registerModelWithContext` -- the verb this branch used to call
        // directly -- so a backend with no non-blocking bind runs exactly that,
        // blocks exactly as long, and settles before `bindModel` returns.
        //
        // `inlineExecutor()` because that is where this continuation ran
        // before: on whichever thread the backend settled on, which for the
        // blocking default is this one. See `exec::detail::InlineExecutor`.
        //
        // A synchronous backend that *fails* must still fail the way it used to
        // -- `registerModelWithContext` threw out of `registerHandler()`, and a
        // `BridgeHandler` constructor that cannot bind must not return as if it
        // had. So a rejection arriving inside this frame is parked and
        // rethrown, while one arriving later (a genuinely non-blocking backend,
        // where no caller is left to throw at) goes to `onFailed`, which
        // settles `whenBound()` waiters instead.
        auto handoff = std::make_shared<detail::AsyncDispatchHandoff>();
        auto completion = backend->bindModel(::morph::backend::detail::BindRequest{.typeId = binding->typeId,
                                                                                   .factory = binding->modelFactory,
                                                                                   .contextKey = binding->contextKey,
                                                                                   .primary = {},
                                                                                   .current = {}},
                                             ::morph::exec::detail::inlineExecutor());
        auto* const bridgeExec = _bridgeExec;
        completion
            .then([onRegistered, handoff, bridgeExec](::morph::exec::detail::ModelId newId) mutable {
                if (detail::parkIfInFrame(*handoff, true, newId, nullptr)) {
                    return;
                }
                // Only reachable for a `kCallerMustNotBlock` backend, whose
                // reply lands after this frame gave up waiting: exactly the
                // delivery morph#588 gives the bridge's executor. The
                // in-frame outcome below is published by this frame instead,
                // on this thread, whatever executor the bridge holds.
                detail::deliverLate(bridgeExec, [registered = std::move(onRegistered), newId] { registered(newId); });
            })
            .onError([onFailed, handoff, bridgeExec](const std::exception_ptr& failure) mutable {
                if (detail::parkIfInFrame(*handoff, false, {}, failure)) {
                    return;
                }
                detail::deliverLate(
                    bridgeExec, [failed = std::move(onFailed), failure] { failed(detail::describeFailure(failure)); });
            });
        // `registerHandler` is a synchronous entry point: its caller
        // constructs a `BridgeHandler` and uses it on the next line, and
        // `executeVia` fails fast on an unbound `currentId`. So unless the
        // backend says its completion cannot settle while this thread waits,
        // this frame waits for it -- and the outcome is then published and
        // rethrown by exactly the code below that already handles a backend
        // that settled inline. The two cases differ in how long this frame
        // sits still, not in what the caller observes.
        //
        // `kCallerMustNotBlock` is the exception, and it is the *reason* it is
        // an exception that matters: a `QtWebSocketBackend` with
        // `asyncRegistrationEnabled` set delivers its reply through the Qt
        // event loop of this very thread, so waiting here is a deadlock, not a
        // delay -- on a WASM main thread it aborts the page (morph#568). Such a
        // backend's caller gets an unbound handler and must gate on
        // `whenBound()`, exactly as the removed `*Async` path already required
        // of it. See `IBackend::bindWaitPolicy` and morph#593.
        auto parked = backend->bindWaitPolicy() == ::morph::backend::detail::BindWait::kCallerMayBlock
                          ? detail::awaitHandoff(*handoff)
                          : detail::claimHandoff(*handoff);
        if (parked) {
            if (!parked->succeeded) {
                // Settles the waiters queued during the window above before
                // unwinding, so a concurrent whenBound() is rejected rather
                // than left hanging on a registration that will never complete.
                resolveRegistrationWaiters(*binding, /*ok=*/false, parked->failure);
                std::rethrow_exception(parked->failure);
            }
            onRegistered(parked->modelId);
        }
    }

    /// @brief Clears `registrationInFlight` and settles every queued
    ///        `whenBound()` waiter exactly once. Shared by both
    ///        `registerHandlerImpl` callbacks (success and failure).
    /// @param binding Binding whose registration just settled.
    /// @param ok      `true` to resolve waiters with `setValue(true)`;
    ///                `false` with @p err via `setException`.
    /// @param err     Exception to deliver when `ok` is `false`.
    static void resolveRegistrationWaiters(detail::HandlerBinding& binding, bool ok, std::exception_ptr err) {
        std::vector<std::pair<std::function<void(bool)>, std::function<void(std::exception_ptr)>>> waiters;
        {
            std::scoped_lock const lock{binding.registrationMtx};
            binding.registrationInFlight = false;
            waiters.swap(binding.registrationWaiters);
        }
        for (auto& [onOk, onErr] : waiters) {
            if (ok) {
                onOk(true);
            } else {
                // Only the failure callback above supplies an `err`; the
                // success callback settles through this same arm with a null
                // one whenever the reply's id was discarded (`applied` false)
                // and the binding is still unbound. `CompletionState` now
                // refuses to settle on a null (issue #347), but it can only
                // substitute a generic message — the meaning of *this*
                // failure is known here and nowhere else, so name it here.
                onErr(err ? err
                          : std::make_exception_ptr(
                                std::runtime_error{"registration did not complete: the reply was discarded and '" +
                                                   binding.typeId + "' is still unbound"}));
            }
        }
    }

    /// @brief Builds the two continuations every model acquisition needs when
    ///        its reply may arrive after the dispatching frame has gone.
    ///
    /// One body for the three sites that acquire an instance for a binding —
    /// `registerHandlerImpl`, `switchBackend`'s phase 1 and
    /// `installReconnectHandler`'s handler — because all three have the same
    /// two problems, and had three chances to solve one of them differently:
    /// the reply may land on a thread that does not own the `Bridge`, and it
    /// may be stale by the time it lands.
    ///
    /// The success continuation holds `lifetime`'s gate across the whole touch
    /// of `this` (`_mtx`, `loadBackend()`) rather than only checking at entry —
    /// `CallbackToken::active()` is advisory and cannot carry that weight, see
    /// `detail::BridgeLifetime`. Holding it across this span is safe for the
    /// reason morph#489 gives: nothing inside is a call into unbounded or
    /// consumer-supplied code, only a mutex and a backend-pointer comparison.
    /// The id is published only while @p backend is still the active one, so a
    /// reply from a backend `switchBackend()` has already replaced cannot
    /// overwrite the id that switch just installed.
    ///
    /// Waiters are resolved either way, and outside `_mtx`: whether the id was
    /// applied or discarded, this binding's registration attempt has settled,
    /// and nothing should still be described as in flight.
    /// @param binding Binding the reply belongs to; held weakly by both callbacks.
    /// @param backend Backend the call was issued to; held weakly, and compared
    ///                against the active one before anything is published.
    /// @param site    Bracketed tag naming the caller in the failure log line.
    /// @return `{onRegistered, onFailed}` — the first takes the new `ModelId`,
    ///         the second a diagnostic string.
    [[nodiscard]] std::pair<std::function<void(::morph::exec::detail::ModelId)>,
                            std::function<void(const std::string&)>>
    makeBindCallbacks(const std::shared_ptr<detail::HandlerBinding>& binding,
                      const std::shared_ptr<::morph::backend::detail::IBackend>& backend, std::string_view site) {
        std::weak_ptr<::morph::backend::detail::IBackend> const weakBackend{backend};
        std::weak_ptr<detail::HandlerBinding> const weakBinding{binding};
        std::function<void(::morph::exec::detail::ModelId)> onRegistered =
            [this, weakBackend, weakBinding, lifetime = _lifetime](::morph::exec::detail::ModelId newId) {
                auto strongBinding = weakBinding.lock();
                bool applied = false;
                if (strongBinding) {
                    std::shared_lock const gate{lifetime->mtx};
                    if (lifetime->alive) {
                        std::scoped_lock const lock{_mtx};
                        auto pinned = weakBackend.lock();
                        if (pinned && pinned == loadBackend()) {
                            strongBinding->currentId.store(newId.v);
                            applied = true;
                        }
                    }
                }
                if (strongBinding) {
                    resolveRegistrationWaiters(*strongBinding, /*ok=*/applied || isBound(strongBinding), nullptr);
                }
            };
        std::function<void(const std::string&)> onFailed = [weakBinding, tag = std::string{site},
                                                            typeId = binding->typeId](const std::string& message) {
            ::morph::log::logError(tag + " registration of '" + typeId + "' failed: " + message);
            if (auto strongBinding = weakBinding.lock()) {
                resolveRegistrationWaiters(
                    *strongBinding, /*ok=*/false,
                    std::make_exception_ptr(std::runtime_error("registration failed: " + message)));
            }
        };
        return {std::move(onRegistered), std::move(onFailed)};
    }

    /// @brief Acquires an instance for @p binding on @p backend through
    ///        `IBackend::bindModel`, waiting only if @p mayBlock says it may.
    ///
    /// The one dispatch shape `switchBackend`'s staging phase and the reconnect
    /// handler share (morph#615). A non-empty `primary` with a zero `current`
    /// is the request shape that means `registerModelShared`, an empty one the
    /// shape that means `registerModelWithContext` — the two blocking verbs
    /// both sites called directly until morph#615 — so a backend with only the
    /// default `bindModel` runs exactly the call it always did and settles
    /// before this returns. `inlineExecutor()` because that is where these
    /// continuations ran before; see `attachHandlerAsync`.
    ///
    /// A reply that lands inside this frame is parked rather than acted on,
    /// because both callers hold `_mtx`/`_attachMtx` here and the continuation
    /// takes `_mtx` itself.
    /// @param binding  Binding to acquire an instance for.
    /// @param backend  Backend to acquire it from.
    /// @param mayBlock `true` to wait the bind out (`bindWaitPolicy()` said the
    ///                 caller may), `false` to take only what has already landed.
    /// @param site     Bracketed tag naming the caller in a failure log line.
    /// @return The outcome if it settled in this frame; `std::nullopt` if the
    ///         reply is still to come, which only @p mayBlock `== false` allows.
    [[nodiscard]] std::optional<detail::ParkedOutcome> rebindThroughSurface(
        const std::shared_ptr<detail::HandlerBinding>& binding,
        const std::shared_ptr<::morph::backend::detail::IBackend>& backend, bool mayBlock, std::string_view site) {
        auto handoff = std::make_shared<detail::AsyncDispatchHandoff>();
        auto [onBound, onFailed] = makeBindCallbacks(binding, backend, site);
        auto completion = backend->bindModel(
            ::morph::backend::detail::BindRequest{.typeId = binding->typeId,
                                                  .factory = binding->modelFactory,
                                                  .contextKey = binding->contextKey,
                                                  .primary = binding->shared ? binding->primary : std::string{},
                                                  .current = {}},
            ::morph::exec::detail::inlineExecutor());
        auto* const bridgeExec = _bridgeExec;
        completion
            .then([onBound, handoff, bridgeExec](::morph::exec::detail::ModelId newId) mutable {
                if (detail::parkIfInFrame(*handoff, true, newId, nullptr)) {
                    return;  // Settled inline: the dispatching frame owns the outcome.
                }
                // A deferred reply, so both callers have long since released
                // `_mtx`/`_attachMtx` that this continuation re-takes; it is
                // also the delivery whose thread morph#588 lets the bridge
                // choose. See `detail::deliverLate`.
                detail::deliverLate(bridgeExec, [bound = std::move(onBound), newId] { bound(newId); });
            })
            .onError([onFailed, handoff, bridgeExec](const std::exception_ptr& failure) mutable {
                if (detail::parkIfInFrame(*handoff, false, {}, failure)) {
                    return;
                }
                detail::deliverLate(
                    bridgeExec, [failed = std::move(onFailed), failure] { failed(detail::describeFailure(failure)); });
            });
        return mayBlock ? detail::awaitHandoff(*handoff) : detail::claimHandoff(*handoff);
    }

    /// @brief Marks @p binding's registration in flight, so `whenBound()` can
    ///        gate on the window a deferred re-registration opens.
    ///
    /// Set *before* the dispatch, not after: the reply can land the moment
    /// `bindModel` is called, and a `whenBound()` racing in from another thread
    /// must see "in flight" for the whole window the bind can resolve in.
    /// @param binding Binding to arm.
    static void armRegistration(detail::HandlerBinding& binding) {
        std::scoped_lock const guard{binding.registrationMtx};
        binding.registrationInFlight = true;
    }

    /// @brief `switchBackend`'s phase 1: acquire an instance for every live
    ///        binding on @p newBackend without publishing anything yet.
    ///
    /// Caller holds `_mtx` and `_attachMtx`. Reports failure by returning the
    /// exception rather than throwing it, so the caller keeps everything
    /// @p out accumulated and can roll it back.
    ///
    /// Atomicity is exactly as strong as the wait is. For a `kCallerMayBlock`
    /// backend — everything in the tree that is not a
    /// `SynchronousBackendAdapter` or a WASM-configured `QtWebSocketBackend` —
    /// this frame waits out each bind, so every outcome is known before
    /// anything is published and the guarantee is what it always was. For a
    /// `kCallerMustNotBlock` backend it cannot be: "did every re-registration
    /// succeed" is not knowable without waiting, and waiting is the deadlock
    /// the policy exists to prevent. Those binds land in `out.deferred`.
    /// @param newBackend Backend being switched to.
    /// @param mayBlock   Whether this frame may wait for each bind to settle.
    /// @param out        Accumulator; filled incrementally, valid on failure too.
    /// @return Null on success, or the failure that ended staging.
    [[nodiscard]] std::exception_ptr stageRebinds(
        const std::shared_ptr<::morph::backend::detail::IBackend>& newBackend, bool mayBlock,
        detail::StagedRebinds& out) {
        try {
            for (auto& weak : _handlers) {
                auto binding = weak.lock();
                if (!binding) {
                    continue;
                }
                // A shared binding that never attached has no instance to
                // re-create: it stays live and unbound, and acquires one on the
                // new backend the first time it is attached.
                if (binding->shared && binding->primary.empty()) {
                    out.live.push_back(weak);
                    continue;
                }
                // Only armed on the path that can actually defer — a waiting
                // frame settles every outcome itself, so `registrationInFlight`
                // stays what it was before morph#615 for every backend that
                // lets this frame wait.
                if (!mayBlock) {
                    armRegistration(*binding);
                    out.armed.push_back(binding);
                }
                auto parked = rebindThroughSurface(binding, newBackend, mayBlock, "[switchBackend]");
                if (!parked) {
                    out.deferred.push_back(binding);
                    out.live.push_back(weak);
                    continue;
                }
                if (!parked->succeeded) {
                    // A rejected `Completion`, where this loop used to catch a
                    // throw: the structural surface reports failure through the
                    // completion, and a bare `catch (...)` can no longer see
                    // it. The rollback keys on this (morph#615).
                    return parked->failure;
                }
                out.staged.emplace_back(binding, parked->modelId.v);
                out.live.push_back(weak);
            }
        } catch (...) {
            // `bindModel` rejects rather than throws by contract, so this
            // guards only what the contract does not cover — an allocation in
            // this loop, or a backend that throws out of `bindModel` anyway.
            return std::current_exception();
        }
        return nullptr;
    }

    /// @brief Undoes every instance `stageRebinds` created, after it failed.
    ///
    /// A deregister that itself throws is logged and the sweep continues: the
    /// staging failure is the error the caller must see, not this one.
    /// @param backend Backend the staged instances were created on.
    /// @param staged  Staged `(binding, newId)` pairs to release.
    static void rollbackStaged(
        ::morph::backend::detail::IBackend& backend,
        const std::vector<std::pair<std::shared_ptr<detail::HandlerBinding>, std::uint64_t>>& staged) {
        for (const auto& [binding, newId] : staged) {
            try {
                backend.deregisterModel(::morph::exec::detail::ModelId{newId});
            } catch (const std::exception& exc) {
                ::morph::log::logError(std::string{"[switchBackend] rollback deregister failed: "} + exc.what());
            }
        }
    }

    /// @brief `switchBackend`'s phase 2: publish the staged ids and swap the
    ///        backend in. Caller holds `_mtx` and `_attachMtx`.
    ///
    /// A deferred binding is published as **unbound**: the id it held belonged
    /// to the backend being replaced and means nothing on the new one, while
    /// the new id is not here yet. Unbound is the honest state, and it is the
    /// one `executeVia` fails fast on and `whenBound()` gates on — strictly
    /// better than a dangling id from a backend nothing uses any more, which
    /// `executeVia` would happily dispatch against.
    /// @param newBackend Backend to install.
    /// @param mayBlock   Whether staging was allowed to wait; when it was, no
    ///                   binding was armed and none needs settling.
    /// @param rebinds    What staging accumulated.
    /// @param settleBound Out: bindings whose waiters the caller must resolve
    ///                    with `true`, once it has released the mutexes.
    /// @return The backend just replaced, for the caller to retire.
    [[nodiscard]] std::shared_ptr<::morph::backend::detail::IBackend> commitRebinds(
        const std::shared_ptr<::morph::backend::detail::IBackend>& newBackend, bool mayBlock,
        detail::StagedRebinds& rebinds, std::vector<std::shared_ptr<detail::HandlerBinding>>& settleBound) {
        for (const auto& [binding, newId] : rebinds.staged) {
            binding->currentId.store(newId);
        }
        for (const auto& binding : rebinds.deferred) {
            binding->currentId.store(0);
        }
        _handlers = std::move(rebinds.live);

        auto previous = exchangeBackend(newBackend);
        newBackend->notifyBackendChanged();
        installReconnectHandler(newBackend);
        if (!mayBlock) {
            settleBound.reserve(rebinds.staged.size());
            for (const auto& [binding, newId] : rebinds.staged) {
                settleBound.push_back(binding);
            }
        }
        return previous;
    }

    /// @brief Re-acquires an instance for every live binding on a backend that
    ///        has just reconnected. Caller holds `_mtx` and `_attachMtx`.
    ///
    /// The reconnect counterpart of `stageRebinds`, and deliberately not the
    /// same function: there is nothing to stage here. The connection those ids
    /// belonged to is gone either way, so each binding is published as it
    /// settles and a failure costs only that binding.
    /// @param pinned   The reconnected backend, already checked to be the active one.
    /// @param mayBlock Whether this frame may wait for each bind to settle.
    /// @return `(binding, failure)` pairs whose waiters the caller must resolve
    ///         once it has released the mutexes; a null failure means success.
    [[nodiscard]] std::vector<std::pair<std::shared_ptr<detail::HandlerBinding>, std::exception_ptr>> reregisterLive(
        const std::shared_ptr<::morph::backend::detail::IBackend>& pinned, bool mayBlock) {
        std::vector<std::pair<std::shared_ptr<detail::HandlerBinding>, std::exception_ptr>> settle;
        for (auto& weak : _handlers) {
            auto binding = weak.lock();
            if (!binding) {
                continue;
            }
            // Same branch as switchBackend()'s staging phase: a shared binding
            // that never attached has no instance to re-create on the
            // reconnected backend either, and a shared binding that did attach
            // must come back through the `registerModelShared` request shape (a
            // non-empty `primary`), or it re-registers as a non-shared instance
            // and silently drops the sharing.
            if (binding->shared && binding->primary.empty()) {
                continue;
            }
            if (!mayBlock) {
                armRegistration(*binding);
            }
            auto parked = rebindThroughSurface(binding, pinned, mayBlock, "[reconnect]");
            if (!parked) {
                // The reply is still to come and will publish itself. The id
                // this binding holds belongs to the connection that just
                // dropped, so it is cleared rather than left to be dispatched
                // against: `executeVia` fails fast on an unbound handler, and
                // `whenBound()` — which this path is the first re-registration
                // ever to arm — lets a caller wait the reconnect out instead.
                binding->currentId.store(0);
                continue;
            }
            if (!parked->succeeded) {
                // Before morph#615 a failing re-registration threw out of the
                // handler and onto the transport thread, taking every binding
                // after it with it. A rejected `Completion` is reported per
                // binding instead: this one is left unbound and the loop
                // carries on.
                binding->currentId.store(0);
                settle.emplace_back(binding, parked->failure);
                continue;
            }
            binding->currentId.store(parked->modelId.v);
            if (!mayBlock) {
                settle.emplace_back(binding, nullptr);
            }
        }
        return settle;
    }

    std::shared_ptr<::morph::backend::detail::IBackend> exchangeBackend(
        std::shared_ptr<::morph::backend::detail::IBackend> next) {
        std::scoped_lock const lock{_backendMtx};
        auto previous = std::move(_backend);
        _backend = std::move(next);
        return previous;
    }

    void installReconnectHandler(const std::shared_ptr<::morph::backend::detail::IBackend>& backend) {
        if (!backend) {
            return;
        }
        // The handler is invoked on the backend's transport thread. We keep a
        // weak_ptr to the same shared backend so a stale callback fired after a
        // switchBackend is a no-op, and a `CallbackToken` for the bridge
        // so a callback fired after the Bridge is destroyed (a co-owned backend
        // outliving its bridge) is also a no-op instead of a use-after-free on
        // `this`. `~Bridge` additionally clears the handler; this guard covers a
        // reconnect that is already in flight when the Bridge is torn down.
        std::weak_ptr<::morph::backend::detail::IBackend> const weakBackend{backend};
        auto const weakLiveness = _callbacks.token();
        backend->setReconnectHandler([this, weakBackend, weakLiveness] {
            if (!weakLiveness.active()) {
                return;  // The Bridge is gone; do not touch `this`.
            }
            auto pinned = weakBackend.lock();
            // Settled after the mutexes are released, for the reason
            // switchBackend() gives: resolving a waiter runs consumer code.
            std::vector<std::pair<std::shared_ptr<detail::HandlerBinding>, std::exception_ptr>> settle;
            {
                // Both mutexes: reads `_handlers` (guarded by `_mtx`) and each
                // binding's `contextKey` (guarded by `_attachMtx`), same
                // reasoning as switchBackend() above.
                std::scoped_lock const lock{_mtx, _attachMtx};
                if (!pinned || pinned != loadBackend()) {
                    return;  // We've moved on to a different backend; ignore.
                }
                // The same question switchBackend() now asks, for the same
                // reason, and it matters more here: this handler runs on the
                // backend's *transport* thread, and for a `QtWebSocketBackend`
                // with `asyncRegistrationEnabled` that thread is the one whose
                // event loop has to deliver the reply. Calling the blocking
                // verb here — which is what this loop did until morph#615 —
                // parks it against itself, and on a WASM main thread aborts
                // the page. See `IBackend::bindWaitPolicy` and morph#568.
                settle = reregisterLive(
                    pinned, pinned->bindWaitPolicy() == ::morph::backend::detail::BindWait::kCallerMayBlock);
            }
            for (const auto& [binding, failure] : settle) {
                if (failure) {
                    ::morph::log::logError("[reconnect] registration of '" + binding->typeId +
                                           "' failed: " + detail::describeFailure(failure));
                }
                resolveRegistrationWaiters(*binding, /*ok=*/failure == nullptr, failure);
            }
        });
    }

    mutable std::mutex _backendMtx;
    std::shared_ptr<::morph::backend::detail::IBackend> _backend;
    // Where a registration reply that missed its dispatching frame is
    // delivered; null means "inline, on the settling thread", which is what
    // every site did before morph#588. Read once per dispatch and captured by
    // value into the continuation, never read *from* the callback: the
    // callback can run after `~Bridge`, and `this->_bridgeExec` would then be
    // a read of destroyed memory ahead of the very gate that exists to
    // prevent one. Borrowed for the bridge's whole life plus the lifetime of
    // anything still in flight -- see the constructor's `bridgeExec`.
    ::morph::exec::IExecutor* _bridgeExec = nullptr;
    std::mutex _mtx;
    std::vector<std::weak_ptr<detail::HandlerBinding>> _handlers;
    // Guards the shared-handler attach/register/assign path (ensureBound,
    // attachHandler, assignHandlerPrimary) separately from `_mtx`, which
    // guards `_handlers` membership and the active-backend swap.
    // attachModel/registerModelShared/assignPrimary can block on a full
    // network round-trip for a remote backend; if that ran under `_mtx`, an
    // unrelated handler's construction or destruction, or a switchBackend()
    // call, on the *same* Bridge would block for the same round-trip, and a
    // reply-delivering thread that itself needed `_mtx` could deadlock
    // against it. `HandlerBinding::primary`/`contextKey` are therefore
    // mutated (and must be read) only under `_attachMtx` — never under `_mtx`
    // alone. **One carve-out**, and it is load-bearing rather than an
    // oversight: `registerHandlerImpl` reads `contextKey` unlocked *during
    // registration*, because acquiring `_attachMtx` there would make
    // `registerHandler()` contend with a slow shared attach — the exact
    // regression "Bridge: an in-flight shared attach does not block unrelated
    // handler registration" (tests/test_shared_instances.cpp) was written to
    // catch, and which taking the lock there demonstrably reproduces. That read
    // is ordered rather than locked; see its own comment for the requirement
    // that places on a caller of the pre-built-binding overload (morph#505). `switchBackend()` and the reconnect
    // handler, which also touch them alongside `_handlers`, take both mutexes together via `std::scoped_lock{_mtx,
    // _attachMtx}` (deadlock-safe regardless of acquisition order, by `std::scoped_lock`'s own guarantee).
    std::mutex _attachMtx;
    mutable std::mutex _sessionMtx;
    ::morph::session::Context _defaultSession;
    mutable std::mutex _principalMtx;
    ::morph::session::Principal _principal;
    // Client-side execute deadline (see setExecuteDeadline). Both the duration
    // and the lazily-created scheduler live under one mutex, so a concurrent
    // setExecuteDeadline() can never let executeVia() observe a non-zero
    // deadline before the scheduler backing it exists. Declared ahead of
    // `_callbacks` (the last member, and therefore the first destroyed) so the
    // token an in-flight completion callback checks before touching these has
    // already gone inactive by the time they are torn down.
    mutable std::mutex _executeDeadlineMtx;
    std::chrono::milliseconds _executeDeadline{0};
    std::shared_ptr<::morph::async::detail::TimeoutScheduler> _timeoutScheduler;
    // Instance subscriptions. Held against the binding rather than a fixed
    // instance id so a re-pointed handler keeps its subscriptions; matched at
    // publish time by comparing the binding's current instance. See
    // `detail::SubscriptionRegistry` for the locking/pruning implementation.
    //
    // Heap-allocated and shared, like `_lifetime` below: `executeVia()`'s
    // `.then` continuation needs to call `hasSubscribers()`/`publishResult()`
    // from a possibly-post-~Bridge() context (morph#489, sites 1/2), and
    // `SubscriptionRegistry` already snapshots its sinks under its own lock
    // and invokes them outside it (see that class), so pinning the registry
    // itself with a captured `shared_ptr` -- rather than gating a touch of
    // `this` -- makes the call safe with no risk of blocking `~Bridge` behind
    // a subscriber's own callback (the deadlock class morph#489 names for the
    // sites this fix does *not* mechanically apply to). Never null.
    std::shared_ptr<detail::SubscriptionRegistry<detail::HandlerBinding>> _subscriptions{
        std::make_shared<detail::SubscriptionRegistry<detail::HandlerBinding>>()};
    // Count of executeVia() dispatches not yet resolved -- see pendingCalls().
    // Incremented once per call right before backend dispatch; decremented
    // exactly once by whichever of the two mutually-exclusive resolution
    // continuations (success or error) actually fires.
    //
    // Heap-allocated and shared for the same reason as `_subscriptions`
    // above: both resolution continuations decrement this from a context
    // that may outlive the Bridge, and a plain `std::atomic` member cannot be
    // touched safely once the Bridge is gone. A pinned counter needs no
    // liveness check at all to decrement safely -- see the continuations'
    // own comments. Never null.
    std::shared_ptr<std::atomic<std::size_t>> _pendingCalls{std::make_shared<std::atomic<std::size_t>>(0)};
    // Destroyed with the Bridge; the bridge's own in-flight continuations hold
    // weak `CallbackToken`s issued from it (see liveness()). Handlers do not:
    // gating a *call into the bridge* needs `_lifetime` below, not a token.
    ::morph::async::CallbackScope _callbacks;
    // Heap-allocated on purpose: a handler outliving this Bridge must still be
    // able to *ask* whether the bridge is there, so the answer cannot live in
    // the Bridge's own storage. Declaration position is irrelevant for the same
    // reason -- `~Bridge`'s first statement retires it explicitly, long before
    // any member is destroyed. See detail::BridgeLifetime and issue #486.
    std::shared_ptr<detail::BridgeLifetime> _lifetime{std::make_shared<detail::BridgeLifetime>()};
};

/// @brief `BridgeHandler` sharing policy: private, one instance per handler.
///
/// The default, and what every pre-existing call site gets. Such a handler
/// registers its own instance at construction and never enters the shared
/// directory, so two `BridgeHandler<M>` objects are two independent models —
/// byte-for-byte the behaviour morph has always had.
struct NoSharing {};

/// @brief `BridgeHandler` sharing policy: joins the shared instance directory.
///
/// A shared handler registers **nothing** at construction. It acquires an
/// instance the first time a keyed action or an explicit `attach()` names a
/// primary key, and every other `AllowShared` handler naming that same key —
/// in this process or, for a remote backend, in any other client — reaches the
/// same instance. Releasing the last such handler destroys it.
///
/// A shared handler that only ever runs *keyless* actions never attaches, and
/// its `execute` fails fast with "handler not bound": there is no instance to
/// run against and inventing a private one would silently defeat the sharing
/// the caller asked for. Attach first — see docs/spec/core/shared_instances.md.
struct AllowShared {};

/// @brief RAII wrapper that binds a single model type to a `Bridge`.
///
/// On construction, registers a `HandlerBinding` on the bridge. On destruction,
/// deregisters it automatically. The handler is non-copyable.
///
/// @par Instance subscriptions
/// Beyond the one-shot `execute(action) -> Completion<R>` API, a handler can
/// observe *the instance it is attached to*:
///
/// - `subscribe<R>(cb)` fires whenever an `R` is produced on that instance, by
///   any handler attached to it.
/// - `unsubscribe<R>()` drops the callback.
///
/// @tparam Model Concrete model type.
template <typename Model, typename Sharing = NoSharing>
// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions)
class BridgeHandler {
public:
    /// @brief Whether this handler participates in the shared instance directory.
    static constexpr bool kShared = std::is_same_v<Sharing, AllowShared>;

    /// @brief Constructs and registers the handler using the default model factory.
    ///
    /// @param bridge   The bridge to register on. Borrowed, not owned: it must
    ///                 outlive every *call* made on this handler. Destruction
    ///                 order itself is unconstrained, on any thread —
    ///                 `~BridgeHandler` takes the bridge's `detail::BridgeLifetime`
    ///                 gate and deregisters nothing if the bridge is already gone
    ///                 (see "Lifetime & ownership" in `docs/spec/core/bridge.md`).
    /// @param guiExec  Executor used to deliver `Completion` callbacks (e.g. the
    ///                 GUI thread). Borrowed: it must outlive this handler.
    BridgeHandler(Bridge& bridge MORPH_LIFETIMEBOUND, ::morph::exec::IExecutor* guiExec MORPH_LIFETIMEBOUND)
        : _bridge{bridge}, _bridgeGate{bridge.lifetimeGate()}, _guiExec{guiExec}, _binding{makeBinding(bridge)} {
        static_assert(!kShared || ::morph::model::KeyedModel<Model>,
                      "BridgeHandler<Model, AllowShared> requires Model to declare a PrimaryKey alias");
    }

    /// @brief Constructs the handler with a pre-built binding (for dependency injection).
    ///
    /// @param bridge   The bridge to register on. Borrowed, on the same terms as
    ///                 the constructor above.
    /// @param guiExec  Executor for callback delivery. Borrowed: it must outlive
    ///                 this handler.
    /// @param binding  Pre-built binding whose factory captures injected dependencies.
    BridgeHandler(Bridge& bridge MORPH_LIFETIMEBOUND, ::morph::exec::IExecutor* guiExec MORPH_LIFETIMEBOUND,
                  std::shared_ptr<detail::HandlerBinding> binding)
        : _bridge{bridge}, _bridgeGate{bridge.lifetimeGate()}, _guiExec{guiExec}, _binding{std::move(binding)} {
        _bridge.registerHandler(_binding);
    }

    /// @brief Deregisters the binding from the bridge.
    ///
    /// If the `Bridge` has already been destroyed, this is a no-op: there is
    /// nothing to deregister from and dereferencing the dangling `Bridge&` would
    /// be undefined behaviour. Destroying the bridge before its handlers is still
    /// discouraged, but it is safe — in either order, and **from any thread**.
    ///
    /// The gate is held across the whole call, not merely across the question.
    /// A handler is routinely released on a thread that is not the bridge's
    /// owner: the framework's own idiom for a fire-and-forget pass keeps the
    /// `shared_ptr<BridgeHandler>` alive inside the completions it dispatched,
    /// so the last reference is dropped wherever those completions are destroyed
    /// — a worker-pool thread for `LocalBackend`/`SimulatedRemoteBackend`, the
    /// transport thread for `SocketBackend`. A bare "is the bridge alive?" check
    /// answers for an instant that has already passed by the time the call is
    /// made, which is how issue #486 turned an ordinary `~App` into a
    /// use-after-free on `Bridge::deregisterHandler`'s `_handlers`. Holding the
    /// shared lock across the call makes the check and the call one step:
    /// `~Bridge` cannot start until this returns, and a `~Bridge` that started
    /// first is already visible here as "not alive".
    ~BridgeHandler() {
        std::shared_lock const gate{_bridgeGate->mtx};
        if (_bridgeGate->alive) {
            _bridge.deregisterHandler(_binding);
        }
    }

    BridgeHandler(const BridgeHandler&) = delete;
    BridgeHandler& operator=(const BridgeHandler&) = delete;

    /// @brief Dispatches @p action via the underlying `Bridge` and returns a `Completion`.
    ///
    /// The bridge's currently-installed default session (set once at startup or
    /// after login via `Bridge::setDefaultSession`) is attached automatically —
    /// callers never thread the session through individual call sites.
    ///
    /// @tparam Action Concrete action type registered with `BRIDGE_REGISTER_ACTION`.
    /// @param action Action to execute (moved into the dispatch).
    /// @return Completion that resolves on the GUI executor. A payload- or
    ///         result-keyed action's attach/promote step never throws out of
    ///         this call, even when the backend refuses it (e.g. a remote
    ///         server at `LimitPolicy::maxLiveModels`, a transport error, or
    ///         an unauthorized attach) — the failure is instead delivered
    ///         through the returned Completion's `.onError(...)`, exactly
    ///         like any other dispatch failure.
    template <typename Action>
    ::morph::async::Completion<typename ::morph::model::ActionTraits<Action>::Result> execute(Action action) {
        using R = ::morph::model::ActionTraits<Action>::Result;
        // Three mutually exclusive routes, chained rather than sequential: an
        // action is payload-keyed or result-keyed or neither (`PayloadKeyed`
        // and `ResultKeyed` differ only in `fromResult`, so no action can
        // satisfy both), and an unkeyed action — or any action at all on a
        // `NoSharing` handler — always lands in the final `else`. Chaining the
        // `if constexpr`s (rather than leaving the payload-keyed branch to
        // fall through to that `else`, as it did while the attach step was
        // synchronous) is what lets the keyed branches own their dispatch: the
        // attach now completes asynchronously, so the dispatch it precedes has
        // to happen from inside its completion callback, not on this stack.
        if constexpr (kShared && ::morph::model::detail::PayloadKeyed<Action>) {
            // The action names its instance: attach (or re-point) before
            // dispatching, so the call lands on the instance it asked for. A
            // remote backend's refusal (LimitPolicy::maxLiveModels, a
            // transport error, unauthorized) must surface through the
            // returned Completion's onError, exactly like every other
            // dispatch failure — not as a synchronous throw out of execute().
            //
            // The attach goes through Bridge::attachHandlerAsync, which
            // dispatches `IBackend::bindModel`: a backend with a genuinely
            // non-blocking attach settles later, one without settles inline,
            // having run the identical synchronous attach and called back
            // before returning — so a backend with no non-blocking path
            // still behaves synchronously here, just through the async
            // interface.
            auto state = std::make_shared<::morph::async::detail::CompletionState<R>>();
            ::morph::async::Completion<R> pending{state, _guiExec};
            auto* const bridgePtr = &_bridge;
            auto binding = _binding;
            // Key extraction is user code (ActionKeyTraits + keyToString), so it
            // is inside the same no-throw-out-of-execute() promise the attach
            // itself makes: a throw here resolves the Completion, it does not
            // escape.
            std::string key;
            try {
                key = ::morph::model::ActionKeyTraits<Action>::key(action);
            } catch (...) {
                state->setException(std::current_exception());
                return pending;
            }
            auto sharedAction = std::make_shared<Action>(std::move(action));
            bridgePtr->template attachHandlerAsync<Model>(
                binding, std::move(key),
                [bridgePtr, binding, sharedAction, state, guiExec = _guiExec](std::exception_ptr err) {
                    if (err) {
                        state->setException(err);
                        return;
                    }
                    bridgePtr->template executeVia<Model, Action>(binding, std::move(*sharedAction), guiExec)
                        .then([state](R value) { state->setValue(std::move(value)); })
                        .onError([state](std::exception_ptr exc) { state->setException(exc); });
                });
            return pending;
        } else if constexpr (kShared && ::morph::model::detail::ResultKeyed<Action>) {
            // The action *creates* the instance and its result carries the
            // generated key, exactly as a database insert returns its primary
            // key. Adopt it before any user callback observes the result, so a
            // .then() can immediately run further actions on the new instance.
            //
            // The action generates its own key, so it must run before the key
            // exists. Give the handler an anonymous instance to run on, then
            // promote *that* instance once the reply names it — re-pointing to a
            // fresh one instead would strand whatever the create just did.
            // Same async/fallback contract as the payload-keyed branch above,
            // via Bridge::ensureBoundAsync.
            auto state = std::make_shared<::morph::async::detail::CompletionState<R>>();
            ::morph::async::Completion<R> pending{state, _guiExec};
            auto* const bridgePtr = &_bridge;
            auto binding = _binding;
            auto sharedAction = std::make_shared<Action>(std::move(action));
            bridgePtr->ensureBoundAsync(
                binding, [bridgePtr, binding, sharedAction, state, guiExec = _guiExec](std::exception_ptr err) {
                    if (err) {
                        state->setException(err);
                        return;
                    }
                    bridgePtr
                        ->template executeVia<Model, Action>(
                            binding, std::move(*sharedAction), guiExec,
                            [bridgePtr, binding](const R& result) {
                                bridgePtr->template assignHandlerPrimary<Model>(
                                    binding, ::morph::model::ActionKeyTraits<Action>::template keyOfResult<R>(result));
                            })
                        .then([state](R value) { state->setValue(std::move(value)); })
                        .onError([state](std::exception_ptr exc) { state->setException(exc); });
                });
            return pending;
        } else {
            return _bridge.template executeVia<Model, Action>(_binding, std::move(action), _guiExec);
        }
    }

    /// @brief Attaches (or re-points) this handler to the instance for @p key.
    ///
    /// Creates the instance if no live instance holds @p key, otherwise joins the
    /// existing one. Re-pointing an already-attached handler releases the old
    /// instance, which survives only if another handler still holds it.
    ///
    /// The primary is deliberately **not** write-once: naming a different key is
    /// how a screen switches which entity it is looking at. Instances never
    /// change their own identity — the *handler* moves — so a key always maps to
    /// exactly one instance and no collision case can arise.
    ///
    /// @tparam M Defaulted to `Model`; never named explicitly. Present only so the
    ///         signature is instantiated lazily, since `PrimaryKeyOf` is
    ///         ill-formed for an unkeyed model.
    /// @param key Primary key of the instance to attach to.
    /// @throws std::runtime_error if the backend refuses the attach (e.g. a
    ///         remote server at `LimitPolicy::maxLiveModels`, a transport
    ///         error, or an unauthorized attach). `attach()` is a synchronous
    ///         `void` call with no `Completion` to route a failure through,
    ///         unlike `execute()`; a caller that wants the failure delivered
    ///         asynchronously should attach via a payload-keyed action's
    ///         `execute()` instead.
    template <typename M = Model>
    void attach(const ::morph::model::PrimaryKeyOf<M>& key)
        requires kShared
    {
        _bridge.template attachHandler<Model>(_binding, ::morph::model::keyToString(key));
    }

    /// @brief This handler's current primary key, or `nullopt` if unattached.
    /// @tparam M Defaulted to `Model`; never named explicitly. See `attach`.
    /// @return The attached key, or `nullopt` before the first attach.
    template <typename M = Model>
    [[nodiscard]] std::optional<::morph::model::PrimaryKeyOf<M>> primary()
        requires kShared
    {
        auto raw = _bridge.bindingPrimary(_binding);
        if (raw.empty()) {
            return std::nullopt;
        }
        return ::morph::model::keyFromString<::morph::model::PrimaryKeyOf<M>>(raw);
    }

    /// @brief Snapshot of the live shared instance keys for `Model`.
    ///
    /// Asynchronous even in local mode: the directory is backend state, and in
    /// remote mode answering costs a round trip. Returning a bare `std::vector`
    /// would work in-process and force a different call site everywhere else,
    /// breaking the local/remote symmetry the framework is built on.
    ///
    /// The result is a snapshot, stale the moment it arrives — another client may
    /// attach or release before the callback runs. Treat a returned key as "was
    /// live recently", never as a guarantee that a later `attach` finds the same
    /// instance.
    ///
    /// @tparam M Defaulted to `Model`; never named explicitly. See `attach`.
    /// @return Completion resolving on the GUI executor with the live keys.
    template <typename M = Model>
    [[nodiscard]] ::morph::async::Completion<std::vector<::morph::model::PrimaryKeyOf<M>>> instances()
        requires kShared
    {
        using Key = ::morph::model::PrimaryKeyOf<M>;
        auto state = std::make_shared<::morph::async::detail::CompletionState<std::vector<Key>>>();
        ::morph::async::Completion<std::vector<Key>> comp{state, _guiExec};
        try {
            std::vector<Key> keys;
            for (const auto& raw : _bridge.template listInstancesOf<Model>()) {
                keys.push_back(::morph::model::keyFromString<Key>(raw));
            }
            state->setValue(std::move(keys));
        } catch (...) {
            state->setException(std::current_exception());
        }
        return comp;
    }

    /// @brief Type-erased execute: looks up the action by its registered
    ///        string id and dispatches it through `ActionExecuteRegistry`.
    ///
    /// Use this only when the concrete `Action` type is not known at the
    /// call site (e.g. a schema-driven UI reading action names out of a
    /// JSON Schema at runtime). Prefer the templated `execute<Action>()`
    /// whenever the type is known at compile time.
    ///
    /// @param actionType Registered action type-id (the `NAME` passed to `BRIDGE_REGISTER_ACTION`).
    /// @param bodyJson   JSON-encoded action body.
    /// @return Completion resolving with the JSON-encoded result.
    /// @throws std::runtime_error if `actionType` was never registered for `Model`.
    [[nodiscard]] ::morph::async::Completion<std::string> executeJson(std::string_view actionType,
                                                                      std::string_view bodyJson) {
        // Dispatches through the executor registered for this handler's own
        // Sharing policy (see issue #68 / ActionExecuteRegistry::registerAction's
        // doc comment): a NoSharing-only executor would static_cast `this`
        // to the wrong BridgeHandler<Model, Sharing> instantiation for a
        // shared handler, silently skipping its attach/promote step.
        return ActionExecuteRegistry::instance().execute<Sharing>(
            std::string{::morph::model::ModelTraits<Model>::typeId()}, actionType, this, bodyJson);
    }

    /// @brief Creates this handler's binding: deferred when shared, immediate otherwise.
    /// @param bridge Bridge to create the binding on.
    /// @return The new binding.
    static std::shared_ptr<detail::HandlerBinding> makeBinding(Bridge& bridge) {
        if constexpr (kShared) {
            return bridge.template registerSharedHandler<Model>();
        } else {
            return bridge.template registerHandler<Model>();
        }
    }

    /// @brief The executor used to deliver this handler's `Completion` callbacks.
    /// @return The GUI/callback executor passed at construction.
    [[nodiscard]] ::morph::exec::IExecutor* guiExecutor() const noexcept { return _guiExec; }

    /// @brief Whether this handler currently has a live backend instance.
    ///
    /// A handler constructed against a backend that answers
    /// `BindWait::kCallerMustNotBlock` (see `backend.md`, "Waiting for a
    /// bind") starts unbound and only
    /// becomes bound once the deferred `onRegistered` fires; `execute()`
    /// fails fast with "handler not bound" for any call issued before that.
    /// This is the synchronous, point-in-time check; `whenBound()` below is
    /// the awaitable counterpart for a caller that wants to dispatch the
    /// moment registration settles rather than poll this in a loop.
    /// @return `true` if the handler is currently bound to a `ModelId`.
    [[nodiscard]] bool isBound() const noexcept { return Bridge::isBound(_binding); }

    /// @brief Resolves once this handler's initial registration settles.
    ///
    /// See `Bridge::whenBound()` for the full semantics: resolves
    /// immediately with `true` if already bound, waits for the in-flight
    /// async registration to settle (`true` on success, an error via
    /// `.onError(...)` on failure) if one is outstanding, or resolves
    /// immediately with `false` if nothing is in flight and the handler is
    /// still unbound.
    /// @return `Completion<bool>` delivered on this handler's GUI executor.
    [[nodiscard]] ::morph::async::Completion<bool> whenBound() { return _bridge.whenBound(_binding, _guiExec); }

    /// @brief Subscribes to results of type @p R produced on the attached instance.
    ///
    /// Fires whenever an `R` is produced on the instance this handler is
    /// attached to — by this handler, by another handler sharing the instance,
    /// or by another screen entirely. The subscriber names *what it renders*,
    /// not what somebody else must call to produce it, so adding an action that
    /// also yields an `R` never breaks an existing subscriber.
    ///
    /// One callback per `(handler, R)`: subscribing again replaces the previous
    /// one. Callbacks are delivered on this handler\'s executor. Failed actions
    /// notify nobody; delivery is best-effort and unbuffered, with no replay.
    ///
    /// @tparam R Result/state type to observe.
    /// @param cb Callable receiving the value by value on the GUI executor.
    template <typename R>
    void subscribe(std::function<void(R)> cb) {
        _bridge.addSubscription(
            _binding, std::type_index{typeid(R)},
            [cb = std::move(cb)](const std::any& boxed) { cb(std::any_cast<const R&>(boxed)); }, _guiExec);
    }

    /// @brief Subscribes to results of type @p R, gated on @p scope.
    ///
    /// Same subscription as `subscribe<R>(cb)` — one callback per `(handler,
    /// R)`, delivered on this handler's executor — except that @p cb runs only
    /// while @p scope is alive and un-stopped. Subscription sinks are the
    /// longest-lived callbacks in the system (they fire repeatedly, for as long
    /// as the handler exists), so they are the attachments most likely to
    /// outlive the screen that installed them.
    ///
    /// The sink itself is *not* pruned when the scope goes inactive: delivery is
    /// refused, the subscription entry stays until `unsubscribe<R>()` or handler
    /// destruction removes it.
    ///
    /// @tparam R Result/state type to observe.
    /// @param scope Receiver-owned gate; observed weakly, and only its current
    ///              generation is captured (a later `reset()` retires this sink).
    /// @param cb    Callable receiving the value by value on the GUI executor.
    template <typename R>
    void subscribe(const ::morph::async::CallbackScope& scope, std::function<void(R)> cb) {
        subscribe<R>(scope.token(), std::move(cb));
    }

    /// @brief Subscribes to results of type @p R, gated on an already-issued @p token.
    ///
    /// The token-taking form of `subscribe<R>(const CallbackScope&, cb)`.
    ///
    /// @tparam R Result/state type to observe.
    /// @param token Gate observing some receiver's `CallbackScope`. A
    ///              default-constructed token suppresses every delivery.
    /// @param cb    Callable receiving the value by value on the GUI executor.
    template <typename R>
    void subscribe(::morph::async::CallbackToken token, std::function<void(R)> cb) {
        subscribe<R>(std::function<void(R)>{token.guard(std::move(cb))});
    }

    /// @brief Removes this handler\'s subscription for @p R.
    /// @tparam R Result/state type to stop hearing about.
    template <typename R>
    void unsubscribe() {
        _bridge.removeSubscription(_binding, std::type_index{typeid(R)});
    }

    /// @brief Returns the underlying `HandlerBinding`.
    ///
    /// @return Shared pointer to the binding owned by this handler — a reference
    ///         into the handler, valid only for as long as it is. Copy it to
    ///         keep the binding past that point.
    [[nodiscard]] const std::shared_ptr<detail::HandlerBinding>& binding() const MORPH_LIFETIMEBOUND {
        return _binding;
    }

private:
    Bridge& _bridge;
    // Shared gate that answers "is `_bridge` still there?" *and* keeps that
    // answer true for as long as the destructor below holds it -- see
    // detail::BridgeLifetime. Never null.
    std::shared_ptr<detail::BridgeLifetime> _bridgeGate;
    ::morph::exec::IExecutor* _guiExec;
    std::shared_ptr<detail::HandlerBinding> _binding;
};

/// Out-of-line definition of ActionExecuteRegistry::registerAction.
/// Placed here after BridgeHandler is fully defined so we can safely cast and call its methods.
///
/// Builds one executor per `Sharing` policy the framework defines
/// (`NoSharing`, `AllowShared`) from the same generic-lambda template,
/// `static_cast`ing `handlerVoid` to the matching `BridgeHandler<Model,
/// Sharing>*` in each — see issue #68 / bridge.md's design-decision entry for
/// why a single `NoSharing`-only executor is unsound for a shared handler.
template <typename Model, typename Action>
inline void ActionExecuteRegistry::registerAction(std::string_view modelId, std::string_view actionId) {
    auto makeExecutor = []<typename Sharing>() {
        return [](void* handlerVoid, std::string_view bodyJson) -> ::morph::async::Completion<std::string> {
            auto* handler = static_cast<BridgeHandler<Model, Sharing>*>(handlerVoid);
            auto resultState = std::make_shared<::morph::async::detail::CompletionState<std::string>>();
            try {
                Action action = ::morph::model::ActionTraits<Action>::fromJson(bodyJson);
                // Retag any Quantity fields to their declared precision so the stored
                // value matches the schema's advertised `x-decimalPlaces`, rather than
                // silently keeping whatever runtime `dp` the client sent. No-op for
                // actions with no Quantity members. See docs/spec/forms/forms.md.
                ::morph::forms::reconcileDeclaredPrecision(action);
                // Pre-decode wire validation seam: reject a Quantity field whose
                // engaged value falls outside its unit's declared bounds
                // (UnitTraits<E>::bounds), before the validator/business-rule
                // check below. No-op for actions with no Quantity members, or
                // whose units declare no bounds(). See docs/spec/forms/forms.md,
                // "Pre-decode wire validation". Thrown as QuantityDecodeError,
                // caught by the same catch block as every other decode/validation
                // failure on this path.
                ::morph::forms::enforceQuantityBounds(action);
                // Overwrite any computed fields from their declared inputs -- a
                // computed field is never trusted from the client, on any path.
                // No-op for actions with no computedFields. See docs/spec/forms/forms.md.
                ::morph::forms::recomputeAll(action);
                // Enforce the action's validator on the request/reply dispatch path,
                // just as the reactive `set<>` path does in
                // `morph::flows::FlowSession::set<>` (forms/flows.hpp), which fires a
                // step only once its `ActionValidator` is ready. Without this, a
                // submitted action that fails its readiness/validity check
                // (empty required Quantity, out-of-range field, …) would reach the
                // handler and either produce a silently wrong result or force every
                // handler to re-check by hand. `ActionValidator<Action>::ready`
                // auto-detects a `bool validate() const` member and defaults to
                // `true` for actions with no validator, so this is a no-op for
                // unvalidated actions and a hard gate for validated ones. An invalid
                // action resolves the completion through `setException` (a proper
                // error reply upstream), never a bad execution.
                if (!::morph::model::ActionValidator<Action>::ready(action)) {
                    throw std::invalid_argument{"action failed validation: " +
                                                std::string{::morph::model::ActionTraits<Action>::typeId()}};
                }
                handler
                    ->template execute<Action>(std::move(action))
                    // NOLINTNEXTLINE(performance-unnecessary-value-param) — lambda captures the result by value
                    .then([resultState](auto result) {
                        // Guard the JSON forwarding for the same reason as executeVia:
                        // a throwing resultToJson (or the move it does) must land on
                        // the error sink, not escape the callback executor and either
                        // hang the completion or terminate the Qt loop.
                        try {
                            resultState->setValue(::morph::model::ActionTraits<Action>::resultToJson(result));
                        } catch (...) {
                            resultState->setException(std::current_exception());
                        }
                    })
                    .onError([resultState](const std::exception_ptr& err) { resultState->setException(err); });
            } catch (...) {
                resultState->setException(std::current_exception());
            }
            return {resultState, handler->guiExecutor()};
        };
    };
    _executors[Key{std::string{modelId}, std::string{actionId}, std::type_index{typeid(NoSharing)}}] =
        makeExecutor.template operator()<NoSharing>();
    _executors[Key{std::string{modelId}, std::string{actionId}, std::type_index{typeid(AllowShared)}}] =
        makeExecutor.template operator()<AllowShared>();
}

}  // namespace morph::bridge
