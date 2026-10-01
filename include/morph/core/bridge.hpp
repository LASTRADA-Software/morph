// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <any>
#include <atomic>
#include <cassert>
#include <chrono>
#include <concepts>
#include <core/async/StopToken.hpp>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
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
#include "detail/owner_affinity.hpp"
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
        // See `ActionDispatcher::dispatch` -- the registration-phase latch,
        // closed on the first read of a process-level registry so a later
        // registration can assert. Debug builds only.
        ::morph::model::detail::noteRegistryRead(this == &instance());
        auto iter = _executors.find(
            KeyView{.modelId = modelId, .actionId = actionId, .sharing = std::type_index{typeid(Sharing)}});
        if (iter == _executors.end()) {
            throw std::runtime_error("unknown action for executeJson: " + std::string{modelId} + "/" +
                                     std::string{actionId});
        }
        return iter->second(handler, bodyJson);
    }

    /// @brief Checks whether an executor is registered for `(modelId, actionId)`,
    ///        specialised for the caller's own `Sharing` policy, without invoking it.
    ///
    /// `registerAction` always files both the `NoSharing` and `AllowShared`
    /// executors for a given `(Model, Action)` pair (see its own doc comment),
    /// so for any action registered via `BRIDGE_REGISTER_ACTION` this answers
    /// the same for either `Sharing` -- the template parameter exists for
    /// symmetry with `execute<Sharing>` and to stay correct if that ever
    /// changes, not because the two currently disagree.
    /// @tparam Sharing `NoSharing` or `AllowShared` — the caller's own sharing policy.
    /// @param modelId  String id of the target model.
    /// @param actionId String id of the action to check.
    /// @return `true` if `execute<Sharing>(modelId, actionId, ...)` would find an executor.
    template <typename Sharing>
    [[nodiscard]] bool contains(std::string_view modelId, std::string_view actionId) const noexcept {
        // Same registration-phase latch `execute` closes (see its own doc
        // comment): this is a read of `_executors` too, so a registration
        // racing a routing-only caller that never calls `execute` (e.g. one
        // that only ever probes unrouted actions) must still be caught.
        ::morph::model::detail::noteRegistryRead(this == &instance());
        return _executors.contains(
            KeyView{.modelId = modelId, .actionId = actionId, .sharing = std::type_index{typeid(Sharing)}});
    }

    /// @brief Returns the process-level singleton registry.
    /// @return Reference to the singleton `ActionExecuteRegistry`.
    static ActionExecuteRegistry& instance();

private:
    // The stored key: the registry owns its ids, because nothing else outlives
    // a registration.
    struct Key {
        std::string modelId;
        std::string actionId;
        std::type_index sharing;
        bool operator==(const Key&) const = default;
    };
    // The key a caller looks an entry up *with*. Every id reaching `execute`
    // arrives as a `string_view` -- a schema-driven GUI's decoded action name,
    // or a `constexpr` `ModelTraits<M>::typeId()` -- so materialising `Key`
    // just to hash it would charge every `executeJson` two `std::string`
    // constructions. Deliberately not
    // `morph::model::detail::PairKeyView`: this key carries a `std::type_index`
    // as well as the two ids, so it needs its own view type and its own
    // functors rather than a reuse that would silently drop the sharing tag.
    struct KeyView {
        std::string_view modelId;
        std::string_view actionId;
        std::type_index sharing;
    };
    struct KeyHash {
        // Marks the functor transparent; `KeyEqual` below is the other half
        // `unordered_map` needs before a heterogeneous `find` compiles at all.
        using is_transparent = void;

        std::size_t operator()(KeyView key) const noexcept {
            // Spelled with the view type rather than a braced list: `PairKeyHash`
            // is transparent now, so a braced `{modelId, actionId}` is equally
            // convertible to both of its overloads and would be ambiguous.
            std::size_t const modelHash =
                ::morph::model::detail::PairKeyHash{}(::morph::model::detail::PairKeyView{key.modelId, key.actionId});
            return modelHash ^ (key.sharing.hash_code() + 0x9e3779b9U + (modelHash << 6) + (modelHash >> 2));
        }

        // Routed through the view body rather than hashing the stored strings
        // separately, for the reason `PairKeyHash`'s own comment gives: two
        // bodies that happen to agree are not a property a heterogeneous
        // lookup may rest on. If they disagreed, a transparent `find()` would
        // hash into the wrong bucket and report a registered action as
        // unknown, with no diagnostic anywhere.
        std::size_t operator()(const Key& key) const noexcept {
            return (*this)(KeyView{.modelId = key.modelId, .actionId = key.actionId, .sharing = key.sharing});
        }
    };
    struct KeyEqual {
        // Marks the functor transparent, enabling heterogeneous lookup. A
        // transparent hash alone is not enough: `unordered_map` requires both
        // before a heterogeneous `find` compiles, and naming only the hash
        // leaves every lookup silently materialising a `Key`.
        using is_transparent = void;

        // Accepts any mix of `Key` and `KeyView` on either side.
        template <typename LhsT, typename RhsT>
        bool operator()(const LhsT& lhs, const RhsT& rhs) const noexcept {
            return lhs.sharing == rhs.sharing && std::string_view{lhs.modelId} == std::string_view{rhs.modelId} &&
                   std::string_view{lhs.actionId} == std::string_view{rhs.actionId};
        }
    };
    std::unordered_map<Key, Executor, KeyHash, KeyEqual> _executors;
};

inline ActionExecuteRegistry& ActionExecuteRegistry::instance() {
    static ActionExecuteRegistry inst;
    return inst;
}

}  // namespace morph::bridge

namespace morph::model::detail {

template <typename Model, typename Action>
inline bool registerActionExecutorOnce(std::string_view modelId, std::string_view actionId) noexcept {
    // clang-tidy's misc-static-assert fires on any `assert(!f())` whose `f` takes no
    // argument, whether or not `f` is constexpr -- and this one loads an atomic.
    // Applying the check's own fix does not compile: `static_assert` on this
    // condition is "static assertion expression is not an integral constant
    // expression / non-constexpr function 'registrationPhaseClosed' cannot be used
    // in a constant expression".
    //
    // Last re-checked at clang-tidy 22.1.8, the version CI pins: the diagnostic
    // still fires and the suggested fix still does not compile. The stamp is here
    // because nothing re-checks it for you -- no script flags a directive that has
    // stopped suppressing anything, and nothing triggers a re-read on a
    // CLANG_VERSION bump. Re-read this when the pin moves; if the finding is gone,
    // delete all four directives together.
    // NOLINTNEXTLINE(misc-static-assert,cert-dcl03-c)
    assert(!::morph::model::registrationPhaseClosed() &&
           "registerActionExecutorOnce: registration after the registration phase closed. The "
           "process-level registries are unsynchronised and are read-only once dispatch begins -- "
           "registering now races their internals against concurrent lookups (see "
           "docs/spec/core/registry.md, \"Thread safety\"). Load and register plugin modules before "
           "the first dispatch.");
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
/// Shared between `Bridge` and `BridgeHandler`. Every field is touched only on
/// the bridge's owner except `currentId`, an atomic so `isBound()` can be asked
/// from anywhere.
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
    /// can attach a log to the instance it creates. Set it before the binding
    /// is registered.
    std::string contextKey;

    /// @brief Canonical string encoding of this instance's primary key.
    ///
    /// Empty until a keyed action or an explicit `attach` supplies one. Only
    /// meaningful when `shared` is set; a private binding never consults it.
    std::string primary;

    /// @brief Whether this binding participates in the shared instance directory.
    ///
    /// Set once at construction from `BridgeHandler`'s `Sharing` template
    /// argument and never changed. A shared binding acquires no instance until
    /// it has a primary.
    bool shared = false;

    /// @brief Current `ModelId` value in the active backend (0 = unbound).
    std::atomic<uint64_t> currentId{0};

    /// @brief Whether a bind issued for this binding has not settled yet.
    bool bindInFlight = false;

    /// @brief Counts the binds issued for this binding. A reply carrying an
    ///        older count was superseded — by a switch, a reconnect, or the
    ///        handler's destruction — and is released rather than applied.
    std::uint64_t bindGeneration = 0;

    /// @brief The last bind's failure, or null. What a held call is rejected
    ///        with when the bind it waited for failed.
    std::exception_ptr bindFailure;

    /// @brief The continuation of the operation that issued the bind in flight
    ///        (an attach, a first bind for a result-keyed action), run when it
    ///        settles, before `waiting`. Null to proceed, an error to give up.
    std::function<void(std::exception_ptr)> afterBind;

    /// @brief Calls held until no bind is in flight, in the order they were
    ///        made. Each is resumed with null to proceed, or with the error it
    ///        is rejected with.
    std::deque<std::function<void(std::exception_ptr)>> waiting;
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

/// @brief A bind's outcome, taken from a `Completion` that settled before the
///        call that returned it came back.
struct BindOutcome {
    /// @brief The bound instance; meaningful only without a failure.
    ::morph::exec::detail::ModelId id{};
    /// @brief The failure, or null.
    std::exception_ptr failure;
};

/// @brief Takes @p completion's outcome if it has already settled.
///
/// How the bind rule's "a backend that can settles it before returning" is
/// honoured: the caller applies the outcome in the same call, so a handler
/// over such a backend is bound when its constructor returns. Taking a failure
/// counts as handling it, so the state logs no orphan.
/// @param completion A `bindModel`/`promoteModel` result, just returned.
/// @return The outcome, or `std::nullopt` while the backend has not settled it.
inline std::optional<BindOutcome> takeSettled(
    const ::morph::async::Completion<::morph::exec::detail::ModelId>& completion) {
    auto const state = completion.state();
    if (state == nullptr) {
        return BindOutcome{.id = {}, .failure = std::make_exception_ptr(std::runtime_error{"empty bind completion"})};
    }
    // `ready` publishes the write-once outcome, so reading it here needs no
    // hop to the completion's executor.
    if (!state->ready.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    if (state->value) {
        return BindOutcome{.id = *state->value, .failure = nullptr};
    }
    state->onErrAttached.store(true, std::memory_order_relaxed);
    return BindOutcome{.id = {}, .failure = state->error};
}

/// @brief The typed completion state a backend settles directly.
///
/// This object *is* the typed completion state `Bridge::executeVia` hands the
/// caller, and is also the `ISettleSink` the backend settles: one allocation
/// per dispatch rather than a typed completion plus an erased one with a
/// forwarding `.then`/`.onError` pair between them.
///
/// @par What the settle path holds
/// - **The deadline disarm happens first**, before any other settle work, so a
///   slow `onResult`/`publishResult` cannot give the timer a window to resolve
///   this completion with `ClientTimeoutError` while the real result is in
///   hand — `settleOnce`, called at the top of both settle methods.
/// - **`_pendingCalls` is decremented on exactly one of two mutually exclusive
///   paths**, whether or not the work that follows then throws — `settleOnce`
///   again, which is also what makes a `cancelPending` racing a reply decrement
///   once rather than twice.
/// - **Bridge-side work runs on the owner.** A backend settles on its own
///   thread. When the result has work to do on the bridge — `onResult` (a
///   result-keyed action's promotion) or a subscription fan-out — the rest of
///   the settle is posted to the bridge's owner and done there, gated on the
///   bridge's `CallbackToken`; the value reaches the caller after it. Without
///   such work the value is forwarded at once, where the backend settled.
/// - **The value forwarding is guarded**, so a throwing move of `R` routes to
///   this state's own error sink instead of escaping the settling thread.
///
/// @tparam R The action's result type.
template <typename R>
class BridgeSink final : public ::morph::async::detail::CompletionState<R>,
                         public ::morph::async::detail::ISettleSink {
public:
    /// @brief Builds the sink for one dispatch.
    /// @param onResult      Optional observer run on the owner on the typed
    ///                      result, before it is moved into this state and
    ///                      before the caller's `.then`.
    /// @param pendingCalls  The bridge's pinned in-flight counter.
    /// @param subscriptions The bridge's pinned subscription registry; only its
    ///                      atomic `hasSubscribers()` is read off the owner.
    /// @param liveness      The bridge's token: nothing bridge-side runs once
    ///                      it has expired.
    /// @param owner         The bridge's owner.
    BridgeSink(std::function<void(const R&)> onResult, std::shared_ptr<std::atomic<std::size_t>> pendingCalls,
               std::shared_ptr<SubscriptionRegistry<HandlerBinding>> subscriptions,
               ::morph::async::CallbackToken liveness, ::morph::exec::detail::OwnerAffinity owner)
        : _onResult{std::move(onResult)},
          _pendingCalls{std::move(pendingCalls)},
          _subscriptions{std::move(subscriptions)},
          _liveness{std::move(liveness)},
          _owner{owner} {}

    /// @brief Names the instance this dispatch targets, for the fan-out.
    ///        Called on the owner before the dispatch.
    /// @param mid The instance.
    void target(::morph::exec::detail::ModelId mid) noexcept { _mid = mid; }

    /// @brief Hands the sink the deadline entry it must disarm when it settles.
    ///
    /// Called by `executeVia` before the dispatch, so the write is sequenced
    /// before anything that could settle this sink on another thread. The
    /// scheduler is held as a `shared_ptr` copy, so `cancel()` stays safe from
    /// a settle that outlives the bridge.
    ///
    /// @param handle    The scheduled entry.
    /// @param scheduler The scheduler that owns it.
    void armDeadline(::morph::async::detail::TimeoutScheduler::Handle handle,
                     std::shared_ptr<::morph::async::detail::TimeoutScheduler> scheduler) {
        _deadlineHandle = handle;
        _schedulerRef = std::move(scheduler);
    }

    /// @brief Whether the state already settled — a deadline that fired while
    ///        the call waited for its bind.
    /// @return True once settled.
    [[nodiscard]] bool alreadySettled() const { return this->ready.load(std::memory_order_acquire); }

    /// @brief Undoes `armDeadline` and the pending count for a dispatch that
    ///        never started, because `IBackend::executeInto` threw or the
    ///        deadline settled the call while it waited for its bind.
    ///
    /// Routed through `settleOnce` so it cannot double-count against a sink
    /// the backend had already settled before it threw.
    void abandon() {
        if (settleOnce()) {
            _pendingCalls->fetch_sub(1, std::memory_order_relaxed);
        }
    }

    /// @brief Settles with the backend's opaque result.
    /// @param opaque The result, which must point at an `R`. Spelled
    ///        `opaque` rather than `value` only because `CompletionState`
    ///        already has a member of that name, and -Wshadow-field is on.
    void settleValue(std::shared_ptr<void> opaque) override {
        if (!settleOnce()) {
            return;
        }
        _pendingCalls->fetch_sub(1, std::memory_order_relaxed);
        bool bridgeWork = static_cast<bool>(_onResult);
        if constexpr (std::is_copy_constructible_v<R>) {
            bridgeWork = bridgeWork || _subscriptions->hasSubscribers();
        }
        if (!bridgeWork) {
            forward(opaque, /*onOwner=*/false);
            return;
        }
        if (_owner.here()) {
            forward(opaque, /*onOwner=*/true);
            return;
        }
        auto self = std::static_pointer_cast<BridgeSink>(this->shared_from_this());
        _owner.owner().post([self = std::move(self), opaque = std::move(opaque)] { self->forward(opaque, true); });
    }

    /// @brief Settles with a failure from the backend.
    /// @param exc The exception to deliver to the caller's `.onError`.
    void settleException(const std::exception_ptr& exc) override {
        if (settleOnce()) {
            _pendingCalls->fetch_sub(1, std::memory_order_relaxed);
        }
        // First result wins: a late failure after a settle is dropped by the
        // state itself.
        this->setException(exc);
    }

private:
    /// @brief Runs the bridge-side work, when on the owner and the bridge is
    ///        still there, then hands the value to the caller.
    /// @param opaque  The backend's result.
    /// @param onOwner Whether this runs on the bridge's owner.
    void forward(const std::shared_ptr<void>& opaque, bool onOwner) {
        try {
            auto* const typedResult = static_cast<R*>(opaque.get());
            if (onOwner && _liveness.active()) {
                if (_onResult) {
                    _onResult(*typedResult);
                }
                if constexpr (std::is_copy_constructible_v<R>) {
                    if (_subscriptions->hasSubscribers()) {
                        _owner.note("Bridge::publishResult");
                        _subscriptions->publishResult(_mid, std::type_index{typeid(R)}, std::any{*typedResult});
                    }
                }
            }
            this->setValue(std::move(*typedResult));
        } catch (...) {
            this->setException(std::current_exception());
        }
    }

    /// @brief Claims the one-time settle work; `true` for exactly one caller.
    ///
    /// The deadline disarm and the `_pendingCalls` decrement happen once per
    /// dispatch. `IBackend::cancelPending` settles a sink from one thread while
    /// a reply may be settling it from another, so the latch is explicit.
    /// @return `true` if this call claimed the settle, `false` if another did.
    bool settleOnce() {
        if (_settled.test_and_set(std::memory_order_acq_rel)) {
            return false;
        }
        if (_deadlineHandle && _schedulerRef) {
            try {
                _schedulerRef->cancel(*_deadlineHandle);
            } catch (...) {  // NOLINT(bugprone-empty-catch)
                // Best-effort by contract: a deadline callback already running
                // still settles, and this state discards it -- first result
                // wins. That, not the disarm, is what makes the race harmless.
            }
        }
        return true;
    }

    ::morph::exec::detail::ModelId _mid{};
    std::function<void(const R&)> _onResult;
    std::shared_ptr<std::atomic<std::size_t>> _pendingCalls;
    std::shared_ptr<SubscriptionRegistry<HandlerBinding>> _subscriptions;
    ::morph::async::CallbackToken _liveness;
    ::morph::exec::detail::OwnerAffinity _owner;
    std::optional<::morph::async::detail::TimeoutScheduler::Handle> _deadlineHandle;
    std::shared_ptr<::morph::async::detail::TimeoutScheduler> _schedulerRef;
    std::atomic_flag _settled = ATOMIC_FLAG_INIT;
};

/// @brief `ActionCall::localOpAsync` for an action whose handler returns
///        `core::async::Task`: the local-path twin of `localOp`.
///
/// Recomputes the action's computed fields and enforces its validator exactly
/// as `localOp` does, starts the handler on the model's strand, and -- when its
/// Task completes -- journals the outcome and reports it through @p done. See
/// `docs/spec/core/coroutines.md`. Declared in every build, so `executeVia` can
/// name it; a `MORPH_CLIENT_ONLY` build never instantiates it.
/// @tparam Model  Concrete model type.
/// @tparam Action Concrete action type.
/// @param holder      The model instance; kept alive by @p done's owner until it has run.
/// @param actionOwner The action, owned: the handler's frame outlives this call.
/// @param executor    The handler's resumer, on the model's strand.
/// @param token       The stop token the handler observes.
/// @param done        Called exactly once, on the strand.
template <typename Model, typename Action>
// Validation, the handler call, and the journalling of each outcome, in the
// order localOp keeps them; splitting it would put that order in two places.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void localTaskOp(::morph::model::detail::IModelHolder& holder, std::shared_ptr<void> actionOwner,
                 const std::shared_ptr<::morph::exec::detail::TaskResumer>& executor, ::core::async::StopToken token,
                 ::morph::backend::detail::ActionCall::LocalDone done) {
    using R = ::morph::model::ActionTraits<Action>::Result;
    using Handler = decltype(std::declval<Model&>().execute(std::declval<Action&>()));
    Action& actionRef = *static_cast<Action*>(actionOwner.get());
    Handler task;
    try {
        ::morph::forms::recomputeAll(actionRef);
        if (!::morph::model::ActionValidator<Action>::ready(actionRef)) {
            throw ::morph::model::ValidationError{::morph::model::ModelTraits<Model>::typeId(),
                                                  ::morph::model::ActionTraits<Action>::typeId()};
        }
        task = holder.template into<Model>().execute(actionRef);
    } catch (...) {
        done(nullptr, std::current_exception());
        return;
    }
    ::morph::model::detail::startTaskHandler<R>(
        executor, std::move(task), std::move(token),
        [&holder, actionOwner = std::move(actionOwner), done = std::move(done)](std::optional<R> result,
                                                                                const std::exception_ptr& error) {
            // Read only to journal the outcome, which a Loggable::No action
            // compiles out.
            static_cast<void>(holder);
            const Action& action = *static_cast<const Action*>(actionOwner.get());
            if (error) {
                // The handler failed, so the action was rejected: recorded
                // Outcome::Failed, as localOp records a throw from
                // Model::execute. A journal write that throws here fails the
                // call with the handler's own exception still.
                try {
                    if constexpr (::morph::model::detail::actionLoggable<Action>() == ::morph::model::Loggable::Yes) {
                        if (holder.hasActionLog()) {
                            try {
                                std::rethrow_exception(error);
                            } catch (const std::exception& exc) {
                                ::morph::model::detail::recordActionFailure(
                                    holder, std::string{::morph::model::ModelTraits<Model>::typeId()},
                                    std::string{::morph::model::ActionTraits<Action>::typeId()},
                                    ::morph::model::ActionTraits<Action>::toJson(action),
                                    ::morph::model::detail::actionPayloadSchema<Action>(), exc.what());
                            } catch (...) {  // NOLINT(bugprone-empty-catch): as localOp, below
                                // Not a std::exception: recorded nowhere, as localOp's
                                // rethrow leaves such a throw unrecorded.
                            }
                        }
                    }
                } catch (...) {  // NOLINT(bugprone-empty-catch): the handler's exception is reported
                }
                done(nullptr, error);
                return;
            }
            // The Task completed, so the model's mutation has committed: as in
            // localOp, failing to serialise the result or to record it is an
            // ActionRecordingError, never a rejection.
            std::shared_ptr<void> value;
            std::string resultJson;
            try {
                auto typed = std::make_shared<R>(std::move(*result));
                if constexpr (::morph::model::detail::actionLoggable<Action>() == ::morph::model::Loggable::Yes) {
                    if (holder.hasActionLog()) {
                        resultJson = ::morph::model::ActionTraits<Action>::resultToJson(*typed);
                        ::morph::model::detail::recordActionSuccess(
                            holder, std::string{::morph::model::ModelTraits<Model>::typeId()},
                            std::string{::morph::model::ActionTraits<Action>::typeId()},
                            ::morph::model::ActionTraits<Action>::toJson(action),
                            ::morph::model::detail::actionPayloadSchema<Action>(), resultJson);
                    }
                }
                value = std::move(typed);
            } catch (const std::exception& exc) {
                done(nullptr,
                     std::make_exception_ptr(::morph::model::ActionRecordingError{std::move(resultJson), exc.what()}));
                return;
            } catch (...) {
                done(nullptr, std::current_exception());
                return;
            }
            done(std::move(value), nullptr);
        });
}

}  // namespace detail

/// @brief Central dispatcher that routes typed actions to an `IBackend`.
///
/// `Bridge` owns exactly one active backend at a time. It tracks all registered
/// `HandlerBinding` instances and re-registers them when `switchBackend()` is
/// called or the backend reconnects, enabling seamless local ↔ remote
/// transitions.
///
/// @par One owner
/// A `Bridge`, its handlers and its backend belong to the executor given at
/// construction. Every member function is called there — inside a task of the
/// owner, or on the thread that constructed the bridge outside the owner's
/// tasks (a GUI owner's own thread) — and every field is touched only there,
/// so nothing here takes a lock. A call from anywhere else is a contract
/// violation, asserted in a debug build. What reaches the bridge from other
/// threads arrives as a task on the owner: bind and promote replies (the owner
/// is the executor `bindModel`/`promoteModel` deliver on), reconnects (the
/// executor the reconnect handler is posted to), and the bridge-side work an
/// action's result triggers. `pendingCalls()`, `isBound()` and
/// `hasSubscribers()` read atomics and may be asked from any thread. See
/// `docs/spec/core/bridge.md`, "Thread safety — one owner".
///
/// @par The bind rule
/// The bind is a `Completion` delivered on the owner; a backend that can
/// settles it before returning, and that outcome is applied at once. `execute`
/// dispatches immediately when the binding's `currentId` is set and no bind is
/// in flight, and is otherwise held by the binding and dispatched when the bind
/// settles.
class Bridge {
public:
    /// @brief Constructs a bridge that dispatches through @p backend, owned by @p owner.
    ///
    /// Tells the backend its owner, installs a reconnect handler (posted to
    /// @p owner) so a backend with a recoverable transport re-registers every
    /// live handler on the owner after a reconnect, and pushes the (initially
    /// empty) default session to the backend.
    ///
    /// @param backend Initial backend. Ownership is transferred.
    /// @param owner   The executor this bridge, its handlers and its backend
    ///                belong to. Construct the bridge there: inside one of its
    ///                tasks, or on the thread that runs them. Borrowed: it must
    ///                outlive this bridge and every reply still in flight when
    ///                the bridge is destroyed.
    Bridge(std::unique_ptr<::morph::backend::detail::IBackend> backend,
           ::morph::exec::IExecutor& owner MORPH_LIFETIMEBOUND)
        : _affinity{owner}, _backend{std::shared_ptr<::morph::backend::detail::IBackend>(std::move(backend))} {
        if (_backend) {
            _backend->setOwner(_affinity);
            installReconnectHandler(_backend);
            _backend->setSession(_defaultSession);
        }
    }

    /// @brief Clears the backend's reconnect handler, rejects every call still
    ///        waiting for a bind with `BridgeDestroyedError`, and cancels the
    ///        backend's pending calls with the same error.
    ///
    /// On the owner. A reply or reconnect still queued there finds the
    /// bridge's token expired and touches nothing; a bind reply for a binding
    /// the bridge no longer tracks releases its instance on the backend that
    /// issued it.
    ~Bridge() {
        note("Bridge::~Bridge");
        auto const destroyed = std::make_exception_ptr(::morph::backend::BridgeDestroyedError{});
        if (_backend) {
            _backend->setReconnectHandler(nullptr, nullptr);
        }
        std::vector<std::function<void(std::exception_ptr)>> rejected;
        for (auto const& weak : _handlers) {
            if (auto binding = weak.lock()) {
                ++binding->bindGeneration;
                binding->bindInFlight = false;
                takeHeld(*binding, rejected);
            }
        }
        for (auto& resume : rejected) {
            resume(destroyed);
        }
        if (_backend) {
            _backend->cancelPending(destroyed);
        }
    }

    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;
    Bridge(Bridge&&) = delete;
    Bridge& operator=(Bridge&&) = delete;

    /// @brief The executor this bridge belongs to.
    /// @return The owner given at construction.
    [[nodiscard]] ::morph::exec::IExecutor& owner() const noexcept { return _affinity.owner(); }

    /// @brief Whether the calling thread is on this bridge's owner.
    /// @return True inside one of the owner's tasks, or on the thread that
    ///         constructed the bridge outside them.
    [[nodiscard]] bool onOwner() const noexcept { return _affinity.here(); }

    /// @brief Creates and registers a new `HandlerBinding` for `Model`.
    ///
    /// Uses the default `ModelFactory::create<Model>()` factory. The bind is
    /// issued and this returns; see `registerHandler(binding)`.
    /// @tparam Model Concrete model type. Must have a registered `ModelTraits` specialisation.
    /// @return Shared pointer to the new binding.
    template <typename Model>
    std::shared_ptr<detail::HandlerBinding> registerHandler() {
        auto binding = makeBinding<Model>(false);
        registerHandler(binding);
        return binding;
    }

    /// @brief Tracks @p binding and issues its bind on the active backend.
    ///
    /// Never waits. A backend that settles the bind before returning has bound
    /// @p binding by the time this returns; otherwise a call made through it
    /// is held until the bind settles. A failed bind is logged and rejects the
    /// calls held for it with its error.
    /// @param binding Pre-constructed binding. Its `typeId`, `modelFactory`
    ///        and `contextKey` must be set.
    void registerHandler(const std::shared_ptr<detail::HandlerBinding>& binding) {
        note("Bridge::registerHandler");
        _handlers.push_back(binding);
        auto superseded = startBind(binding,
                                    ::morph::backend::detail::BindRequest{.typeId = binding->typeId,
                                                                          .factory = binding->modelFactory,
                                                                          .contextKey = binding->contextKey,
                                                                          .primary = {},
                                                                          .current = {}},
                                    std::nullopt, {});
        runSuperseded(superseded);
    }

    /// @brief Creates a shared, initially **unattached** binding for `Model`.
    ///
    /// Registers nothing on the backend: a shared handler has no instance
    /// until a keyed action or an explicit `attach` names one. The binding is
    /// tracked from the start so `switchBackend()` knows about it.
    ///
    /// @tparam Model Concrete model type. Must have a registered `ModelTraits`.
    /// @return Shared pointer to the new, unattached binding.
    template <typename Model>
    std::shared_ptr<detail::HandlerBinding> registerSharedHandler() {
        note("Bridge::registerSharedHandler");
        auto binding = makeBinding<Model>(true);
        _handlers.push_back(binding);
        return binding;
    }

    /// @brief Attaches (or re-points) @p binding to the shared instance for @p primary.
    ///
    /// Idempotent: attaching to the primary a binding already holds is a no-op.
    /// A different primary re-points the binding — the previous instance is
    /// released and survives only if another handler still holds it. The
    /// binding's `contextKey` is set to @p primary as well, so a keyed model's
    /// journal entries carry the entity key.
    ///
    /// Issued after any bind already in flight for @p binding, and never
    /// waits: a call made through the binding meanwhile is held until it
    /// settles. A refused attach is logged and leaves the binding on the
    /// instance it held; a call held behind it is rejected with its error only
    /// when the binding has no instance to fall back to.
    ///
    /// @tparam Model Concrete model type.
    /// @param binding Shared binding, as returned by `registerSharedHandler<Model>()`.
    /// @param primary Canonical string encoding of the primary key to attach to.
    template <typename Model>
    void attachHandler(const std::shared_ptr<detail::HandlerBinding>& binding, std::string primary) {
        note("Bridge::attachHandler");
        std::weak_ptr<detail::HandlerBinding> const weak{binding};
        whenIdle(*binding, [this, weak, primary = std::move(primary)](const std::exception_ptr& failure) {
            auto strong = weak.lock();
            if (failure || !strong || (strong->primary == primary && strong->currentId.load() != 0U)) {
                return;
            }
            runSuperseded(startBind(strong, attachRequest(*strong, primary), primary, {}));
        });
    }

    /// @brief Files @p binding's current instance under @p primary, in place.
    ///
    /// The instance keeps everything the creating action just did — nothing is
    /// re-created and nothing is stranded. A no-op if @p binding is unbound or
    /// already holds a real primary (the backend refuses to re-key a keyed
    /// instance — see `IBackend::assignPrimary`).
    ///
    /// Reaches the backend through `IBackend::promoteModel`, delivered on the
    /// owner. A backend that settles before returning has promoted the binding
    /// by the time this returns, which a result-keyed `execute` relies on: its
    /// `.then` then already sees the new primary. A failure is logged.
    ///
    /// Known gap: if the target key is already held by a different instance,
    /// the backend declines to promote (the existing holder always wins) but
    /// reports success, so the binding caches a primary the backend never
    /// filed it under. Closing it needs `assignPrimary`'s outcome to become
    /// observable across every backend.
    /// @tparam Model Concrete model type.
    /// @param binding Shared binding whose instance is being promoted.
    /// @param primary Canonical string encoding of the key to file it under.
    template <typename Model>
    void assignHandlerPrimary(const std::shared_ptr<detail::HandlerBinding>& binding, std::string primary) {
        note("Bridge::assignHandlerPrimary");
        auto const raw = binding->currentId.load();
        if (raw == 0U || primary.empty() || !binding->primary.empty()) {
            return;
        }
        auto backend = _backend;
        auto completion = backend->promoteModel(
            ::morph::backend::detail::PromoteRequest{
                .mid = ::morph::exec::detail::ModelId{raw}, .typeId = binding->typeId, .primary = primary},
            _affinity.owner());
        std::weak_ptr<detail::HandlerBinding> const weak{binding};
        std::weak_ptr<::morph::backend::detail::IBackend> const weakBackend{backend};
        if (auto settled = detail::takeSettled(completion)) {
            applyPromote(weak, weakBackend, settled->failure, primary);
            return;
        }
        auto const token = _callbacks.token();
        completion
            .then([this, token, weak, weakBackend, primary](::morph::exec::detail::ModelId) {
                if (token.active()) {
                    applyPromote(weak, weakBackend, nullptr, primary);
                }
            })
            .onError([this, token, weak, weakBackend, primary](const std::exception_ptr& failure) {
                if (token.active()) {
                    applyPromote(weak, weakBackend, failure, primary);
                }
            });
    }

    /// @brief Returns @p binding's current primary key, or empty if unattached.
    /// @param binding Binding to inspect.
    /// @return Canonical key string, or an empty string when unattached.
    [[nodiscard]] std::string bindingPrimary(const std::shared_ptr<detail::HandlerBinding>& binding) const {
        note("Bridge::bindingPrimary");
        return binding->primary;
    }

    /// @brief Whether @p binding currently has a live `ModelId`.
    ///
    /// A point-in-time atomic read, safe from any thread. Nothing needs to wait
    /// on it: a call made through an unbound handler whose bind is in flight
    /// is held until the bind settles.
    /// @param binding Binding to inspect.
    /// @return `true` if `currentId != 0`.
    [[nodiscard]] static bool isBound(const std::shared_ptr<detail::HandlerBinding>& binding) noexcept {
        return binding->currentId.load() != 0U;
    }

    /// @brief Lists the live shared primary keys of `Model` on the active backend.
    /// @tparam Model Concrete model type.
    /// @param cbExec Executor the answer is delivered on. Borrowed: it must
    ///        outlive the returned `Completion`.
    /// @return A `Completion` resolved with the canonical key strings, in
    ///         unspecified order, or rejected with the backend's failure.
    template <typename Model>
    [[nodiscard]] ::morph::async::Completion<std::vector<std::string>> instancesOf(
        ::morph::exec::IExecutor& cbExec MORPH_LIFETIMEBOUND) {
        note("Bridge::instancesOf");
        return _backend->instances(std::string{::morph::model::ModelTraits<Model>::typeId()}, cbExec);
    }

    /// @brief Registers a result-type subscription for @p binding.
    ///
    /// The subscription is stored against the *binding*, not against a fixed
    /// instance id, and is matched at publish time by comparing the binding's
    /// current instance, so re-pointing a handler moves its subscriptions with
    /// it. See `detail::SubscriptionRegistry`.
    /// @param binding Handler binding that owns the subscription.
    /// @param type    Result type being subscribed to.
    /// @param sink    Type-erased delivery callback; receives the boxed result.
    /// @param exec    Executor the callback is delivered on.
    void addSubscription(const std::shared_ptr<detail::HandlerBinding>& binding, std::type_index type,
                         std::function<void(const std::any&)> sink, ::morph::exec::IExecutor* exec) {
        note("Bridge::addSubscription");
        _subscriptions->addSubscription(binding, type, std::move(sink), exec);
    }

    /// @brief Removes @p binding's subscription for @p type, if any.
    /// @param binding Handler binding that owns the subscription.
    /// @param type    Result type to stop hearing about.
    void removeSubscription(const std::shared_ptr<detail::HandlerBinding>& binding, std::type_index type) {
        note("Bridge::removeSubscription");
        _subscriptions->removeSubscription(binding, type);
    }

    /// @brief Whether any subscription is currently registered on this bridge.
    ///
    /// A single relaxed atomic load, safe from any thread: a result with no
    /// subscriber is never boxed and never sent to the owner.
    /// @return `true` if at least one subscription exists.
    [[nodiscard]] bool hasSubscribers() const noexcept { return _subscriptions->hasSubscribers(); }

    /// @brief Delivers @p value to every subscriber attached to instance @p mid.
    ///
    /// Called, on the owner, for every successful action result that has
    /// subscribers. Subscribers are matched on *the instance the result was
    /// produced on*, so a handler hears about work another handler — or, with a
    /// shared instance, another screen entirely — did on the model it is
    /// attached to. The producing handler is notified too.
    ///
    /// @param mid   Instance the result was produced on.
    /// @param type  Result type produced.
    /// @param value Boxed result.
    void publishResult(::morph::exec::detail::ModelId mid, std::type_index type, const std::any& value) {
        note("Bridge::publishResult");
        _subscriptions->publishResult(mid, type, value);
    }

    /// @brief Installs a default session context that `executeVia` stamps onto the
    ///        `ActionCall` of every subsequent call.
    ///
    /// There is no per-call session override — this default is applied to all calls
    /// until replaced or cleared. Also pushed to the active backend, so every
    /// control envelope it builds afterwards carries it too.
    ///
    /// @param session The new default. Pass `{}` to clear.
    void setDefaultSession(::morph::session::Context session) {
        note("Bridge::setDefaultSession");
        _defaultSession = session;
        if (_backend) {
            _backend->setSession(std::move(session));
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
    /// The clock starts inside `executeVia()`, when the call is made — before a
    /// wait for its bind, if it has to wait — so @p deadline covers the whole
    /// round trip.
    ///
    /// Disabled (`std::chrono::milliseconds{0}`, the default): a dropped
    /// frame or a hung server leaves the `Completion` pending forever.
    ///
    /// The backing `TimeoutScheduler` (and the `exec::IoLoop` it owns, with
    /// that loop's one thread) is created lazily on the first call that enables
    /// a deadline, so a `Bridge` that never opts in spawns no extra thread. Once
    /// created it lives until `~Bridge()`.
    ///
    /// @param deadline Maximum time to wait for any reply. `0` disables the
    ///                 deadline.
    void setExecuteDeadline(std::chrono::milliseconds deadline) {
        note("Bridge::setExecuteDeadline");
        _executeDeadline = deadline;
        if (_executeDeadline.count() > 0 && !_timeoutScheduler) {
            _timeoutScheduler = std::make_shared<::morph::async::detail::TimeoutScheduler>();
        }
    }

    /// @brief Returns the currently installed client-side execute deadline.
    /// @return The deadline; `std::chrono::milliseconds{0}` when disabled.
    [[nodiscard]] std::chrono::milliseconds executeDeadline() const {
        note("Bridge::executeDeadline");
        return _executeDeadline;
    }

    /// @brief Returns a copy of the currently installed default session.
    /// @return Snapshot of the default `Context`.
    [[nodiscard]] ::morph::session::Context defaultSession() const {
        note("Bridge::defaultSession");
        return _defaultSession;
    }

    /// @brief Installs the verified `Principal` for this `Bridge`.
    ///
    /// Typically called once right after a successful login dispatch, from
    /// data the server actually returned (see `session::Principal`'s doc
    /// comment on the trust model). Distinct from `setDefaultSession`: that
    /// installs the per-call `Context` forwarded with every dispatch; this
    /// installs the longer-lived identity UI code reads *outside* a dispatch
    /// via `currentPrincipal()` to shape itself.
    /// @param principal Verified identity to install. Pass a
    ///        default-constructed `Principal{}` (or call this from a sign-out
    ///        handler) to clear it.
    void setPrincipal(::morph::session::Principal principal) {
        note("Bridge::setPrincipal");
        _principal = std::move(principal);
    }

    /// @brief Returns a copy of the currently installed `Principal`.
    ///
    /// Default-constructed (empty `id`, no `roles`) if `setPrincipal` was
    /// never called or the application signed out by clearing it.
    /// @return Snapshot of the installed `Principal`.
    [[nodiscard]] ::morph::session::Principal currentPrincipal() const {
        note("Bridge::currentPrincipal");
        return _principal;
    }

    /// @brief Returns the number of actions made through `executeVia()` that
    ///        have not yet resolved.
    ///
    /// Incremented once per call, when it is made (a call held for its bind
    /// included); decremented exactly once when the returned `Completion`
    /// settles on success or on error (including a cancellation error from
    /// `switchBackend()`/`~Bridge()`/a dropped transport). A client can poll
    /// this to build a "still loading" indicator or gate a feature on
    /// quiescence. A single relaxed atomic load, safe from any thread.
    ///
    /// @return Count of actions made but not yet resolved.
    [[nodiscard]] std::size_t pendingCalls() const noexcept { return _pendingCalls->load(std::memory_order_relaxed); }

    /// @brief Replaces the active backend with @p newBackend.
    ///
    /// @note Templated on the concrete @p Backend (rather than taking
    ///       `unique_ptr<IBackend>` directly) so it is an exact match for a
    ///       `unique_ptr<Concrete>` argument (e.g. `std::make_unique<LocalBackend>(...)`)
    ///       and is preferred over the `shared_ptr<IBackend>` overload during overload
    ///       resolution — both would otherwise require an equally-ranked user-defined
    ///       conversion, making every existing call site ambiguous.
    /// @tparam Backend Concrete backend type; must derive from `IBackend`.
    /// @param newBackend Replacement backend. Ownership is transferred.
    template <typename Backend>
        requires std::derived_from<Backend, ::morph::backend::detail::IBackend>
    void switchBackend(std::unique_ptr<Backend> newBackend) {
        switchBackend(std::shared_ptr<::morph::backend::detail::IBackend>{std::move(newBackend)});
    }

    /// @brief Replaces the active backend with @p newBackend.
    ///
    /// Tells @p newBackend its owner and the current default session, then
    /// issues a bind on it for every live binding (an unattached shared one
    /// has nothing to re-create). Binds the new backend settles before
    /// returning decide the switch: if any of them failed, every instance
    /// acquired so far is released, and the failure is rethrown with the old
    /// backend and every binding untouched. Otherwise the switch commits: the
    /// settled ids are published, a bind still in flight leaves its binding
    /// unbound with its calls held until it settles, the backend is swapped,
    /// `notifyBackendChanged()` runs, the reconnect handler moves to the new
    /// backend, and the old backend's pending calls are cancelled with
    /// `BackendChangedError` (a Task handler still running there is asked to
    /// stop). A failure of a bind still in flight at the commit cannot roll the
    /// switch back; it is recorded on its binding.
    ///
    /// The caller keeps shared ownership of @p newBackend, so the same backend
    /// can be re-installed later without reconstructing it.
    ///
    /// @param newBackend Replacement backend, shared with the caller.
    void switchBackend(std::shared_ptr<::morph::backend::detail::IBackend> newBackend) {
        note("Bridge::switchBackend");
        auto next = std::move(newBackend);
        next->setOwner(_affinity);
        next->setSession(_defaultSession);

        struct Issued {
            std::shared_ptr<detail::HandlerBinding> binding;
            ::morph::async::Completion<::morph::exec::detail::ModelId> completion;
            std::optional<detail::BindOutcome> settled;
        };
        std::vector<Issued> issued;
        std::vector<std::weak_ptr<detail::HandlerBinding>> live;
        std::vector<std::shared_ptr<detail::HandlerBinding>> untouched;
        std::exception_ptr staging;
        try {
            for (auto const& weak : _handlers) {
                auto binding = weak.lock();
                if (!binding) {
                    continue;
                }
                live.push_back(weak);
                if (binding->shared && binding->primary.empty()) {
                    untouched.push_back(binding);
                    continue;
                }
                auto completion = next->bindModel(rebindRequest(*binding), _affinity.owner());
                auto settled = detail::takeSettled(completion);
                bool const failed = settled && settled->failure;
                if (failed) {
                    staging = settled->failure;
                }
                issued.push_back(Issued{.binding = binding, .completion = std::move(completion), .settled = settled});
                if (failed) {
                    break;
                }
            }
        } catch (...) {
            staging = std::current_exception();
        }
        if (staging) {
            rollback(next, issued);
            std::rethrow_exception(staging);
        }

        std::vector<std::function<void(std::exception_ptr)>> superseded;
        for (auto& entry : issued) {
            auto& binding = *entry.binding;
            auto const generation = ++binding.bindGeneration;
            if (auto own = std::exchange(binding.afterBind, {})) {
                superseded.push_back(std::move(own));
            }
            binding.bindFailure = nullptr;
            if (entry.settled) {
                binding.currentId.store(entry.settled->id.v);
                binding.bindInFlight = false;
                continue;
            }
            binding.currentId.store(0);
            binding.bindInFlight = true;
            awaitBind(entry.binding, next, generation, std::move(entry.completion), std::nullopt);
        }
        for (auto const& binding : untouched) {
            if (binding->bindInFlight) {
                // An attach in flight on the backend being replaced: its reply
                // is superseded, and a held keyed call re-issues it on the new
                // backend when it resumes.
                ++binding->bindGeneration;
                binding->bindInFlight = false;
                if (auto own = std::exchange(binding->afterBind, {})) {
                    superseded.push_back(std::move(own));
                }
            }
        }
        _handlers = std::move(live);
        auto previous = std::exchange(_backend, next);
        next->notifyBackendChanged();
        installReconnectHandler(next);
        if (previous && previous != next) {
            previous->setReconnectHandler(nullptr, nullptr);
            previous->cancelPending(std::make_exception_ptr(::morph::backend::BackendChangedError{}));
        }
        auto const changed = std::make_exception_ptr(::morph::backend::BackendChangedError{});
        for (auto& resume : superseded) {
            resume(changed);
        }
        for (auto const& weak : _handlers) {
            if (auto binding = weak.lock()) {
                drainWaiting(*binding);
            }
        }
    }

    /// @brief Deregisters @p binding from the active backend and removes it from tracking.
    ///
    /// Every call held for a bind in flight is rejected with
    /// `HandlerDestroyedError`, and that bind's reply, when it lands, releases
    /// its instance rather than binding it. After this, a call made through
    /// @p binding fails with "handler not bound".
    /// @param binding Binding to remove.
    void deregisterHandler(const std::shared_ptr<detail::HandlerBinding>& binding) {
        note("Bridge::deregisterHandler");
        ++binding->bindGeneration;
        binding->bindInFlight = false;
        std::vector<std::function<void(std::exception_ptr)>> rejected;
        takeHeld(*binding, rejected);
        auto const raw = binding->currentId.exchange(0);
        if (raw != 0U && _backend) {
            _backend->deregisterModel(::morph::exec::detail::ModelId{raw});
        }
        std::erase_if(_handlers, [&binding](const auto& weak) {
            auto strong = weak.lock();
            return !strong || strong.get() == binding.get();
        });
        auto const destroyed = std::make_exception_ptr(::morph::backend::HandlerDestroyedError{});
        for (auto& resume : rejected) {
            resume(destroyed);
        }
    }

    /// @brief Dispatches @p action against the model identified by @p binding.
    ///
    /// Counts the call as pending and arms the client-side deadline at once.
    /// When @p binding is bound and no bind is in flight, the action is
    /// dispatched here, through `IBackend::executeInto`, handing the backend a
    /// `detail::BridgeSink<R>` that is both the caller's typed completion state
    /// and the backend's settle sink. Otherwise the call is held by the binding
    /// and dispatched on the owner when the bind settles; a failed bind rejects
    /// it with the bind's error, a destroyed handler with
    /// `HandlerDestroyedError`. An `AllowShared` binding that was never
    /// attached rejects it with "handler not bound".
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
    /// A call whose handler returns a `Task` carries a stop source, requested
    /// by the execute deadline, by a `CallbackScope` a callback was attached
    /// through (see `Completion::then`), and by the backend's `cancelPending`.
    ///
    /// @tparam Model  Model type that owns the handler.
    /// @tparam Action Action type to dispatch.
    /// @param binding Binding returned by `registerHandler<Model>()`.
    /// @param action  Action to execute (moved in).
    /// @param cbExec  Executor on which the `Completion` callbacks are posted.
    /// @param onResult Optional observer run on the owner on the typed result
    ///                 *before* it is moved into the returned `Completion` and
    ///                 before the caller's own `.then`. Used to adopt a
    ///                 result-sourced primary key so the binding is already
    ///                 promoted by the time user code sees the result; empty for
    ///                 every other call.
    /// @return Completion that resolves with the typed result or an exception
    ///         (including `ValidationError` on `LocalBackend` when the action
    ///         fails its validator).
    template <typename Model, typename Action>
    ::morph::async::Completion<typename ::morph::model::ActionTraits<Action>::Result> executeVia(
        const std::shared_ptr<detail::HandlerBinding>& binding, Action action, ::morph::exec::IExecutor* cbExec,
        std::function<void(const typename ::morph::model::ActionTraits<Action>::Result&)> onResult = {}) {
        using R = ::morph::model::ActionTraits<Action>::Result;
        note("Bridge::executeVia");
        auto sink = makeSink<Model, Action>(std::move(onResult));
        ::morph::async::Completion<R> typed{sink, cbExec};
        if (!binding->bindInFlight) {
            dispatchNow<Model, Action>(*binding, sink, std::move(action), cbExec, /*held=*/false);
            return typed;
        }
        std::weak_ptr<detail::HandlerBinding> const weak{binding};
        binding->waiting.push_back([this, weak, sink, cbExec, held = std::make_shared<Action>(std::move(action))](
                                       const std::exception_ptr& failure) {
            auto strong = weak.lock();
            if (failure || !strong) {
                sink->settleException(failure ? failure
                                              : std::make_exception_ptr(::morph::backend::HandlerDestroyedError{}));
                return;
            }
            dispatchNow<Model, Action>(*strong, sink, std::move(*held), cbExec, /*held=*/true);
        });
        return typed;
    }

    /// @brief Dispatches a payload-keyed @p action on a shared @p binding,
    ///        attaching it to the instance @p key names first.
    ///
    /// Waits for any bind in flight, then attaches (or re-points) the binding
    /// to @p key if it does not hold it already, and dispatches once the attach
    /// has settled. A refused attach rejects the call with its error.
    /// @tparam Model  Model type that owns the handler.
    /// @tparam Action Action type to dispatch.
    /// @param binding Shared binding.
    /// @param action  Action to execute (moved in).
    /// @param cbExec  Executor on which the `Completion` callbacks are posted.
    /// @param key     Canonical primary key the action names.
    /// @return Completion that resolves with the typed result or an exception.
    template <typename Model, typename Action>
    ::morph::async::Completion<typename ::morph::model::ActionTraits<Action>::Result> executeAttachedVia(
        const std::shared_ptr<detail::HandlerBinding>& binding, Action action, ::morph::exec::IExecutor* cbExec,
        std::string key) {
        using R = ::morph::model::ActionTraits<Action>::Result;
        note("Bridge::executeVia");
        auto sink = makeSink<Model, Action>({});
        ::morph::async::Completion<R> typed{sink, cbExec};
        auto held = std::make_shared<Action>(std::move(action));
        std::weak_ptr<detail::HandlerBinding> const weak{binding};
        whenIdle(*binding, [this, weak, sink, cbExec, held, key = std::move(key)](const std::exception_ptr& failure) {
            auto strong = weak.lock();
            if (rejectIfGone(sink, strong, failure)) {
                return;
            }
            if (strong->primary == key && strong->currentId.load() != 0U) {
                dispatchNow<Model, Action>(*strong, sink, std::move(*held), cbExec, /*held=*/true);
                return;
            }
            runSuperseded(startBind(strong, attachRequest(*strong, key), key,
                                    [this, weak, sink, cbExec, held, key](const std::exception_ptr& settled) {
                                        auto bound = weak.lock();
                                        if (rejectIfGone(sink, bound, settled)) {
                                            return;
                                        }
                                        if (bound->primary != key || bound->currentId.load() == 0U) {
                                            sink->settleException(bound->bindFailure ? bound->bindFailure
                                                                                     : notBound());
                                            return;
                                        }
                                        dispatchNow<Model, Action>(*bound, sink, std::move(*held), cbExec, true);
                                    }));
        });
        return typed;
    }

    /// @brief Dispatches a result-keyed @p action on a shared @p binding,
    ///        giving it an anonymous instance first if it has none, and
    ///        promoting that instance under the key its result carries.
    /// @tparam Model  Model type that owns the handler.
    /// @tparam Action Action type to dispatch.
    /// @param binding Shared binding.
    /// @param action  Action to execute (moved in).
    /// @param cbExec  Executor on which the `Completion` callbacks are posted.
    /// @return Completion that resolves with the typed result or an exception.
    template <typename Model, typename Action>
    ::morph::async::Completion<typename ::morph::model::ActionTraits<Action>::Result> executeCreatingVia(
        const std::shared_ptr<detail::HandlerBinding>& binding, Action action, ::morph::exec::IExecutor* cbExec) {
        using R = ::morph::model::ActionTraits<Action>::Result;
        note("Bridge::executeVia");
        std::weak_ptr<detail::HandlerBinding> const weak{binding};
        auto sink = makeSink<Model, Action>([this, weak](const R& result) {
            if (auto strong = weak.lock()) {
                assignHandlerPrimary<Model>(strong,
                                            ::morph::model::ActionKeyTraits<Action>::template keyOfResult<R>(result));
            }
        });
        ::morph::async::Completion<R> typed{sink, cbExec};
        auto held = std::make_shared<Action>(std::move(action));
        whenIdle(*binding, [this, weak, sink, cbExec, held](const std::exception_ptr& failure) {
            auto strong = weak.lock();
            if (rejectIfGone(sink, strong, failure)) {
                return;
            }
            if (strong->currentId.load() != 0U) {
                dispatchNow<Model, Action>(*strong, sink, std::move(*held), cbExec, /*held=*/true);
                return;
            }
            runSuperseded(startBind(strong,
                                    ::morph::backend::detail::BindRequest{.typeId = strong->typeId,
                                                                          .factory = strong->modelFactory,
                                                                          .contextKey = strong->contextKey,
                                                                          .primary = {},
                                                                          .current = {}},
                                    std::nullopt, [this, weak, sink, cbExec, held](const std::exception_ptr& settled) {
                                        auto bound = weak.lock();
                                        if (rejectIfGone(sink, bound, settled)) {
                                            return;
                                        }
                                        dispatchNow<Model, Action>(*bound, sink, std::move(*held), cbExec, true);
                                    }));
        });
        return typed;
    }

private:
    template <typename, typename>
    friend class BridgeHandler;

    using Resume = std::function<void(std::exception_ptr)>;

    /// @brief Checks, in a debug build, that the caller is on the owner.
    /// @param site Name of the calling body.
    void note(char const* site) const noexcept { _affinity.note(site); }

    /// @brief Weak observer of this bridge's lifetime. Every continuation that
    ///        touches the bridge checks it first; on the owner the check is
    ///        exact, because `~Bridge` runs there too.
    /// @return A token that expires when the bridge is destroyed.
    [[nodiscard]] ::morph::async::CallbackToken liveness() const { return _callbacks.token(); }

    /// @brief The affinity every handler of this bridge checks itself against.
    /// @return A copy of the bridge's owner affinity.
    [[nodiscard]] ::morph::exec::detail::OwnerAffinity affinity() const noexcept { return _affinity; }

    /// @brief Builds a binding for `Model`, not yet tracked or bound.
    /// @tparam Model Concrete model type.
    /// @param shared Whether the binding joins the shared directory.
    /// @return The new binding.
    template <typename Model>
    static std::shared_ptr<detail::HandlerBinding> makeBinding(bool shared) {
        auto binding = std::make_shared<detail::HandlerBinding>();
        binding->typeId = std::string{::morph::model::ModelTraits<Model>::typeId()};
        binding->modelFactory = [] { return ::morph::model::detail::ModelFactory::create<Model>(); };
        binding->shared = shared;
        return binding;
    }

    /// @brief Takes a handler's binding into this bridge: at once on the owner;
    ///        off it — a handler constructed inside a running action — by
    ///        posting the registration to the owner.
    ///
    /// Posted, the binding is marked as binding until the task runs, so a call
    /// made through it on the owner meanwhile is held rather than refused.
    /// @param binding The handler's binding; nothing else has seen it yet.
    void adoptHandler(const std::shared_ptr<detail::HandlerBinding>& binding) {
        if (onOwner()) {
            adoptOnOwner(binding);
            return;
        }
        binding->bindInFlight = true;
        _affinity.owner().post([this, token = _callbacks.token(), binding] {
            if (!token.active()) {
                return;
            }
            binding->bindInFlight = false;
            adoptOnOwner(binding);
        });
    }

    /// @brief `adoptHandler`'s body, on the owner.
    /// @param binding The handler's binding.
    void adoptOnOwner(const std::shared_ptr<detail::HandlerBinding>& binding) {
        if (binding->shared) {
            note("Bridge::registerSharedHandler");
            _handlers.push_back(binding);
            drainWaiting(*binding);
            return;
        }
        registerHandler(binding);
    }

    /// @brief Runs @p resume now when no bind is in flight for @p binding,
    ///        or holds it until one settles.
    /// @param binding The binding.
    /// @param resume  Resumed with null to proceed, or an error to give up.
    static void whenIdle(detail::HandlerBinding& binding, Resume resume) {
        if (binding.bindInFlight) {
            binding.waiting.push_back(std::move(resume));
            return;
        }
        resume(nullptr);
    }

    /// @brief Resumes the calls held by @p binding while no bind is in flight.
    ///        A resumed call may issue a bind of its own, which holds the rest.
    /// @param binding The binding.
    static void drainWaiting(detail::HandlerBinding& binding) {
        while (!binding.bindInFlight && !binding.waiting.empty()) {
            auto next = std::move(binding.waiting.front());
            binding.waiting.pop_front();
            next(nullptr);
        }
    }

    /// @brief Moves everything @p binding holds — the in-flight operation's
    ///        continuation, then the held calls — into @p out, to be rejected
    ///        once the bridge's own state is consistent.
    /// @param binding The binding.
    /// @param out     Where the continuations go.
    static void takeHeld(detail::HandlerBinding& binding, std::vector<Resume>& out) {
        if (auto own = std::exchange(binding.afterBind, {})) {
            out.push_back(std::move(own));
        }
        for (auto& resume : std::exchange(binding.waiting, {})) {
            out.push_back(std::move(resume));
        }
    }

    /// @brief Rejects an operation's continuation that a newer bind replaced.
    /// @param superseded The continuation, or empty.
    static void runSuperseded(Resume superseded) {
        if (superseded) {
            superseded(std::make_exception_ptr(std::runtime_error{"bind superseded by a newer one"}));
        }
    }

    /// @brief The error a call gets through a binding with no instance and no
    ///        bind to wait for: an `AllowShared` handler never attached.
    /// @return The error.
    [[nodiscard]] static std::exception_ptr notBound() {
        return std::make_exception_ptr(std::runtime_error("handler not bound"));
    }

    /// @brief Rejects @p sink when a held operation cannot go on.
    /// @tparam Sink    The sink's type.
    /// @param sink     The call's sink.
    /// @param binding  The binding, or null once its handler is gone.
    /// @param failure  The error the operation was resumed with, or null.
    /// @return Whether @p sink was rejected.
    template <typename Sink>
    static bool rejectIfGone(const Sink& sink, const std::shared_ptr<detail::HandlerBinding>& binding,
                             const std::exception_ptr& failure) {
        if (failure) {
            sink->settleException(failure);
            return true;
        }
        if (!binding) {
            sink->settleException(std::make_exception_ptr(::morph::backend::HandlerDestroyedError{}));
            return true;
        }
        return false;
    }

    /// @brief The bind request that re-creates @p binding's instance on a
    ///        backend: private, or the shared one for its primary.
    /// @param binding The binding.
    /// @return The request.
    [[nodiscard]] static ::morph::backend::detail::BindRequest rebindRequest(const detail::HandlerBinding& binding) {
        return ::morph::backend::detail::BindRequest{.typeId = binding.typeId,
                                                     .factory = binding.modelFactory,
                                                     .contextKey = binding.contextKey,
                                                     .primary = binding.shared ? binding.primary : std::string{},
                                                     .current = {}};
    }

    /// @brief The bind request that attaches (or re-points) @p binding to @p key.
    /// @param binding The binding.
    /// @param key     Canonical primary key.
    /// @return The request.
    [[nodiscard]] static ::morph::backend::detail::BindRequest attachRequest(const detail::HandlerBinding& binding,
                                                                             const std::string& key) {
        return ::morph::backend::detail::BindRequest{
            .typeId = binding.typeId,
            .factory = binding.modelFactory,
            .contextKey = key,
            .primary = key,
            .current = ::morph::exec::detail::ModelId{binding.currentId.load()}};
    }

    /// @brief Issues @p request for @p binding on the active backend, with the
    ///        owner as the executor its reply is delivered on.
    ///
    /// A backend that settles before returning has its outcome applied here;
    /// otherwise the continuation applies it on the owner, gated on the
    /// bridge's token. @p afterBind runs when the bind settles, before the calls
    /// held for it.
    /// @param binding   The binding the bind is for.
    /// @param request   Owning bind request.
    /// @param identity  The key to publish as the binding's primary and
    ///                  context key on success, or none.
    /// @param afterBind The issuing operation's continuation, or empty.
    /// @return A continuation this bind superseded, for the caller to reject
    ///         once its own state is consistent; usually empty.
    [[nodiscard]] Resume startBind(const std::shared_ptr<detail::HandlerBinding>& binding,
                                   ::morph::backend::detail::BindRequest request, std::optional<std::string> identity,
                                   Resume afterBind) {
        auto superseded = std::exchange(binding->afterBind, std::move(afterBind));
        auto const generation = ++binding->bindGeneration;
        binding->bindInFlight = true;
        binding->bindFailure = nullptr;
        auto backend = _backend;
        ::morph::async::Completion<::morph::exec::detail::ModelId> completion;
        try {
            completion = backend->bindModel(std::move(request), _affinity.owner());
        } catch (...) {
            applyBind(binding, backend, generation, {}, std::current_exception(), identity);
            return superseded;
        }
        if (auto settled = detail::takeSettled(completion)) {
            applyBind(binding, backend, generation, settled->id, settled->failure, identity);
            return superseded;
        }
        awaitBind(binding, backend, generation, std::move(completion), std::move(identity));
        return superseded;
    }

    /// @brief Applies a bind's outcome when its reply arrives, on the owner.
    /// @param binding    The binding the bind is for.
    /// @param backend    The backend that will settle it.
    /// @param generation The binding's bind count when it was issued.
    /// @param completion The bind's result.
    /// @param identity   What to publish as the binding's primary on success.
    void awaitBind(const std::shared_ptr<detail::HandlerBinding>& binding,
                   const std::shared_ptr<::morph::backend::detail::IBackend>& backend, std::uint64_t generation,
                   ::morph::async::Completion<::morph::exec::detail::ModelId> completion,
                   std::optional<std::string> identity) {
        std::weak_ptr<detail::HandlerBinding> const weak{binding};
        std::weak_ptr<::morph::backend::detail::IBackend> const weakBackend{backend};
        auto const token = _callbacks.token();
        completion
            .then([this, token, weak, weakBackend, generation, identity](::morph::exec::detail::ModelId mid) {
                if (!token.active()) {
                    // The bridge is gone: nothing will use this instance.
                    if (auto owner = weakBackend.lock()) {
                        owner->deregisterModel(mid);
                    }
                    return;
                }
                applyBind(weak.lock(), weakBackend.lock(), generation, mid, nullptr, identity);
            })
            .onError([this, token, weak, weakBackend, generation, identity](const std::exception_ptr& failure) {
                if (token.active()) {
                    applyBind(weak.lock(), weakBackend.lock(), generation, {}, failure, identity);
                }
            });
    }

    /// @brief Applies one bind outcome to @p binding, on the owner.
    ///
    /// A bind whose binding is gone, or whose count a newer bind has passed,
    /// releases a successful id on the backend that issued it instead. The
    /// current one publishes the id (and @p identity) or records the failure,
    /// then runs the issuing operation's continuation and the held calls.
    /// @param binding    The binding, or null once its handler is gone.
    /// @param backend    The backend that settled it, or null once it is gone.
    /// @param generation The binding's bind count when it was issued.
    /// @param mid        The bound instance, on success.
    /// @param failure    The failure, or null.
    /// @param identity   What to publish as the binding's primary on success.
    void applyBind(const std::shared_ptr<detail::HandlerBinding>& binding,
                   const std::shared_ptr<::morph::backend::detail::IBackend>& backend, std::uint64_t generation,
                   ::morph::exec::detail::ModelId mid, const std::exception_ptr& failure,
                   const std::optional<std::string>& identity) {
        note("Bridge::applyBind");
        if (!binding || binding->bindGeneration != generation) {
            if (!failure && mid.v != 0U && backend) {
                backend->deregisterModel(mid);
            }
            return;
        }
        binding->bindInFlight = false;
        if (failure) {
            binding->bindFailure = failure;
            ::morph::log::logError("[bridge] bind of '" + binding->typeId +
                                   "' failed: " + detail::describeFailure(failure));
        } else {
            binding->currentId.store(mid.v);
            if (identity) {
                binding->contextKey = *identity;
                binding->primary = *identity;
            }
        }
        if (auto own = std::exchange(binding->afterBind, {})) {
            own(nullptr);
        }
        drainWaiting(*binding);
    }

    /// @brief Applies a promote's outcome, on the owner.
    /// @param weak        The binding.
    /// @param weakBackend The backend the promote went to.
    /// @param failure     The failure, or null.
    /// @param primary     The key the instance was filed under.
    void applyPromote(const std::weak_ptr<detail::HandlerBinding>& weak,
                      const std::weak_ptr<::morph::backend::detail::IBackend>& weakBackend,
                      const std::exception_ptr& failure, const std::string& primary) {
        note("Bridge::assignHandlerPrimary");
        auto binding = weak.lock();
        if (!binding) {
            return;
        }
        if (failure) {
            ::morph::log::logError("[assignHandlerPrimary] promotion of '" + binding->typeId +
                                   "' failed: " + detail::describeFailure(failure));
            return;
        }
        // A switch that moved past this promotion re-created the instance; a
        // binding already keyed by an attach that raced ahead keeps its key.
        if (weakBackend.lock() != _backend || !binding->primary.empty()) {
            return;
        }
        binding->contextKey = primary;
        binding->primary = primary;
    }

    /// @brief Releases every instance a failed switch acquired on @p backend,
    ///        now or when its bind settles.
    /// @tparam Issued The switch's record of one bind.
    /// @param backend The backend the switch was going to.
    /// @param issued  The binds issued.
    template <typename Issued>
    static void rollback(const std::shared_ptr<::morph::backend::detail::IBackend>& backend,
                         std::vector<Issued>& issued) {
        for (auto& entry : issued) {
            if (entry.settled) {
                if (!entry.settled->failure) {
                    try {
                        backend->deregisterModel(entry.settled->id);
                    } catch (const std::exception& exc) {
                        ::morph::log::logError(std::string{"[switchBackend] rollback deregister failed: "} +
                                               exc.what());
                    }
                }
                continue;
            }
            std::weak_ptr<::morph::backend::detail::IBackend> const weak{backend};
            entry.completion
                .then([weak](::morph::exec::detail::ModelId mid) {
                    if (auto owner = weak.lock()) {
                        owner->deregisterModel(mid);
                    }
                })
                .onError([](const std::exception_ptr&) {});
        }
    }

    /// @brief Builds the sink for one call: counts it pending, arms the
    ///        deadline, and gives a Task-handler call its stop source.
    /// @tparam Model  Model type.
    /// @tparam Action Action type.
    /// @param onResult Optional owner-side observer of the result.
    /// @return The sink.
    template <typename Model, typename Action>
    std::shared_ptr<detail::BridgeSink<typename ::morph::model::ActionTraits<Action>::Result>> makeSink(
        std::function<void(const typename ::morph::model::ActionTraits<Action>::Result&)> onResult) {
        using R = ::morph::model::ActionTraits<Action>::Result;
        auto sink = std::make_shared<detail::BridgeSink<R>>(std::move(onResult), _pendingCalls, _subscriptions,
                                                            _callbacks.token(), _affinity);
        _pendingCalls->fetch_add(1, std::memory_order_relaxed);
        std::shared_ptr<::core::async::StopSource> stopSource;
        if constexpr (isTaskHandlerCall<Model, Action>()) {
            stopSource = std::make_shared<::core::async::StopSource>();
            sink->stopSource = stopSource;
        }
        if (_executeDeadline.count() > 0 && _timeoutScheduler) {
            // The callback settles the *state*, not the sink: a deadline is not
            // one of the two resolution paths and must not decrement
            // `_pendingCalls`, which stays counted until the real reply lands
            // (or `dispatchNow` abandons a call the deadline settled while it
            // waited for its bind). It captures the sink alone, never `this`.
            auto const handle = _timeoutScheduler->schedule(_executeDeadline, [sink, stopSource] {
                sink->setException(std::make_exception_ptr(::morph::backend::ClientTimeoutError{}));
                if (stopSource) {
                    static_cast<void>(stopSource->request_stop());
                }
            });
            sink->armDeadline(handle, _timeoutScheduler);
        }
        return sink;
    }

    /// @brief Whether `Model`'s handler for `Action` returns a `Task`.
    /// @tparam Model  Model type.
    /// @tparam Action Action type.
    /// @return True for a coroutine handler.
    template <typename Model, typename Action>
    static consteval bool isTaskHandlerCall() {
#ifndef MORPH_CLIENT_ONLY
        return ::morph::model::isTaskHandler<decltype(std::declval<Model&>().execute(std::declval<Action&>()))>;
#else
        // A client-only build never runs a handler locally (see makeActionCall).
        return false;
#endif
    }

    /// @brief Dispatches @p action through @p binding's instance now, or
    ///        rejects the call when the binding has none.
    /// @tparam Model  Model type.
    /// @tparam Action Action type.
    /// @param binding The binding.
    /// @param sink    The call's sink.
    /// @param action  The action, moved in.
    /// @param cbExec  The caller's executor.
    /// @param held    Whether the call waited for a bind: its caller has
    ///                returned, so a throw from the backend rejects the call
    ///                instead of propagating.
    template <typename Model, typename Action>
    void dispatchNow(
        detail::HandlerBinding& binding,
        const std::shared_ptr<detail::BridgeSink<typename ::morph::model::ActionTraits<Action>::Result>>& sink,
        Action action, ::morph::exec::IExecutor* cbExec, bool held) {
        note("Bridge::dispatch");
        auto const raw = binding.currentId.load();
        if (raw == 0U) {
            sink->settleException(binding.bindFailure ? binding.bindFailure : notBound());
            return;
        }
        if (held && sink->alreadySettled()) {
            sink->abandon();
            return;
        }
        sink->target(::morph::exec::detail::ModelId{raw});
        auto call = makeActionCall<Model, Action>(std::move(action), sink->stopSource);
        call.session = _defaultSession;
        try {
            _backend->executeInto(::morph::exec::detail::ModelId{raw}, std::move(call), cbExec, sink);
        } catch (...) {
            if (held) {
                sink->settleException(std::current_exception());
                return;
            }
            // Undoes the pending count and the deadline together, through the
            // same settle-once latch the resolution paths use.
            sink->abandon();
            throw;
        }
    }

    /// @brief Builds the `ActionCall` for one dispatch of @p action.
    /// @tparam Model  Model type.
    /// @tparam Action Action type.
    /// @param action     The action, moved in.
    /// @param stopSource The call's stop source, for a Task handler; else null.
    /// @return The call, without its session.
    template <typename Model, typename Action>
    // One block: the lambdas below are the whole of the local path's contract
    // (recompute, validate, execute, journal), kept in the order they run.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    static ::morph::backend::detail::ActionCall makeActionCall(Action action,
                                                               std::shared_ptr<::core::async::StopSource> stopSource) {
        using R = ::morph::model::ActionTraits<Action>::Result;
        constexpr bool taskHandler = isTaskHandlerCall<Model, Action>();
        ::morph::backend::detail::ActionCall call;
        // Views of `constexpr` string literals, and stateless operations
        // addressed rather than copied: this whole block allocates exactly
        // once (the action itself). Copying instead would cost four
        // allocations per call -- two `std::string` copies of compile-time
        // constants and two `std::function`s whose `shared_ptr` capture
        // defeats libstdc++'s small-object buffer -- including on the
        // `LocalBackend` calls that never look at `serializeAction` or
        // `deserializeResult`. See `backend::detail::ActionCall` for the
        // lifetime contract this shape carries.
        call.modelTypeId = ::morph::model::ModelTraits<Model>::typeId();
        call.actionTypeId = ::morph::model::ActionTraits<Action>::typeId();
        auto sharedAction = std::make_shared<Action>(std::move(action));
        call.action = sharedAction;
        call.serializeAction = [](const void* actionPtr) {
            return ::morph::model::ActionTraits<Action>::toJson(*static_cast<const Action*>(actionPtr));
        };
        call.deserializeResult = [](std::string_view jsonStr) -> std::shared_ptr<void> {
            return std::make_shared<R>(::morph::model::ActionTraits<Action>::resultFromJson(jsonStr));
        };
        if constexpr (taskHandler) {
            call.localOpAsync = &detail::localTaskOp<Model, Action>;
            call.stopSource = std::move(stopSource);
        } else {
            call.localOp = [](::morph::model::detail::IModelHolder& holder, void* actionPtr) -> std::shared_ptr<void> {
                // The action `ActionCall::action` owns, handed back typed. The
                // backend that invokes this keeps that handle alive across the
                // call (LocalBackend carries it onto the strand with `localOp`).
                Action& actionRef = *static_cast<Action*>(actionPtr);
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
                ::morph::forms::recomputeAll(actionRef);
                if (!::morph::model::ActionValidator<Action>::ready(actionRef)) {
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
                // including why a rejected/throwing execute must not leave the audit
                // trail silent, and why `Model::execute` is the only call inside the
                // try that records Outcome::Failed.
// MSVC's C4702 fires on the `return` below for any action whose handler never
// returns -- a test double whose body is a bare `throw`, for instance. The
// warning is correct for that instantiation and wrong as a verdict on this
// statement, which every other instantiation reaches. It is suppressed here
// rather than in each translation unit that instantiates such a handler,
// because the set of those is open-ended: eleven test files already qualify.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4702)
#endif
                auto result = [&] {
                    try {
                        return std::make_shared<R>(model.execute(actionRef));
                    } catch (const std::exception& exc [[maybe_unused]]) {
                        if constexpr (::morph::model::detail::actionLoggable<Action>() ==
                                      ::morph::model::Loggable::Yes) {
                            if (holder.hasActionLog()) {
                                ::morph::model::detail::recordActionFailure(
                                    holder, std::string{::morph::model::ModelTraits<Model>::typeId()},
                                    std::string{::morph::model::ActionTraits<Action>::typeId()},
                                    ::morph::model::ActionTraits<Action>::toJson(actionRef),
                                    ::morph::model::detail::actionPayloadSchema<Action>(), exc.what());
                            }
                        }
                        throw;
                    }
                }();
#ifdef _MSC_VER
#pragma warning(pop)
#endif
                // Past this point the model's mutation has committed, so neither
                // serialising the result nor appending the entry may be reported as
                // an execution failure: both throw (ParseError; a sink that could
                // not reach its backend), and inside the try above that throw would
                // reject this call's Completion as if the model had refused the
                // action and file an Outcome::Failed entry blaming the action for an
                // infrastructure fault. ActionRecordingError says what is true
                // instead -- the action ran, the recording of it did not -- and
                // carries the result JSON the audit trail never received.
                if constexpr (::morph::model::detail::actionLoggable<Action>() == ::morph::model::Loggable::Yes) {
                    if (holder.hasActionLog()) {
                        std::string resultJson;
                        try {
                            resultJson = ::morph::model::ActionTraits<Action>::resultToJson(*result);
                            // entityKey/principal/timestampMs are filled in by recordIfAttached.
                            ::morph::model::detail::recordActionSuccess(
                                holder, std::string{::morph::model::ModelTraits<Model>::typeId()},
                                std::string{::morph::model::ActionTraits<Action>::typeId()},
                                ::morph::model::ActionTraits<Action>::toJson(actionRef),
                                ::morph::model::detail::actionPayloadSchema<Action>(), resultJson);
                        } catch (const std::exception& exc) {
                            throw ::morph::model::ActionRecordingError{std::move(resultJson), exc.what()};
                        }
                    }
                }
                return result;
#endif
            };
        }
        if constexpr (!taskHandler) {
            static_cast<void>(stopSource);
        }
        return call;
    }

    /// @brief Installs this bridge's reconnect handler on @p backend, posted to
    ///        the owner.
    /// @param backend The backend.
    void installReconnectHandler(const std::shared_ptr<::morph::backend::detail::IBackend>& backend) {
        std::weak_ptr<::morph::backend::detail::IBackend> const weak{backend};
        backend->setReconnectHandler(
            [this, weak, token = _callbacks.token()] {
                if (token.active()) {
                    reconnect(weak);
                }
            },
            &_affinity.owner());
    }

    /// @brief Re-issues a bind for every live binding after @p weak reconnected,
    ///        on the owner.
    ///
    /// The ids the bindings held belonged to the connection that dropped, so
    /// each is cleared; a bind that settles here binds it at once, and one
    /// still in flight holds its calls until it settles. A reconnect of a
    /// backend this bridge has switched away from is ignored.
    /// @param weak The backend that reconnected.
    void reconnect(const std::weak_ptr<::morph::backend::detail::IBackend>& weak) {
        note("Bridge::reconnect");
        auto pinned = weak.lock();
        if (!pinned || pinned != _backend) {
            return;
        }
        std::vector<Resume> superseded;
        std::vector<std::shared_ptr<detail::HandlerBinding>> rebound;
        for (auto const& entry : _handlers) {
            auto binding = entry.lock();
            if (!binding || (binding->shared && binding->primary.empty())) {
                continue;
            }
            binding->currentId.store(0);
            if (auto own = startBind(binding, rebindRequest(*binding), std::nullopt, {})) {
                superseded.push_back(std::move(own));
            }
        }
        auto const dropped = std::make_exception_ptr(::morph::backend::DisconnectedError{});
        for (auto& resume : superseded) {
            resume(dropped);
        }
    }

    ::morph::exec::detail::OwnerAffinity _affinity;
    std::shared_ptr<::morph::backend::detail::IBackend> _backend;
    std::vector<std::weak_ptr<detail::HandlerBinding>> _handlers;
    ::morph::session::Context _defaultSession;
    ::morph::session::Principal _principal;
    // Client-side execute deadline (see setExecuteDeadline). The scheduler is
    // created lazily and shared with every sink that armed an entry on it, so a
    // sink's disarm outlives the bridge safely.
    std::chrono::milliseconds _executeDeadline{0};
    std::shared_ptr<::morph::async::detail::TimeoutScheduler> _timeoutScheduler;
    // Instance subscriptions; see `detail::SubscriptionRegistry`. Shared with
    // every sink, which reads its atomic count on the backend's thread to
    // decide whether a result needs the owner at all. Never null.
    std::shared_ptr<detail::SubscriptionRegistry<detail::HandlerBinding>> _subscriptions{
        std::make_shared<detail::SubscriptionRegistry<detail::HandlerBinding>>()};
    // Count of calls not yet resolved -- see pendingCalls(). Shared with every
    // sink, which decrements it on the backend's thread. Never null.
    std::shared_ptr<std::atomic<std::size_t>> _pendingCalls{std::make_shared<std::atomic<std::size_t>>(0)};
    // Declared last, so destroyed first: every continuation queued on the owner
    // finds its token expired before any member it would touch is torn down.
    ::morph::async::CallbackScope _callbacks;
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
/// deregisters it. The handler is non-copyable, and belongs to its bridge's
/// owner: it is used and destroyed there.
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
/// @tparam Sharing `NoSharing` (the default) or `AllowShared`.
template <typename Model, typename Sharing = NoSharing>
// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions)
class BridgeHandler {
public:
    /// @brief Whether this handler participates in the shared instance directory.
    static constexpr bool kShared = std::is_same_v<Sharing, AllowShared>;

    /// @brief Constructs and registers the handler using the default model factory.
    ///
    /// On the bridge's owner the bind is issued here; a backend that settles
    /// it before returning has bound the handler when this returns. Anywhere
    /// else — inside a running action on a pool thread — the registration is
    /// posted to the owner. Either way nothing waits: a call made before the
    /// bind settles is held and dispatched when it does.
    ///
    /// @param bridge   The bridge to register on. Borrowed: it must outlive
    ///                 every call made on this handler.
    /// @param guiExec  Executor used to deliver `Completion` callbacks: the
    ///                 bridge's owner, or an executor that runs its tasks on
    ///                 the owner's thread (a debug build checks it once).
    ///                 Borrowed: it must outlive this handler.
    BridgeHandler(Bridge& bridge MORPH_LIFETIMEBOUND, ::morph::exec::IExecutor* guiExec MORPH_LIFETIMEBOUND)
        : BridgeHandler(bridge, guiExec, Bridge::makeBinding<Model>(kShared)) {
        static_assert(!kShared || ::morph::model::KeyedModel<Model>,
                      "BridgeHandler<Model, AllowShared> requires Model to declare a PrimaryKey alias");
    }

    /// @brief Constructs the handler with a pre-built binding (for dependency injection).
    ///
    /// Registers @p binding exactly as the constructor above registers its own.
    /// @param bridge   The bridge to register on. Borrowed, on the same terms as
    ///                 the constructor above.
    /// @param guiExec  Executor for callback delivery, on the same terms as the
    ///                 constructor above.
    /// @param binding  Pre-built binding whose factory captures injected
    ///                 dependencies. Its `contextKey` must be set before this call.
    BridgeHandler(Bridge& bridge MORPH_LIFETIMEBOUND, ::morph::exec::IExecutor* guiExec MORPH_LIFETIMEBOUND,
                  std::shared_ptr<detail::HandlerBinding> binding)
        : _bridge{bridge},
          _liveness{bridge.liveness()},
          _affinity{bridge.affinity()},
          _guiExec{guiExec},
          _binding{std::move(binding)} {
        _bridge.adoptHandler(_binding);
#ifndef NDEBUG
        if (_guiExec != nullptr && _guiExec != &_affinity.owner()) {
            // Once, where the executor runs its tasks: a guiExec on another
            // thread would deliver this handler's callbacks off the owner.
            _guiExec->post([affinity = _affinity] { affinity.note("BridgeHandler::guiExec"); });
        }
#endif
    }

    /// @brief Deregisters the binding from the bridge, on the owner.
    ///
    /// Every call still held for a bind in flight is rejected with
    /// `HandlerDestroyedError`. Destroyed after its bridge — still on the
    /// owner — it finds the bridge's token expired and deregisters nothing;
    /// on one thread that check is exact.
    ~BridgeHandler() {
        _affinity.note("BridgeHandler::~BridgeHandler");
        if (_liveness.active()) {
            _bridge.deregisterHandler(_binding);
        }
    }

    BridgeHandler(const BridgeHandler&) = delete;
    BridgeHandler& operator=(const BridgeHandler&) = delete;

    /// @brief Dispatches @p action via the underlying `Bridge` and returns a `Completion`.
    ///
    /// The bridge's currently-installed default session is attached
    /// automatically. A call made before the handler's bind has settled is held
    /// and dispatched when it does; a failed bind rejects it with the bind's
    /// error.
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
        if constexpr (kShared && ::morph::model::detail::PayloadKeyed<Action>) {
            // The action names its instance: attach (or re-point) before
            // dispatching, so the call lands on the instance it asked for. Key
            // extraction is user code, so a throw from it resolves the
            // Completion rather than escaping execute().
            std::string key;
            try {
                key = ::morph::model::ActionKeyTraits<Action>::key(action);
            } catch (...) {
                auto [failed, promise] = ::morph::async::Completion<R>::makeSettleable(_guiExec);
                promise.reject(std::current_exception());
                return std::move(failed);
            }
            return _bridge.template executeAttachedVia<Model, Action>(_binding, std::move(action), _guiExec,
                                                                      std::move(key));
        } else if constexpr (kShared && ::morph::model::detail::ResultKeyed<Action>) {
            // The action *creates* the instance and its result carries the
            // generated key: run it on an anonymous instance, then promote that
            // instance in place before any user callback observes the result.
            return _bridge.template executeCreatingVia<Model, Action>(_binding, std::move(action), _guiExec);
        } else {
            return _bridge.template executeVia<Model, Action>(_binding, std::move(action), _guiExec);
        }
    }

    /// @brief Attaches (or re-points) this handler to the instance for @p key.
    ///
    /// Creates the instance if no live instance holds @p key, otherwise joins the
    /// existing one. Re-pointing an already-attached handler releases the old
    /// instance, which survives only if another handler still holds it. A
    /// backend that settles before returning has attached the handler by the
    /// time this returns; otherwise calls made meanwhile are held until it
    /// has. A refused attach is logged and leaves the handler on the instance
    /// it held; a call held behind it is rejected with its error only when
    /// the handler has no instance to fall back to.
    ///
    /// @tparam M Defaulted to `Model`; never named explicitly. Present only so the
    ///         signature is instantiated lazily, since `PrimaryKeyOf` is
    ///         ill-formed for an unkeyed model.
    /// @param key Primary key of the instance to attach to.
    template <typename M = Model>
    void attach(const ::morph::model::PrimaryKeyOf<M>& key)
        requires kShared
    {
        _bridge.template attachHandler<Model>(_binding, ::morph::model::keyToString(key));
    }

    /// @brief This handler's current primary key, or `nullopt` if unattached.
    /// @tparam M Defaulted to `Model`; never named explicitly. See `attach`.
    /// @return The attached key, or `nullopt` before the first attach settles.
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
    /// remote mode answering costs a round trip. The result is a snapshot,
    /// stale the moment it arrives.
    ///
    /// @tparam M Defaulted to `Model`; never named explicitly. See `attach`.
    /// @return Completion resolving on the GUI executor with the live keys.
    template <typename M = Model>
    [[nodiscard]] ::morph::async::Completion<std::vector<::morph::model::PrimaryKeyOf<M>>> instances()
        requires kShared
    {
        using Key = ::morph::model::PrimaryKeyOf<M>;
        auto [comp, promise] = ::morph::async::Completion<std::vector<Key>>::makeSettleable(_guiExec);
        auto answer =
            std::make_shared<typename ::morph::async::Completion<std::vector<Key>>::Promise>(std::move(promise));
        // The backend's answer is delivered on the bridge's owner, which is
        // where this runs, so attaching to it here is attaching on its owner.
        _bridge.template instancesOf<Model>(_bridge.owner())
            .thenDetached([answer](const std::vector<std::string>& raws) {
                try {
                    std::vector<Key> keys;
                    keys.reserve(raws.size());
                    for (const auto& raw : raws) {
                        keys.push_back(::morph::model::keyFromString<Key>(raw));
                    }
                    answer->resolve(std::move(keys));
                } catch (...) {
                    answer->reject(std::current_exception());
                }
            })
            .onErrorDetached([answer](const std::exception_ptr& error) { answer->reject(error); });
        return std::move(comp);
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
        // Sharing policy (see `ActionExecuteRegistry::registerAction`'s doc
        // comment): a NoSharing-only executor would static_cast `this` to the
        // wrong BridgeHandler<Model, Sharing> instantiation for a shared
        // handler, silently skipping its attach/promote step.
        return ActionExecuteRegistry::instance().execute<Sharing>(::morph::model::ModelTraits<Model>::typeId(),
                                                                  actionType, this, bodyJson);
    }

    /// @brief Whether `Model` has an action registered under @p actionId, without
    ///        invoking it.
    ///
    /// A pure existence check over `ActionExecuteRegistry` -- the same
    /// registry `executeJson` dispatches through.
    /// @param actionId Action type id to check.
    /// @return `true` if `executeJson(actionId, ...)` would find a registered action.
    [[nodiscard]] bool servesAction(std::string_view actionId) const noexcept {
        return ActionExecuteRegistry::instance().contains<Sharing>(::morph::model::ModelTraits<Model>::typeId(),
                                                                   actionId);
    }

    /// @brief The executor used to deliver this handler's `Completion` callbacks.
    /// @return The GUI/callback executor passed at construction.
    [[nodiscard]] ::morph::exec::IExecutor* guiExecutor() const noexcept { return _guiExec; }

    /// @brief The executor this handler and its bridge belong to.
    /// @return The bridge's owner.
    [[nodiscard]] ::morph::exec::IExecutor& owner() const noexcept { return _affinity.owner(); }

    /// @brief Whether the calling thread is on this handler's owner.
    /// @return True inside one of the owner's tasks, or on the thread that
    ///         constructed the bridge outside them.
    [[nodiscard]] bool onOwner() const noexcept { return _affinity.here(); }

    /// @brief Whether this handler currently has a live backend instance.
    ///
    /// A point-in-time atomic read, safe from any thread. Nothing needs to wait
    /// on it: a call made while the handler's bind is in flight is held until
    /// the bind settles.
    /// @return `true` if the handler is currently bound to a `ModelId`.
    [[nodiscard]] bool isBound() const noexcept { return Bridge::isBound(_binding); }

    /// @brief Subscribes to results of type @p R produced on the attached instance.
    ///
    /// Fires whenever an `R` is produced on the instance this handler is
    /// attached to — by this handler, by another handler sharing the instance,
    /// or by another screen entirely.
    ///
    /// One callback per `(handler, R)`: subscribing again replaces the previous
    /// one. Callbacks are delivered on this handler's executor. Failed actions
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
    /// Same subscription as `subscribe<R>(cb)`, except that @p cb runs only
    /// while @p scope is alive and un-stopped. The sink itself is *not* pruned
    /// when the scope goes inactive: delivery is refused, the subscription
    /// entry stays until `unsubscribe<R>()` or handler destruction removes it.
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

    /// @brief Removes this handler's subscription for @p R.
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
    // The bridge's liveness, checked by the destructor: a handler destroyed
    // after its bridge, on the owner, deregisters nothing.
    ::morph::async::CallbackToken _liveness;
    // The owner, kept by value so the destructor can check it without the bridge.
    ::morph::exec::detail::OwnerAffinity _affinity;
    ::morph::exec::IExecutor* _guiExec;
    std::shared_ptr<detail::HandlerBinding> _binding;
};

/// Out-of-line definition of ActionExecuteRegistry::registerAction.
/// Placed here after BridgeHandler is fully defined so we can safely cast and call its methods.
///
/// Builds one executor per `Sharing` policy the framework defines
/// (`NoSharing`, `AllowShared`) from the same generic-lambda template,
/// `static_cast`ing `handlerVoid` to the matching `BridgeHandler<Model,
/// Sharing>*` in each — see bridge.md's design-decision entry for why a single
/// `NoSharing`-only executor is unsound for a shared handler.
template <typename Model, typename Action>
inline void ActionExecuteRegistry::registerAction(std::string_view modelId, std::string_view actionId) {
    auto makeExecutor = []<typename Sharing>() {
        return [](void* handlerVoid, std::string_view bodyJson) -> ::morph::async::Completion<std::string> {
            auto* handler = static_cast<BridgeHandler<Model, Sharing>*>(handlerVoid);
            auto resultState = std::make_shared<::morph::async::detail::CompletionState<std::string>>();
            // Named before anything can settle it: a settle posts its delivery to
            // the owner the state already has, and a decode failure settles here.
            ::morph::async::Completion<std::string> completion{resultState, handler->guiExecutor()};
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
            return completion;
        };
    };
    _executors[Key{std::string{modelId}, std::string{actionId}, std::type_index{typeid(NoSharing)}}] =
        makeExecutor.template operator()<NoSharing>();
    _executors[Key{std::string{modelId}, std::string{actionId}, std::type_index{typeid(AllowShared)}}] =
        makeExecutor.template operator()<AllowShared>();
}

}  // namespace morph::bridge
