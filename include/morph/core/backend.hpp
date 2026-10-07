// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../attributes.hpp"
#include "../session/session.hpp"
#include "completion.hpp"
#include "detail/instance_directory.hpp"
#include "detail/owner_affinity.hpp"
#include "model.hpp"
#include "observability.hpp"
#include "profiler.hpp"
#include "registry.hpp"
#include "strand.hpp"

namespace morph::backend {

namespace detail {

/// @brief All the information needed to dispatch one action call through a backend.
///
/// Backends that execute locally use `localOp` directly. Remote backends
/// serialize via `serializeAction` and deserialize replies via `deserializeResult`.
///
/// @par Why function *pointers*, and why the action travels beside them
/// The three callables are plain function pointers and the action object they
/// operate on rides in `action`, rather than each callable capturing its own
/// handle to it. That is a deliberate allocation decision, not a style
/// preference. `Bridge::executeVia` builds an `ActionCall` on **every** call,
/// including one that a `LocalBackend` will serve and that will therefore never
/// touch `serializeAction` or `deserializeResult`. A `std::function` whose
/// target is not trivially copyable cannot use libstdc++'s small-object buffer,
/// so a lambda capturing a `shared_ptr<Action>` heap-allocates — three captured
/// callables cost up to three allocations per dispatch whichever path the call
/// takes. Stateless operations parameterised on the action cost none: the
/// per-`(Model, Action)` behaviour is a compile-time constant, addressed rather
/// than copied. Measured with `morph_bench_alloc`: this shape and the two
/// `string_view` ids below together account for 3 of the allocations a local
/// round trip would otherwise make, out of 17.
///
/// @par Lifetime contract for the callables
/// `serializeAction` and `localOp` take the action as an opaque pointer and do
/// **not** own it: `action` does. A backend that defers either call past the
/// `ActionCall`'s own lifetime must carry a copy of the `action` handle with
/// it — `LocalBackend::execute` does exactly that when it posts to the strand.
/// `deserializeResult` never reads the action and so may outlive it, which is
/// what lets the remote backends store it in their pending-reply table.
struct ActionCall {
    /// @brief String id of the target model type (from `ModelTraits`).
    ///
    /// A view, not a copy. `ModelTraits<Model>::typeId()` is `constexpr` and
    /// returns a view of the string literal `BRIDGE_REGISTER_MODEL` was given,
    /// so the referent has static storage duration and outlives every call.
    /// A hand-built `ActionCall` must observe the same rule: the id must
    /// outlive the dispatch, which a string literal does and a temporary
    /// `std::string` does not.
    std::string_view modelTypeId;

    /// @brief String id of the action type (from `ActionTraits`).
    ///
    /// Same storage rule as `modelTypeId`.
    std::string_view actionTypeId;

    /// @brief Type-erased owner of the action object `serializeAction` and
    ///        `localOp` read.
    ///
    /// Null only for a hand-built call whose callables ignore their action
    /// argument.
    std::shared_ptr<void> action;

    /// @brief Serialises `action` to JSON. Called only on the remote path.
    std::string (*serializeAction)(const void* action) = nullptr;

    /// @brief Deserialises a JSON reply into the opaque result `shared_ptr<void>`.
    std::shared_ptr<void> (*deserializeResult)(std::string_view json) = nullptr;

    /// @brief Executes `action` directly against a model holder. Used on the local path.
    ///
    /// Takes the action as a mutable pointer because the local path recomputes
    /// an action's computed fields in place before the validator or the model
    /// sees it (`Bridge::executeVia`, `morph::forms::recomputeAll`).
    std::shared_ptr<void> (*localOp)(::morph::model::detail::IModelHolder& holder, void* action) = nullptr;

    /// @brief Receives a Task handler's outcome on the local path: the opaque
    ///        result, or the exception (with a null result).
    using LocalDone = std::function<void(std::shared_ptr<void>, std::exception_ptr)>;

    /// @brief Starts a Task handler against a model holder, on the model's
    ///        strand, and reports its outcome through the last argument when the
    ///        Task completes. Set instead of `localOp` for an action whose
    ///        handler returns `core::async::Task`; see
    ///        `docs/spec/core/coroutines.md`.
    ///
    /// Takes the action's owner rather than a borrowed pointer: the handler's
    /// frame outlives the call that starts it.
    void (*localOpAsync)(::morph::model::detail::IModelHolder& holder, std::shared_ptr<void> action,
                         const std::shared_ptr<::morph::exec::detail::TaskResumer>& executor,
                         ::core::async::StopToken token, LocalDone done) = nullptr;

    /// @brief The stop source a Task handler's token comes from, or null for
    ///        none. `Bridge::executeVia` sets one when an execute deadline is
    ///        armed, and the deadline requests stop on it.
    std::shared_ptr<::core::async::StopSource> stopSource;

    /// @brief Session context attached to this call.
    ///
    /// Local backends thread it through a thread-local before invoking `localOp`;
    /// remote backends serialise it into the wire envelope.
    ::morph::session::Context session;

    /// @brief Invokes `serializeAction` on the action this call owns.
    ///
    /// The one place the borrow in `serializeAction`'s signature is closed, so
    /// no remote backend has to remember to pair the pointer with its owner --
    /// and the one place the null check lives. An empty `std::function` used to
    /// raise `std::bad_function_call` here; a null function pointer would be
    /// undefined behaviour instead, so the check is explicit and names the
    /// field.
    ///
    /// @return The JSON body for the wire envelope.
    /// @throws std::runtime_error if `serializeAction` is null.
    [[nodiscard]] std::string serializeBody() const {
        if (serializeAction == nullptr) {
            throw std::runtime_error{"ActionCall::serializeAction is null: nothing to serialise"};
        }
        return serializeAction(action.get());
    }
};

/// @brief One *bind* request: everything needed to acquire a model instance.
///
/// The request half of `IBackend::bindModel`, the one acquire verb. Its *shape*
/// selects the behaviour:
///
/// | `primary` | `current` | Meaning |
/// |---|---|---|
/// | empty | `0` | A private instance, never entered in the shared directory. |
/// | non-empty | `0` | Register-or-attach the shared instance for `(typeId, primary)`. |
/// | non-empty | non-zero | Re-point from `current` to the shared instance for `(typeId, primary)`. |
/// | empty | non-zero | Give `current` up and bind a private instance instead. |
///
/// Every string is **owned**: a bind may outlive the frame that issued it, so a
/// `string_view` into the caller's stack would dangle for a backend that builds
/// its envelope after the call returns.
struct BindRequest {
    /// @brief String type-id of the model to instantiate (from `ModelTraits`).
    std::string typeId;

    /// @brief Callable that constructs the `IModelHolder`. Local path only; a
    ///        wire backend never calls it, and an attach to a live shared
    ///        instance does not call it either.
    std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory;

    /// @brief Entity key for the action log; empty if none. See `journal::LogEntry::entityKey`.
    std::string contextKey;

    /// @brief Canonical string encoding of the primary key. Empty means "no
    ///        identity": the bind produces a private instance that never enters
    ///        the shared directory.
    std::string primary;

    /// @brief Instance currently held, or `ModelId{0}` if none. Non-zero makes
    ///        this bind a *re-point*: the backend releases it once the
    ///        replacement is acquired.
    ::morph::exec::detail::ModelId current{};
};

/// @brief One *promote* request: file an already-live instance under a key.
///
/// The request half of `IBackend::promoteModel`, the structural counterpart of
/// `assignPrimary`. Its strings are owned for the same reason `BindRequest`'s
/// are.
struct PromoteRequest {
    /// @brief Live instance to promote.
    ::morph::exec::detail::ModelId mid{};

    /// @brief Model type id — the directory's first key component.
    std::string typeId;

    /// @brief Canonical string encoding of the key to file @p mid under.
    std::string primary;
};

/// @brief Abstract interface for execution backends (local, remote, …).
///
/// A backend owns model instances and dispatches actions against them.
/// `Bridge` holds one active backend at a time and can swap it via
/// `Bridge::switchBackend()`.
///
/// A backend installed in a `Bridge` belongs to the bridge's owner: the bridge
/// calls every verb from there (`setOwner` tells the backend which executor
/// that is), so a backend keeps the state those verbs touch without a lock.
/// What arrives from the backend's own threads — a reply, a reconnect — reaches
/// the bridge through an executor the bridge names: `bindModel`/`promoteModel`
/// deliver on the one they are given, and `setReconnectHandler` posts to the
/// one it is given.
// NOLINTBEGIN(cppcoreguidelines-special-member-functions)
struct IBackend {
    virtual ~IBackend() = default;

    /// @brief Registers a new model instance and returns its opaque id.
    /// @param typeId  String type-id of the model to instantiate.
    /// @param factory Callable that constructs the `IModelHolder` (local path only).
    /// @return Newly assigned `ModelId`.
    virtual ::morph::exec::detail::ModelId registerModel(
        const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory) = 0;

    /// @brief Registers a new model instance, additionally passing @p contextKey —
    ///        the instance's stable identity (e.g. an account id) — through to
    ///        backends that can make use of it.
    ///
    /// Default implementation forwards to `registerModel()` and drops @p contextKey,
    /// which is exactly correct for `LocalBackend`: the caller's own @p factory
    /// closure already captures whatever identity it needs directly (see
    /// `IModelHolder::attachActionLog`), so there is nothing for the backend to
    /// forward. Backends whose model instances live behind a wire protocol
    /// (`SimulatedRemoteBackend`) override this to carry @p contextKey across —
    /// see `wire::Envelope::contextKey` and `ServerConfig::logProvider`.
    /// @param typeId     String type-id of the model to instantiate.
    /// @param factory    Callable that constructs the `IModelHolder` (local path only).
    /// @param contextKey Stable identity of the new instance; empty if none.
    /// @return Newly assigned `ModelId`.
    virtual ::morph::exec::detail::ModelId registerModelWithContext(
        const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
        std::string_view contextKey) {
        (void)contextKey;
        return registerModel(typeId, std::move(factory));
    }

    /// @brief Enters an already-live instance into the directory under @p primary.
    ///
    /// The *promotion* half of keyed instances, and what makes a result-sourced
    /// key work without losing state: an action that creates its own entity runs
    /// on an instance that does not yet have a key, and the key only exists once
    /// the result comes back. Re-pointing to a freshly created instance would
    /// strand everything the create just did, so instead the instance the action
    /// ran on is given the generated key in place.
    ///
    /// A no-op when @p primary is empty, when @p mid is not live, when another
    /// instance already holds that key, when @p mid itself already holds a
    /// *different* real key, or when @p mid was *created* for a key — even one
    /// it has since been evicted from as poisoned. The existing holder of a key
    /// always wins (a promotion can never silently displace a directory entry
    /// other handlers are attached to), and an instance that was ever keyed
    /// never changes key (a promotion can never silently move one out from
    /// under handlers already attached to it, nor hand later attachers a model
    /// that still identifies itself as the entity it was built for) — only an
    /// instance that has never held a key can be promoted.
    ///
    /// @param mid     Live instance to promote.
    /// @param typeId  Model type id — the directory's first key component.
    /// @param primary Canonical string encoding of the key to file it under.
    virtual void assignPrimary(::morph::exec::detail::ModelId mid, const std::string& typeId,
                               std::string_view primary) {
        (void)mid;
        (void)typeId;
        (void)primary;
    }

    /// @brief Acquires a model instance, selected by @p request's shape (see
    ///        `BindRequest`), and reports it through a `Completion` delivered
    ///        on @p cbExec.
    ///
    /// A backend that can settles the `Completion` before returning — the
    /// default does, and so does every backend whose registration needs no
    /// round trip — so a `Bridge` handler bound through it can dispatch at once.
    /// A backend whose registration is a round trip settles it when the reply
    /// arrives, on whatever thread that is; the continuation still runs on
    /// @p cbExec, which `Bridge` passes as its owner.
    ///
    /// The default implementation has no shared directory: every shape binds a
    /// private instance through `registerModelWithContext`, and a non-zero
    /// `current` is released once the replacement is acquired, so a throwing
    /// acquire never strands the caller with neither instance. An exception
    /// from either verb rejects the `Completion` rather than propagating, so a
    /// caller has one failure channel.
    ///
    /// @param request Owning bind request; moved from.
    /// @param cbExec  Executor the continuation is delivered on. Borrowed: it
    ///                must outlive the returned `Completion`.
    /// @return A `Completion` resolved with the bound `ModelId`, or rejected
    ///         with the failure.
    virtual ::morph::async::Completion<::morph::exec::detail::ModelId> bindModel(BindRequest request,
                                                                                 ::morph::exec::IExecutor& cbExec) {
        auto [completion, promise] =
            ::morph::async::Completion<::morph::exec::detail::ModelId>::makeSettleable(&cbExec);
        try {
            auto const next = registerModelWithContext(request.typeId, std::move(request.factory), request.contextKey);
            if (request.current.v != 0U) {
                deregisterModel(request.current);
            }
            promise.resolve(next);
        } catch (...) {
            promise.reject(std::current_exception());
        }
        return std::move(completion);
    }

    /// @brief Files an already-live instance under a key: the structural
    ///        counterpart of `assignPrimary`.
    ///
    /// The default implementation calls `assignPrimary` and settles the
    /// returned `Completion` before returning, echoing @p request's `mid`
    /// back — including for every no-op case `assignPrimary` documents
    /// (empty primary, dead `mid`, key already taken, `mid` already keyed
    /// differently), which are not backend failures and therefore resolve
    /// rather than reject.
    ///
    /// @param request Owning promote request. By value, matching `bindModel`,
    ///                so an overriding backend can move it into its pending
    ///                state; this default only reads it.
    /// @param cbExec  Executor the continuation is delivered on. Borrowed: it
    ///                must outlive the returned `Completion`.
    /// @return A `Completion` resolved with `request.mid`, or rejected with the
    ///         failure.
    // NOLINTBEGIN(performance-unnecessary-value-param) — by value to match `bindModel` and because an overriding
    // backend moves the request into its pending-reply state; this default happens only to read it.
    virtual ::morph::async::Completion<::morph::exec::detail::ModelId> promoteModel(PromoteRequest request,
                                                                                    ::morph::exec::IExecutor& cbExec) {
        auto [completion, promise] =
            ::morph::async::Completion<::morph::exec::detail::ModelId>::makeSettleable(&cbExec);
        try {
            assignPrimary(request.mid, request.typeId, request.primary);
            promise.resolve(request.mid);
        } catch (...) {
            promise.reject(std::current_exception());
        }
        return std::move(completion);
    }
    // NOLINTEND(performance-unnecessary-value-param)

    /// @brief Tells this backend which owner its caller runs on.
    ///
    /// `Bridge` calls it when it installs the backend, with its own affinity,
    /// and calls every other verb from that owner afterwards. A backend that
    /// keeps state for those verbs records it to check, in a debug build, that
    /// each is called there. Default implementation: ignore it.
    /// @param owner The bridge's owner affinity. Its executor is borrowed: it
    ///        must outlive this backend's use by that bridge.
    virtual void setOwner(const ::morph::exec::detail::OwnerAffinity& owner) { (void)owner; }

    /// @brief Lists the primary keys of live shared instances of @p typeId.
    ///
    /// Only instances bound with a non-empty `primary` appear; a private
    /// instance is invisible to the directory by construction. The result is a
    /// snapshot and is stale the moment it is returned.
    ///
    /// Synchronous. `instances()` is the same question answered through a
    /// `Completion`, which is what `Bridge` asks.
    ///
    /// @param typeId String type-id to enumerate.
    /// @return Canonical key strings of the live shared instances; empty by default.
    virtual std::vector<std::string> listInstances(const std::string& typeId) {
        (void)typeId;
        return {};
    }

    /// @brief Lists the primary keys of live shared instances of @p typeId,
    ///        answered through a `Completion` delivered on @p cbExec.
    ///
    /// What `BridgeHandler::instances()` reaches. The default answers from
    /// `listInstances` and settles before returning, so a backend whose
    /// directory is local needs nothing more. A backend whose directory is
    /// across a round trip overrides it and settles when the reply arrives.
    /// A throw from `listInstances` rejects the `Completion` rather than
    /// propagating, so a caller has one failure channel.
    ///
    /// @param typeId String type-id to enumerate.
    /// @param cbExec Executor the answer is delivered on. Borrowed: it must
    ///        outlive the returned `Completion`.
    /// @return A `Completion` resolved with the canonical key strings of the
    ///         live shared instances, or rejected with the failure.
    virtual ::morph::async::Completion<std::vector<std::string>> instances(const std::string& typeId,
                                                                           ::morph::exec::IExecutor& cbExec) {
        auto [completion, promise] = ::morph::async::Completion<std::vector<std::string>>::makeSettleable(&cbExec);
        try {
            promise.resolve(listInstances(typeId));
        } catch (...) {
            promise.reject(std::current_exception());
        }
        return std::move(completion);
    }

    /// @brief Removes the model identified by @p mid from the backend.
    ///
    /// For a shared instance this *decrements* its attach count and destroys the
    /// instance only when the count reaches zero, so one caller releasing an
    /// instance never tears it out from under another that is still attached.
    /// @param mid Id of the instance to release.
    virtual void deregisterModel(::morph::exec::detail::ModelId mid) = 0;

    /// @brief Dispatches @p call against the model identified by @p mid.
    /// @param mid    Target model id.
    /// @param call   Bundled action; moved from.
    /// @param cbExec Executor the returned `Completion`'s callbacks are posted on.
    /// @return A `Completion` settled with the opaque result or the failure.
    virtual ::morph::async::Completion<std::shared_ptr<void>> execute(::morph::exec::detail::ModelId mid,
                                                                      ActionCall call,
                                                                      ::morph::exec::IExecutor* cbExec) = 0;

    /// @brief Dispatches @p call and settles @p sink with its outcome.
    ///
    /// The same dispatch as `execute`, with the result delivered to a sink the
    /// caller already owns instead of to a fresh `Completion` the caller then
    /// has to forward into its own. `Bridge::executeVia` hands down a sink that
    /// **is** the typed completion state the caller was given, so there is no
    /// forwarding block between the two — no erased `CompletionState`, no
    /// `.then`/`.onError` closures, no second pair of handler vectors, and one
    /// posted settle task instead of two.
    ///
    /// @par Why this has a default rather than being pure
    /// `IBackend::execute` is implemented by five production backends and
    /// roughly ten test doubles. A pure virtual here would be a fifteen-site
    /// change to gain an allocation on one path. The default below is exactly
    /// the forwarding block it replaces, so a backend that does not override it
    /// costs precisely what it costs today — no gain, no regression — and a
    /// backend that does gets the whole saving. `LocalBackend` overrides it;
    /// nothing else does yet.
    ///
    /// @par Contract
    /// Settles @p sink exactly once, through `settleValue` or `settleException`,
    /// on the same thread and at the same point in the sequence the `execute`
    /// overload would have resolved its completion. A backend that also has to
    /// answer `cancelPending` must track the sink, not a state of its own.
    /// Throwing out of this call is permitted and means the dispatch never
    /// started — `Bridge::executeVia` undoes its pending count and its deadline
    /// on that path, exactly as it does for `execute`.
    ///
    /// @param mid    Target model id.
    /// @param call   Bundled action; moved from.
    /// @param cbExec Executor for delivering callbacks attached to the caller's
    ///               own completion. A backend passes it along unchanged.
    /// @param sink   Where to settle the outcome. Never null.
    virtual void executeInto(::morph::exec::detail::ModelId mid, ActionCall call, ::morph::exec::IExecutor* cbExec,
                             std::shared_ptr<::morph::async::detail::ISettleSink> sink) {
        // Deliberately built from this class's own `execute`, not duplicated:
        // one definition of what a dispatch is, whichever entry point a caller
        // uses.
        execute(mid, std::move(call), cbExec)
            .then([sink](const std::shared_ptr<void>& value) { sink->settleValue(value); })
            .onError([sink = std::move(sink)](const std::exception_ptr& exc) { sink->settleException(exc); });
    }

    /// @brief Called by `Bridge::switchBackend()` after all handlers are re-registered.
    virtual void notifyBackendChanged() = 0;

    /// @brief Resolves every still-pending completion this backend produced with @p exc.
    ///
    /// Called by `Bridge::switchBackend()` on the outgoing backend after the swap,
    /// and by `Bridge`'s destructor. After this call, any later `setValue` /
    /// `setException` on those states is a no-op (the state is already ready), so
    /// in-flight server replies cannot resurrect a cancelled completion. A
    /// backend that runs calls it can stop (`LocalBackend`'s Task handlers)
    /// also requests stop on each one it fails.
    /// @param exc Exception delivered to every pending completion's error sink.
    virtual void cancelPending(const std::exception_ptr& exc) = 0;

    /// @brief Installs a callback invoked when the backend reconnects to its peer.
    ///
    /// Used by backends that may lose and re-establish their transport (e.g.
    /// `QtWebSocketBackend`). `Bridge` installs a handler that re-registers every
    /// live `HandlerBinding` so model ids stay valid after the reconnect.
    ///
    /// Deliberately fires only on the *second and later* connects, never the
    /// first — re-registering handlers only makes sense after a drop; on the
    /// first connect there is nothing yet to re-register. See
    /// `setConnectHandler` for a hook that also covers the first connect.
    ///
    /// The handler is never run on the backend's own thread: after a
    /// reconnect the backend posts it to @p exec, so a `Bridge`'s
    /// re-registration is an ordinary task on the bridge's owner.
    ///
    /// Default implementation: store-and-ignore. Backends with no transport (e.g.
    /// `LocalBackend`) never invoke it.
    /// @param handler Callable posted to @p exec after each successful
    ///                reconnect. Pass `nullptr` to clear.
    /// @param exec    Executor the handler is posted to. Borrowed: it must
    ///                outlive the installation. May be null only when
    ///                @p handler is.
    // NOLINTNEXTLINE(performance-unnecessary-value-param): the base discards it; overrides take it by value and move it in.
    virtual void setReconnectHandler(std::function<void()> handler, ::morph::exec::IExecutor* exec) {
        (void)handler;
        (void)exec;
    }

    /// @brief Installs a callback invoked on every successful connect, including the first.
    ///
    /// `setReconnectHandler` deliberately skips the first connect (there is
    /// nothing to re-register yet); this is the complementary hook for UI that
    /// needs to know the transport is up at all — a "connecting… / connected /
    /// offline" status indicator, for instance. `waitForConnected()` (where a
    /// concrete backend offers one, e.g. `QtWebSocketBackend`) answers the same
    /// question but blocks the calling thread, which is unusable on a
    /// browser/WASM main thread and undesirable even on desktop if it means
    /// blocking startup on a network round-trip; this hook is fired
    /// asynchronously instead.
    ///
    /// Default implementation: store-and-ignore. Backends with no transport (e.g.
    /// `LocalBackend`) never invoke it.
    /// @param handler Callable invoked on the backend's transport thread after
    ///                every successful connect (first and subsequent). Pass
    ///                `nullptr` to clear.
    virtual void setConnectHandler(const std::function<void()>& handler) { (void)handler; }

    /// @brief Installs a callback invoked whenever the transport drops.
    ///
    /// Fires before any reconnect is scheduled, so an observer sees the
    /// disconnected state even when a retry follows immediately — a status
    /// indicator that skipped straight from "connected" to a fresh "connected"
    /// (after an instant reconnect) would misreport an outage that did happen.
    /// Without this hook a client learns the socket dropped only indirectly,
    /// when a later action fails.
    ///
    /// Default implementation: store-and-ignore. Backends with no transport (e.g.
    /// `LocalBackend`) never invoke it.
    /// @param handler Callable invoked on the backend's transport thread whenever
    ///                the connection drops. Pass `nullptr` to clear.
    virtual void setDisconnectHandler(const std::function<void()>& handler) { (void)handler; }

    /// @brief Installs the session `Bridge` stamps onto every control envelope
    ///        this backend builds (`register`, `registerShared`, `attach`,
    ///        `assign`, `deregister`).
    ///
    /// `Bridge::executeVia` already stamps `Bridge::defaultSession()` onto the
    /// `ActionCall` passed to `execute()`, so the session reaches `execute`
    /// envelopes regardless of this hook. Control messages are different: they
    /// are built directly by the concrete backend (`bindModel`, `promoteModel`,
    /// `registerModelWithContext`, `assignPrimary`, `deregisterModel`),
    /// which has no other way to learn the `Bridge`'s current session except
    /// this hook. Stamping the stored session onto those envelopes is what lets
    /// `RemoteServer::authorizeRegister` see a caller's identity and the owner
    /// principal recorded at `register` time reflect the registering session —
    /// which `authorizeInstance`'s ownership check relies on for every instance
    /// a `Bridge` registers.
    ///
    /// `Bridge::setDefaultSession()` calls this immediately, and
    /// `Bridge::switchBackend()` calls it on the new backend before any
    /// re-registration runs, so every control envelope built afterward carries
    /// the current session. Default implementation: store-and-ignore, matching
    /// `setReconnectHandler`'s pattern. `LocalBackend` needs no override — the
    /// local path never serialises a session onto a wire envelope in the first
    /// place (see docs/spec/session/session.md).
    /// @param session Session to stamp onto every subsequently built control envelope.
    virtual void setSession(::morph::session::Context session) { (void)session; }
};
// NOLINTEND(cppcoreguidelines-special-member-functions)

}  // namespace detail

/// @brief Thrown to in-flight `Completion`s when `Bridge::switchBackend()` runs.
///
/// Surfaces in the `.onError(...)` callback so the GUI can retry on the new backend
/// or surface a "backend changed" message — there is no public cancel API on
/// `Completion` itself.
struct BackendChangedError : std::runtime_error {
    /// @brief Constructs the error with a canned diagnostic message.
    BackendChangedError() : std::runtime_error{"backend changed before completion resolved"} {}
};

/// @brief Thrown to in-flight `Completion`s when `Bridge` is destroyed.
struct BridgeDestroyedError : std::runtime_error {
    /// @brief Constructs the error with a canned diagnostic message.
    BridgeDestroyedError() : std::runtime_error{"bridge destroyed before completion resolved"} {}
};

/// @brief Thrown to every call a `BridgeHandler` still holds for its bind when
///        the handler is destroyed before the bind settles.
struct HandlerDestroyedError : std::runtime_error {
    /// @brief Constructs the error with a canned diagnostic message.
    HandlerDestroyedError() : std::runtime_error{"handler destroyed before its bind settled"} {}
};

/// @brief Thrown to in-flight `Completion`s when a transport drops mid-call (e.g. a
///        Qt WebSocket disconnect). The framework retries the call on reconnect if
///        the backend supports it; otherwise the GUI's `.onError(...)` runs.
struct DisconnectedError : std::runtime_error {
    /// @brief Constructs the error with a canned diagnostic message.
    DisconnectedError() : std::runtime_error{"transport disconnected before completion resolved"} {}
};

/// @brief Thrown to a pending `Completion` when the server-side
///        `morph::backend::LimitPolicy::executeTimeout` elapses before the
///        model's action replies.
///
/// The action keeps running to completion on its strand — morph never
/// interrupts an in-flight `Model::execute` — but the caller's wait is bounded.
/// Distinguishes a timeout from any other `err` reply (a generic
/// `std::runtime_error` on `QtWebSocketBackend` / `SimulatedRemoteBackend`), so
/// callers can retry or surface a specific "request timed out" message. See
/// `docs/spec/core/backend.md` (`LimitPolicy`).
struct TimeoutError : std::runtime_error {
    /// @brief Constructs the error with a canned diagnostic message.
    TimeoutError() : std::runtime_error{"execute timed out on the server"} {}
};

/// @brief Thrown to a pending `Completion` when `Bridge::setExecuteDeadline`'s
///        duration elapses before any reply arrives — a frame silently
///        dropped by `QtWebSocketServerConfig::messagesPerSecond`, or a
///        genuinely hung server, either way.
///
/// Distinct from `TimeoutError`: that type means the *server* explicitly
/// replied that it hit `LimitPolicy::executeTimeout` while the action was
/// still running. `ClientTimeoutError` means the client gave up waiting —
/// no reply of any kind arrived, so whether the server ever received the
/// request, is still processing it, or replied to a connection that had
/// already dropped is unknown. See `docs/spec/core/completion.md`.
struct ClientTimeoutError : std::runtime_error {
    /// @brief Constructs the error with a canned diagnostic message.
    ClientTimeoutError() : std::runtime_error{"execute timed out waiting for any reply"} {}
};

/// @brief Makes a blocking backend reach its caller without blocking it — and
///        without touching the backend.
///
/// A decorator, not a base class: it wraps an existing `IBackend` and runs
/// every one of its verbs on one control strand over an executor this adapter
/// names. That strand is the wrapped backend's one owner, so a backend that
/// keeps its state without a lock (`LocalBackend`) stays correct behind it,
/// and the caller's thread never pays for a blocking control call.
///
/// @par What it does and does not change
/// The wrapped backend still blocks — nothing here makes a nested event loop
/// or a socket round trip non-blocking. What changes is **which thread pays**:
/// `bindModel` returns at once with an unsettled `Completion`, settled from the
/// strand when the wrapped backend's bind has run there, and `execute` returns
/// at once too. A single-threaded WASM main thread has no such executor to
/// offer and is therefore not what this adapter is for; that case needs a
/// backend with a genuinely non-blocking path.
///
/// @par Why the executor is required rather than optional
/// "Where does the blocking happen" is the only question this class exists to
/// answer, so it is a constructor parameter with no default.
///
/// @par Ordering
/// Every verb is queued on the one strand, so the wrapped backend sees them
/// one at a time and in the order they were issued. `~SynchronousBackendAdapter`
/// waits for every queued and in-flight call, so a reply can never land in a
/// destroyed adapter; the executor must therefore still be running tasks when
/// this adapter is destroyed (see docs/spec/concurrency_and_lifetimes.md,
/// "Destruction ordering"). The single-threaded WebAssembly build has no
/// thread to wait for: there the calls still queued are dropped.
///
/// @par Owner
/// The adapter itself belongs to its caller's owner (`setOwner`): `bindModel`,
/// `promoteModel` and `cancelPending` keep the list of pending binds there,
/// without a lock. The synchronous verbs (`registerModel`,
/// `registerModelWithContext`, `assignPrimary`, `listInstances`) wait for the
/// strand, so they must not be called from the executor's only thread.
class SynchronousBackendAdapter : public detail::IBackend {
public:
    /// @brief Wraps @p inner, running every one of its verbs on @p blockingExec.
    /// @param inner        Backend to wrap. Owned (shared): the adapter keeps it
    ///                     alive for as long as any call it queued is still in
    ///                     flight. Must not be null.
    /// @param blockingExec Executor the wrapped backend's calls run on.
    ///                     Borrowed: it must outlive this adapter and keep
    ///                     running tasks until the destructor's wait has
    ///                     completed.
    /// @throws std::invalid_argument if @p inner is null.
    SynchronousBackendAdapter(std::shared_ptr<detail::IBackend> inner,
                              ::morph::exec::IExecutor& blockingExec MORPH_LIFETIMEBOUND)
        : _inner{std::move(inner)}, _control{blockingExec} {
        if (_inner == nullptr) {
            throw std::invalid_argument{"SynchronousBackendAdapter requires a backend to wrap"};
        }
    }

    /// @brief Waits for every queued and in-flight call; see "Ordering" above.
    ~SynchronousBackendAdapter() override {
        _control.teardown([] {});
    }

    SynchronousBackendAdapter(const SynchronousBackendAdapter&) = delete;
    SynchronousBackendAdapter& operator=(const SynchronousBackendAdapter&) = delete;
    SynchronousBackendAdapter(SynchronousBackendAdapter&&) = delete;
    SynchronousBackendAdapter& operator=(SynchronousBackendAdapter&&) = delete;

    /// @brief The producer side of a `bindModel`/`promoteModel` completion.
    ///
    /// Named because `cancelPending` has to hold these weakly; see
    /// `trackPending`.
    using BindPromise = ::morph::async::Completion<::morph::exec::detail::ModelId>::Promise;

    /// @brief The wrapped backend.
    /// @return Reference to the backend passed at construction; never null.
    [[nodiscard]] detail::IBackend& wrapped() const noexcept { return *_inner; }

    /// @brief Acquires a model instance without blocking the calling thread.
    ///
    /// Queues the wrapped backend's `bindModel` on the control strand and
    /// returns. Exactly one of the returned `Completion`'s `then`/`onError`
    /// handlers runs, on @p cbExec.
    /// @param request Owning bind request; moved from.
    /// @param cbExec  Executor the continuation is delivered on. Borrowed: it
    ///                must outlive the returned `Completion`.
    /// @return A `Completion` resolved with the bound `ModelId`, or rejected
    ///         with the wrapped backend's failure.
    ::morph::async::Completion<::morph::exec::detail::ModelId> bindModel(detail::BindRequest request,
                                                                         ::morph::exec::IExecutor& cbExec) override {
        note("SynchronousBackendAdapter::bindModel");
        return dispatch(cbExec, [request = std::move(request)](detail::IBackend& inner,
                                                               ::morph::exec::IExecutor& replyExec) mutable {
            return inner.bindModel(std::move(request), replyExec);
        });
    }

    /// @brief Files an already-live instance under a key without blocking the caller.
    ///
    /// The promote counterpart of `bindModel` above, on the same strand and
    /// with the same settling rules.
    /// @param request Owning promote request; moved from.
    /// @param cbExec  Executor the continuation is delivered on. Borrowed: it
    ///                must outlive the returned `Completion`.
    /// @return A `Completion` resolved with `request.mid`, or rejected.
    ::morph::async::Completion<::morph::exec::detail::ModelId> promoteModel(
        detail::PromoteRequest request, ::morph::exec::IExecutor& cbExec) override {
        note("SynchronousBackendAdapter::promoteModel");
        return dispatch(cbExec, [request = std::move(request)](detail::IBackend& inner,
                                                               ::morph::exec::IExecutor& replyExec) mutable {
            return inner.promoteModel(std::move(request), replyExec);
        });
    }

    /// @brief Records the caller's owner. Not forwarded: the wrapped backend's
    ///        owner is this adapter's control strand.
    /// @param owner The caller's owner.
    void setOwner(const ::morph::exec::detail::OwnerAffinity& owner) override { _affinity.emplace(owner); }

    /// @brief Runs the wrapped backend's `registerModel` on the strand and waits for it.
    /// @param typeId  String type-id of the model to instantiate.
    /// @param factory Callable that constructs the `IModelHolder`.
    /// @return The wrapped backend's `ModelId`.
    ::morph::exec::detail::ModelId registerModel(
        const std::string& typeId,
        std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory) override {
        return runAndWait([typeId, factory = std::move(factory)](detail::IBackend& inner) mutable {
            return inner.registerModel(typeId, std::move(factory));
        });
    }

    /// @brief Runs the wrapped backend's `registerModelWithContext` on the strand and waits for it.
    /// @param typeId     String type-id of the model to instantiate.
    /// @param factory    Callable that constructs the `IModelHolder`.
    /// @param contextKey Stable identity of the new instance; empty if none.
    /// @return The wrapped backend's `ModelId`.
    ::morph::exec::detail::ModelId registerModelWithContext(
        const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
        std::string_view contextKey) override {
        return runAndWait(
            [typeId, factory = std::move(factory), key = std::string{contextKey}](detail::IBackend& inner) mutable {
                return inner.registerModelWithContext(typeId, std::move(factory), key);
            });
    }

    /// @brief Runs the wrapped backend's `assignPrimary` on the strand and waits for it.
    /// @param mid     Live instance to promote.
    /// @param typeId  Model type id — the directory's first key component.
    /// @param primary Canonical string encoding of the key to file it under.
    void assignPrimary(::morph::exec::detail::ModelId mid, const std::string& typeId,
                       std::string_view primary) override {
        static_cast<void>(runAndWait([mid, typeId, key = std::string{primary}](detail::IBackend& inner) {
            inner.assignPrimary(mid, typeId, key);
            return true;
        }));
    }

    /// @brief Runs the wrapped backend's `listInstances` on the strand and waits for it.
    /// @param typeId String type-id to enumerate.
    /// @return The wrapped backend's snapshot of live shared instance keys.
    std::vector<std::string> listInstances(const std::string& typeId) override {
        return runAndWait([typeId](detail::IBackend& inner) { return inner.listInstances(typeId); });
    }

    /// @brief Queues the wrapped backend's `deregisterModel`.
    /// @param mid Instance to release.
    void deregisterModel(::morph::exec::detail::ModelId mid) override {
        post([mid](detail::IBackend& inner) { inner.deregisterModel(mid); });
    }

    /// @brief Queues the dispatch on the strand and returns its `Completion` at once.
    /// @param mid    Instance to dispatch against.
    /// @param call   The action call to dispatch.
    /// @param cbExec Executor the resulting `Completion`'s callbacks are posted on.
    /// @return A `Completion` settled by the wrapped backend.
    ::morph::async::Completion<std::shared_ptr<void>> execute(::morph::exec::detail::ModelId mid,
                                                              detail::ActionCall call,
                                                              ::morph::exec::IExecutor* cbExec) override {
        auto state = std::make_shared<::morph::async::detail::CompletionState<std::shared_ptr<void>>>();
        ::morph::async::Completion<std::shared_ptr<void>> comp{state, cbExec};
        executeInto(mid, std::move(call), cbExec,
                    std::make_shared<::morph::async::detail::CompletionSettleSink>(state));
        return comp;
    }

    /// @brief Queues the wrapped backend's `executeInto` on the strand.
    /// @param mid    Instance to dispatch against.
    /// @param call   The action call to dispatch.
    /// @param cbExec Executor for delivering callbacks attached to the caller's own completion.
    /// @param sink   Where the wrapped backend settles the outcome.
    void executeInto(::morph::exec::detail::ModelId mid, detail::ActionCall call, ::morph::exec::IExecutor* cbExec,
                     std::shared_ptr<::morph::async::detail::ISettleSink> sink) override {
        post([mid, call = std::move(call), cbExec, sink = std::move(sink)](detail::IBackend& inner) mutable {
            try {
                inner.executeInto(mid, std::move(call), cbExec, sink);
            } catch (...) {
                sink->settleException(std::current_exception());
            }
        });
    }

    /// @brief Queues the wrapped backend's `notifyBackendChanged`.
    void notifyBackendChanged() override {
        post([](detail::IBackend& inner) { inner.notifyBackendChanged(); });
    }

    /// @brief Rejects the binds *this adapter* produced, then queues the
    ///        wrapped backend's own `cancelPending`.
    ///
    /// A `bindModel` here settles from a task on the strand, holding a promise
    /// the wrapped backend never sees, so the wrapped backend's `cancelPending`
    /// reaches nothing of it: without this a bind cancelled by `~Bridge` or by
    /// `switchBackend` would go on to resolve **successfully** afterwards.
    ///
    /// Each queued bind carries a cancellation flag beside its promise, set
    /// here before the promise is rejected: a bind still queued sees it when it
    /// reaches the head of the strand and never reaches the wrapped backend, so
    /// no instance is created that nothing will deregister. A bind already
    /// running on the strand cannot be recalled; its completion stays rejected.
    /// The wrapped backend's own pending calls are cancelled on the strand, in
    /// order with everything queued before this call.
    /// @param exc Exception delivered to every still-pending completion, this
    ///            adapter's own and then the wrapped backend's.
    void cancelPending(const std::exception_ptr& exc) override {
        note("SynchronousBackendAdapter::cancelPending");
        auto snapshot = std::exchange(_pending, {});
        _compactAt = kPendingCompactFloor;
        for (auto& pendingWeak : snapshot) {
            if (auto pending = pendingWeak.lock()) {
                pending->cancelled.store(true, std::memory_order_release);
                pending->promise.reject(exc);
            }
        }
        post([exc](detail::IBackend& inner) { inner.cancelPending(exc); });
    }

    /// @brief Queues the installation on the wrapped backend.
    /// @param handler Callable posted to @p exec after a successful reconnect; `nullptr` clears.
    /// @param exec    Executor the handler is posted to.
    void setReconnectHandler(std::function<void()> handler, ::morph::exec::IExecutor* exec) override {
        post([handler = std::move(handler), exec](detail::IBackend& inner) mutable {
            inner.setReconnectHandler(std::move(handler), exec);
        });
    }

    /// @brief Queues the installation on the wrapped backend.
    /// @param handler Callable invoked after every successful connect; `nullptr` clears.
    void setConnectHandler(const std::function<void()>& handler) override {
        post([handler](detail::IBackend& inner) { inner.setConnectHandler(handler); });
    }

    /// @brief Queues the installation on the wrapped backend.
    /// @param handler Callable invoked whenever the transport drops; `nullptr` clears.
    void setDisconnectHandler(const std::function<void()>& handler) override {
        post([handler](detail::IBackend& inner) { inner.setDisconnectHandler(handler); });
    }

    /// @brief Queues the session on the wrapped backend.
    /// @param session Session stamped onto every subsequently built control envelope.
    void setSession(::morph::session::Context session) override {
        post(
            [session = std::move(session)](detail::IBackend& inner) mutable { inner.setSession(std::move(session)); });
    }

private:
    /// @brief One queued bind: its promise and its cancellation flag.
    ///
    /// The strand task holds the only `shared_ptr` to this record; `_pending`
    /// holds `weak_ptr`s, so an entry expires by itself when the task is
    /// destroyed.
    struct PendingControl {
        /// @brief Takes ownership of the dispatched call's promise.
        /// @param dispatched Producer side of the `Completion` handed to the caller.
        explicit PendingControl(BindPromise dispatched) : promise{std::move(dispatched)} {}

        /// @brief Producer side of the completion this call settles, from the
        ///        strand or from `cancelPending`; `CompletionState` settles once.
        BindPromise promise;

        /// @brief Set by `cancelPending` before it rejects; read by the task
        ///        before it runs the bind.
        std::atomic_bool cancelled{false};
    };

    /// @brief Checks, in a debug build, that the caller is on the owner the
    ///        adapter was given. Nothing to check before `setOwner`.
    /// @param site Name of the calling body.
    void note(char const* site) const noexcept {
        if (_affinity) {
            _affinity->note(site);
        }
    }

    /// @brief Queues @p task, run with the wrapped backend on the strand.
    /// @tparam F Callable taking `detail::IBackend&`.
    /// @param task What to run.
    template <typename F>
    void post(F task) {
        _control.post(kControlStrand, [inner = _inner, task = std::move(task)]() mutable { task(*inner); });
    }

    /// @brief Runs @p task with the wrapped backend on the strand and waits for
    ///        its result: inline when already on the strand.
    /// @tparam F Callable taking `detail::IBackend&` and returning a value.
    /// @param task What to run.
    /// @return What @p task returned; rethrows what it threw.
    template <typename F>
    std::invoke_result_t<F&, detail::IBackend&> runAndWait(F task) {
        using R = std::invoke_result_t<F&, detail::IBackend&>;
        if (_control.runningHere(kControlStrand)) {
            return task(*_inner);
        }
        auto done = std::make_shared<std::promise<R>>();
        auto result = done->get_future();
        post([done, task = std::move(task)](detail::IBackend& inner) mutable {
            try {
                done->set_value(task(inner));
            } catch (...) {
                done->set_exception(std::current_exception());
            }
        });
        return result.get();
    }

    /// @brief Queues @p op on the strand and settles a `Completion` with the
    ///        outcome of the `Completion` it returns.
    ///
    /// The wrapped backend's answer is delivered on @p cbExec too, and is
    /// attached to there, by a task posted from the strand: its owner is the
    /// caller's, which runs one task at a time, while the strand is not an
    /// executor that answer could be delivered on.
    /// @tparam Op    Callable taking `detail::IBackend&` and the executor its
    ///               `Completion<ModelId>` is to be delivered on, and returning it.
    /// @param cbExec Executor the continuation is delivered on.
    /// @param op     The call to run on the strand.
    /// @return A `Completion` settled by @p op's outcome.
    template <typename Op>
    ::morph::async::Completion<::morph::exec::detail::ModelId> dispatch(::morph::exec::IExecutor& cbExec, Op op) {
        using Settled = ::morph::async::Completion<::morph::exec::detail::ModelId>;
        auto [completion, promise] = Settled::makeSettleable(&cbExec);
        auto pending = std::make_shared<PendingControl>(std::move(promise));
        trackPending(pending);
        post([pending, op = std::move(op), replyExec = &cbExec](detail::IBackend& inner) mutable {
            if (pending->cancelled.load(std::memory_order_acquire)) {
                return;
            }
            try {
                auto answer = std::make_shared<Settled>(op(inner, *replyExec));
                replyExec->post([pending, answer] {
                    answer->then([pending](::morph::exec::detail::ModelId mid) { pending->promise.resolve(mid); })
                        .onError([pending](const std::exception_ptr& failure) { pending->promise.reject(failure); });
                });
            } catch (...) {
                pending->promise.reject(std::current_exception());
            }
        });
        return std::move(completion);
    }

    /// @brief Records @p pending as cancellable until its task settles it.
    ///
    /// Swept on the same amortised schedule as `LocalBackend::trackPending`:
    /// dead entries are reclaimed only when the list reaches `_compactAt`,
    /// which each sweep re-arms at twice the surviving count.
    /// @param pending Record to cancel and reject if `cancelPending` runs before
    ///                 its task settles it.
    void trackPending(const std::shared_ptr<PendingControl>& pending) {
        if (_pending.size() >= _compactAt) {
            std::erase_if(_pending, [](const auto& weak) { return weak.expired(); });
            _compactAt = std::max(kPendingCompactFloor, _pending.size() * 2);
        }
        _pending.emplace_back(pending);
    }

    /// @brief The single strand key every call shares, so they run one at a
    ///        time. Not a real model id: these strands are private to the
    ///        adapter.
    static constexpr ::morph::exec::detail::ModelId kControlStrand{1};

    /// @brief Smallest size at which `trackPending` sweeps; see `LocalBackend`'s.
    static constexpr std::size_t kPendingCompactFloor = 32;

    std::shared_ptr<detail::IBackend> _inner;
    ::morph::exec::detail::ModelStrands _control;
    std::optional<::morph::exec::detail::OwnerAffinity> _affinity;
    // Every bind handed to a `_control` task and not yet settled by it. Weak,
    // so a settled task's record drops out on its own. Touched only on the
    // caller's owner: `bindModel`/`promoteModel` add, `cancelPending` takes.
    std::vector<std::weak_ptr<PendingControl>> _pending;
    // Size at which `trackPending` next sweeps `_pending`.
    std::size_t _compactAt = kPendingCompactFloor;
};

/// @brief In-process backend that executes model actions on a thread pool strand.
///
/// Each model instance gets its own strand so actions are serialised per-model
/// without a global lock on the pool.
///
/// Its registry, its pending list and its record of Task runs belong to the
/// caller's owner — the `Bridge`'s, given through `setOwner` — and are touched
/// only there, without a lock. Only the strand tasks it posts run elsewhere,
/// and they reach nothing of the backend but what each carries.
class LocalBackend : public detail::IBackend {
public:
    /// @brief Constructs the backend using @p workerPool to run model actions.
    /// @param workerPool Executor (typically a `ThreadPoolExecutor`) for model
    ///                   work. Borrowed, not owned: this backend's strands run
    ///                   on it, so it must outlive the backend *and* keep
    ///                   running tasks until teardown completes — destroying it
    ///                   first deadlocks (see
    ///                   `docs/spec/concurrency_and_lifetimes.md`, "Destruction
    ///                   ordering").
    explicit LocalBackend(::morph::exec::IExecutor& workerPool MORPH_LIFETIMEBOUND)
        : _strands{std::make_shared<::morph::exec::detail::ModelStrands>(workerPool)} {}

    /// @brief Stops the Task handlers still running, lets the strands drain,
    ///        seals them, drains them again, and only then closes them
    ///        (`ModelStrands::teardown`). See `docs/spec/core/coroutines.md`,
    ///        "Teardown".
    ///
    /// Where threads exist, in that order:
    /// 1. Every live Task run's stop is requested. A handler suspended in an
    ///    awaitable that resumes on the current executor -- morph's own,
    ///    `core::async::AsyncQueue::pop` -- resumes, cancelled, through its
    ///    strand, which still admits it. One suspended on a `core::net` socket
    ///    or timer resumes on that loop instead, and unwinds there; its end is
    ///    posted to the strand.
    /// 2. The strands are drained: the resumptions and ends queued on them,
    ///    and every queued action -- skipped, if `cancelPending` already failed
    ///    it. This drain comes before the seal because a handler's end that
    ///    arrives from a loop thread now is still queued behind its instance's
    ///    other tasks; sealed, it would run inline on the loop thread while
    ///    this drain ran one of those tasks on a pool thread, two threads
    ///    inside the instance's action gate.
    /// 3. The strands are sealed: from here on a resumption or a handler's end
    ///    is refused and runs inline, where it arrives, rather than queued on a
    ///    strand step 5 would drop.
    /// 4. The strands are drained again, for what reached them between steps
    ///    2 and 3. It does not wait for a handler still unwinding on another
    ///    executor.
    /// 5. The strands are closed.
    ///
    /// Must not run on one of this backend's strand threads, whose drain it
    /// would wait for; a debug build asserts that. The single-threaded
    /// WebAssembly build seals, stops the handlers, then closes: sealed first,
    /// each stopped handler unwinds inline in the stop, and the drains wait for
    /// nothing, since nothing else could run the strands.
    ~LocalBackend() override {
        note("LocalBackend::~LocalBackend");
        auto runs = std::exchange(_taskRuns, {});
        _strands->teardown([&runs] {
            for (auto const& weak : runs) {
                if (auto const run = weak.lock()) {
                    run->stopSource->request_stop();
                }
            }
        });
    }

    LocalBackend(const LocalBackend&) = delete;
    LocalBackend& operator=(const LocalBackend&) = delete;
    LocalBackend(LocalBackend&&) = delete;
    LocalBackend& operator=(LocalBackend&&) = delete;

    /// @brief Creates a model instance via @p factory and registers it.
    ///
    /// The `typeId` parameter is accepted for interface compatibility but not
    /// used — it is unnamed in the signature below, and the concrete type is
    /// captured by the factory closure. If the new holder's
    /// `isBackendChangeAware()` returns `true`, the new id (a local in
    /// `createAndTrack`, not a parameter here) is also recorded in
    /// `_changeAware` so `notifyBackendChanged()` finds it without a
    /// `dynamic_cast` sweep.
    /// @param factory  Callable that constructs the `IModelHolder`.
    /// @return Newly assigned `ModelId`.
    ::morph::exec::detail::ModelId registerModel(
        const std::string& /*typeId*/,
        std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory) override {
        note("LocalBackend::registerModel");
        ::morph::observe::detail::emitMetric(::morph::observe::Metric::registerCount, 1.0);
        return createAndTrack(std::move(factory));
    }

    /// @brief Acquires an instance for @p request and settles the returned
    ///        `Completion` before returning.
    ///
    /// Every shape of `BindRequest` is answered here, against this backend's
    /// own directory: an empty `primary` creates a private instance; a
    /// non-empty one registers-or-attaches the shared instance for
    /// `(typeId, primary)`, lazily evicting one whose first action failed
    /// (`InstanceDirectory::attach`); a non-zero `current` is released once
    /// the replacement is acquired, so a same-key re-attach keeps the instance
    /// it already holds and a throwing acquire never strands the caller.
    /// @param request Owning bind request; moved from.
    /// @param cbExec  Executor the continuation is delivered on. Borrowed: it
    ///                must outlive the returned `Completion`.
    /// @return A settled `Completion` carrying the bound id or the failure.
    ::morph::async::Completion<::morph::exec::detail::ModelId> bindModel(detail::BindRequest request,
                                                                         ::morph::exec::IExecutor& cbExec) override {
        note("LocalBackend::bindModel");
        auto [completion, promise] =
            ::morph::async::Completion<::morph::exec::detail::ModelId>::makeSettleable(&cbExec);
        try {
            promise.resolve(bindNow(std::move(request)));
        } catch (...) {
            promise.reject(std::current_exception());
        }
        return std::move(completion);
    }

    /// @brief Records the owner every verb below is called from.
    /// @param owner The caller's owner.
    void setOwner(const ::morph::exec::detail::OwnerAffinity& owner) override { _affinity.emplace(owner); }

    /// @brief Enters an already-live, still-anonymous instance into the
    ///        directory under @p primary.
    /// @param mid     Live instance to promote.
    /// @param typeId  Model type id — the directory's first key component.
    /// @param primary Canonical string encoding of the key to file it under.
    void assignPrimary(::morph::exec::detail::ModelId mid, const std::string& typeId,
                       std::string_view primary) override {
        note("LocalBackend::assignPrimary");
        if (primary.empty()) {
            return;
        }
        // A no-op unless `mid` is live, has never held a directory key, and the
        // key is free — `InstanceDirectory::promote` holds all the guards and
        // the reasons for them.
        (void)_instances.promote(mid, detail::DirectoryKey{typeId, std::string{primary}});
    }

    /// @brief Lists the primary keys of live shared instances of @p typeId.
    /// @param typeId String type-id to enumerate.
    /// @return Canonical key strings of the live shared instances, in unspecified order.
    std::vector<std::string> listInstances(const std::string& typeId) override {
        note("LocalBackend::listInstances");
        return _instances.keysOfType(typeId);
    }

    /// @brief Removes the model with @p mid, or releases one attachment to it.
    ///
    /// A private instance is erased outright. A shared instance has its attach
    /// count decremented and is erased — and removed from the directory — only
    /// when that count reaches zero, so releasing one handler never destroys an
    /// instance another handler still holds.
    /// @param mid Id returned by a prior `registerModel()`/`bindModel()` call.
    void deregisterModel(::morph::exec::detail::ModelId mid) override {
        note("LocalBackend::deregisterModel");
        ::morph::observe::detail::emitMetric(::morph::observe::Metric::deregisterCount, 1.0);
        // One lookup, not five: the attach count, the directory key and the
        // holder are one record, so releasing the last reference unfiles and
        // destroys it in a single step that cannot half-happen.
        if (_instances.release(mid) == detail::InstanceDirectory::Release::destroyed) {
            _changeAware.erase(mid);
        }
    }

    /// @brief Schedules `onBackendChanged()` on each change-aware model's strand.
    ///
    /// Only models recorded in `_changeAware` — maintained by `createAndTrack`/
    /// `deregisterModel` from `IModelHolder::isBackendChangeAware()`, a
    /// compile-time answer per model type — are visited; there is no
    /// `dynamic_cast` and no scan of models that never opted in. Each such
    /// model's `onBackendChanged()` (the `IModelHolder` base virtual) is
    /// **posted onto that model's own strand** (the same per-`ModelId` serial
    /// queue `execute` uses), rather than invoked inline on the caller's thread.
    /// Two properties follow, and they are the whole point:
    ///
    /// - **Strand-serialised, lock-free model code.** `onBackendChanged()` runs
    ///   on the pool inside the model's strand, so it never overlaps an
    ///   `execute()` on the same model. A model reacting to a backend switch
    ///   (e.g. draining an offline queue and mutating counters) needs no locking
    ///   of its own state — exactly what `offline.md` promises.
    /// - **Off the owner.** The model body runs later on a pool thread, never
    ///   inside `Bridge::switchBackend`. A model that wants to reach the bridge
    ///   from `onBackendChanged()` posts to the bridge's owner.
    ///
    /// A `shared_ptr` copy of each holder is captured into the posted task so a
    /// concurrent `deregisterModel` cannot free the model out from under its
    /// pending notification (mirrors `execute`'s holder capture).
    void notifyBackendChanged() override {
        note("LocalBackend::notifyBackendChanged");
        for (auto modelId : _changeAware) {
            if (const auto* inst = _instances.find(modelId)) {
                _strands->post(modelId, [held = inst->holder]() mutable { held->onBackendChanged(); });
            }
        }
    }

    /// @brief Schedules `call.localOp` on the model's strand and returns a `Completion`.
    ///
    /// The completion resolves with the opaque result on the strand thread and
    /// the callbacks are delivered via @p cbExec.
    ///
    /// Expressed in terms of `executeInto` rather than beside it, so there is
    /// one definition of what a local dispatch does whichever entry point a
    /// caller uses. The extra `CompletionState` and its adapter sink are the
    /// price of the `Completion`-returning shape — which is exactly the cost
    /// `executeInto` exists to let `Bridge` avoid.
    ///
    /// @par Why this is `final`
    /// `Bridge::executeVia` calls `executeInto`, not `execute`. A subclass that
    /// overrode `execute` alone would therefore be bypassed for every bridge
    /// dispatch and intercept only the handful of direct `execute` callers —
    /// silently, with every test it has still passing on the paths it does
    /// reach. `final` turns that into a compile error naming the fix: **derive
    /// from `LocalBackend` and override `executeInto`**, which is the primitive
    /// both entry points now share. (A backend deriving straight from
    /// `IBackend` is unaffected: it overrides `execute` and inherits the
    /// default `executeInto`, which forwards to it.)
    ///
    /// @param mid    Target model id.
    /// @param call   Bundled action; `localOp` is the only field used here.
    /// @param cbExec Executor for delivering callbacks.
    /// @return Completion that will carry the result or an exception.
    ::morph::async::Completion<std::shared_ptr<void>> execute(::morph::exec::detail::ModelId mid,
                                                              detail::ActionCall call,
                                                              ::morph::exec::IExecutor* cbExec) final {
        auto compState = std::make_shared<::morph::async::detail::CompletionState<std::shared_ptr<void>>>();
        ::morph::async::Completion<std::shared_ptr<void>> comp{compState, cbExec};
        executeInto(mid, std::move(call), cbExec,
                    std::make_shared<::morph::async::detail::CompletionSettleSink>(compState));
        return comp;
    }

    /// @brief Schedules `call.localOp` on the model's strand and settles @p sink.
    ///
    /// @param mid    Target model id.
    /// @param call   Bundled action; `localOp` is the only field used here.
    /// @param cbExec Unused here: a sink carries its own delivery. Accepted so
    ///               the signature matches `IBackend::executeInto`, whose remote
    ///               implementations do need it.
    /// @param sink   Settled on the strand thread with the result or the
    ///               exception, exactly once.
    void executeInto(::morph::exec::detail::ModelId mid, detail::ActionCall call, ::morph::exec::IExecutor* cbExec,
                     std::shared_ptr<::morph::async::detail::ISettleSink> sink) override {
        (void)cbExec;
        MORPH_ZONE("LocalBackend::executeInto");
        MORPH_ZONE_TEXT(call.session.requestId);
        note("LocalBackend::executeInto");

        std::shared_ptr<::morph::model::detail::IModelHolder> holder;
        std::shared_ptr<detail::HydrationState> hydration;
        // One lookup for the holder and its hydration state together: they
        // are fields of one record, not entries in two maps kept in lockstep
        // by convention.
        if (const auto* inst = _instances.find(mid)) {
            holder = inst->holder;
            hydration = inst->hydration;
        }
        if (!holder) {
            sink->settleException(
                std::make_exception_ptr(std::runtime_error("model not found: id=" + std::to_string(mid.v))));
            return;
        }
        auto const admittedEpoch = trackPending(sink);
        LocalRun run;
        run.localOp = call.localOp;
        run.localOpAsync = call.localOpAsync;
        run.stopSource = std::move(call.stopSource);
        // The action handle travels with `localOp` into the strand task, not
        // just as far as this function: `ActionCall::localOp` borrows the
        // action rather than owning it (see that struct), and `call` is gone
        // long before the task runs.
        run.action = std::move(call.action);
        run.session = std::move(call.session);
        run.modelTypeId = call.modelTypeId;
        run.actionTypeId = call.actionTypeId;
        run.holder = std::move(holder);
        run.sink = std::move(sink);
        run.hydration = std::move(hydration);
        run.strands = _strands;
        run.cancels = _cancels;
        run.admittedEpoch = admittedEpoch;
        run.mid = mid;
        // Held by shared_ptr, never by raw `this`. A shared_ptr copy has its
        // own lifetime, independent of LocalBackend's, so it stays valid even
        // if the backend is torn down while this task is still queued or
        // running. `hydration` follows the same rule and may be null (a
        // private instance has no entry).
        run.inFlightCounter = _inFlight;
        auto const inFlightAfterInc = run.inFlightCounter->fetch_add(1, std::memory_order_relaxed) + 1;
        ::morph::observe::detail::emitMetric(::morph::observe::Metric::executeInFlight,
                                             static_cast<double>(inFlightAfterInc));
        // Through the instance's action gate: an action starts only once the one
        // before it has finished, which a Task handler does when its Task
        // completes rather than when the strand task that started it returns.
        if (run.localOpAsync != nullptr) {
            // A Task run is shared: its completion callback outlives the strand
            // task, and every Task run can be stopped, deadline or not -- the
            // destructor stops the ones still running.
            if (!run.stopSource) {
                run.stopSource = std::make_shared<::core::async::StopSource>();
            }
            auto shared = std::make_shared<LocalRun>(std::move(run));
            rememberTaskRun(shared);
            _strands->post(mid,
                           [shared] { shared->holder->actionGate().enter([shared] { startTaskLocal(shared); }); });
            return;
        }
        // An ordinary run travels by value in the strand task, so a dispatch
        // costs the post's one allocation and nothing more:
        // `bench.alloc_budget` holds that line. The task keeps it while the
        // handler runs, as it kept its captures before there was a gate; only
        // a run that has to wait behind a suspended Task handler is moved out,
        // into the gate's queue.
        _strands->post(mid, [run = std::move(run)]() mutable {
            auto& gate = run.holder->actionGate();
            if (gate.tryEnter()) {
                startLocal(run);
                return;
            }
            gate.enter([waiting = std::make_shared<LocalRun>(std::move(run))] { startLocal(*waiting); });
        });
    }

    /// @brief Resolves every still-pending completion this backend produced
    ///        with @p exc, and requests stop on every Task handler still running.
    ///
    /// A run still waiting for its instance's action gate is failed where it
    /// waits (see `admitLocal`); a Task handler already running observes the
    /// stop on its token at its next `co_await`.
    /// @param exc Exception delivered to every pending completion's error sink.
    void cancelPending(const std::exception_ptr& exc) override {
        note("LocalBackend::cancelPending");
        auto snapshot = std::exchange(_pending, {});
        _compactAt = kPendingCompactFloor;
        // With the swap: a run admitted before this point is in `snapshot`
        // and is failed below, and one admitted after it is not.
        _cancels->record(exc);
        // Settle before stopping. A stopped Task handler resumes on its strand
        // and settles its own sink with `OperationCancelled`; requested first,
        // that settle can land before this one, and the caller would be
        // answered with the stop instead of @p exc. Settled first, the
        // handler's later settle is the ignored one.
        for (auto& weak : snapshot) {
            if (auto sink = weak.lock()) {
                // May race a reply settling the same sink; `ISettleSink`'s
                // contract makes the second call a no-op, which is what
                // `CompletionState`'s first-result-wins gave this loop before
                // the sink existed.
                sink->settleException(exc);
            }
        }
        for (auto const& weak : _taskRuns) {
            if (auto const run = weak.lock()) {
                static_cast<void>(run->stopSource->request_stop());
            }
        }
    }

    /// @brief Number of entries currently held in the pending list.
    ///
    /// **Not** the number of calls in flight: the list is compacted amortised
    /// (see `trackPending`), so it also carries entries whose completion has
    /// already been destroyed and which `cancelPending` would skip. It is an
    /// upper bound on the in-flight count and a direct measure of what the
    /// compaction policy costs in memory — which is what it exists to make
    /// observable. For a count of in-flight *calls*, use `Bridge::pendingCalls()`.
    /// @return Size of the pending list, live and dead entries alike.
    [[nodiscard]] std::size_t trackedPendingCount() const {
        note("LocalBackend::trackedPendingCount");
        return _pending.size();
    }

private:
    /// @brief Checks, in a debug build, that the caller is on the owner the
    ///        backend was given. Nothing to check before `setOwner`.
    /// @param site Name of the calling body.
    void note(char const* site) const noexcept {
        if (_affinity) {
            _affinity->note(site);
        }
    }

    /// @brief `bindModel`'s body: acquires the instance @p request names.
    /// @param request Owning bind request; moved from.
    /// @return The bound id.
    ::morph::exec::detail::ModelId bindNow(detail::BindRequest request) {
        ::morph::observe::detail::emitMetric(::morph::observe::Metric::registerCount, 1.0);
        ::morph::exec::detail::ModelId next{};
        if (request.primary.empty()) {
            next = createAndTrack(std::move(request.factory));
        } else {
            detail::DirectoryKey dirKey{request.typeId, request.primary};
            if (auto const attached = _instances.attach(dirKey)) {
                next = *attached;
            } else {
                auto [mid, holder] = createHolder(std::move(request.factory));
                _instances.insertShared(mid, std::move(holder), std::move(dirKey));
                next = mid;
            }
        }
        // After the acquire: a same-key re-attach took a second reference to
        // `current`, and this releases the redundant one.
        if (request.current.v != 0U) {
            deregisterModel(request.current);
        }
        return next;
    }

    /// @brief Builds a holder via @p factory, records it under a fresh id, and
    ///        returns that id.
    ///
    /// Shared by `registerModel` and `bindModel`'s private path.
    /// @param factory Callable that constructs the `IModelHolder`.
    /// @return Newly assigned `ModelId`.
    ::morph::exec::detail::ModelId createAndTrack(
        std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory) {
        auto [mid, holder] = createHolder(std::move(factory));
        _instances.insertPrivate(mid, std::move(holder));
        return mid;
    }

    /// @brief Allocates an id, builds the holder and notes change-awareness,
    ///        without filing it anywhere.
    ///
    /// The half the private and the shared paths have in common; they differ
    /// only in how the result is filed, which is `InstanceDirectory`'s job.
    /// @param factory Callable that constructs the `IModelHolder`.
    /// @return The newly assigned id and the constructed holder.
    std::pair<::morph::exec::detail::ModelId, std::shared_ptr<::morph::model::detail::IModelHolder>> createHolder(
        std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory) {
        ::morph::exec::detail::ModelId const mid{_nextId.fetch_add(1) + 1};
        std::shared_ptr<::morph::model::detail::IModelHolder> holder = factory();
        if (holder->isBackendChangeAware()) {
            _changeAware.insert(mid);
        }
        return {mid, std::move(holder)};
    }

    /// @brief Records @p state in the pending list, compacting it amortised-O(1).
    ///
    /// The list is append-only between compactions; dead entries are swept only
    /// when its size reaches `_compactAt`, which each sweep re-arms at twice the
    /// number of entries that survived it. Doubling the threshold off the live
    /// count is what makes the sweep amortised: a sweep costs O(size), and at
    /// least `_compactAt / 2` appends must happen before the next one, so the
    /// per-append cost is O(1) however deep the queue gets. The list is
    /// correspondingly bounded at twice the live count (plus the floor), which
    /// is the whole price of dropping the per-dispatch scan.
    ///
    /// Sweeping on *every* append instead would cost `n` atomic
    /// `weak_ptr::expired()` loads to admit one call with
    /// `n` in flight, so a burst of `n` costs O(n²) — measured at 362ms of pure
    /// admission time for 32k queued executes against one slow model, against
    /// 7.6ms without the sweep.
    ///
    /// Purely a cost change: `cancelPending` still sees every live state,
    /// because a dead entry is one whose `CompletionState` is already gone and
    /// which `cancelPending`'s `weak.lock()` has always skipped. Carrying dead
    /// entries for longer changes nothing it observes.
    /// @param sink Settle sink to track until it expires or is cancelled.
    /// @return The cancel epoch @p sink was admitted in: `cancelPending` has
    ///         failed the dispatch once the epoch has moved on.
    std::uint64_t trackPending(const std::shared_ptr<::morph::async::detail::ISettleSink>& sink) {
        if (_pending.size() >= _compactAt) {
            std::erase_if(_pending, [](const auto& weak) { return weak.expired(); });
            _compactAt = std::max(kPendingCompactFloor, _pending.size() * 2);
        }
        _pending.emplace_back(sink);
        return _cancels->epoch();
    }

    /// What `cancelPending` has done so far, shared with every run: how many
    /// times it has run, and with what reason the last time.
    ///
    /// Written only by `cancelPending`, on the owner; read by runs on their
    /// strands. Each reason is an immutable node published with a release
    /// store before the epoch moves, so a run that sees the epoch move reads a
    /// complete reason. Nodes live as long as the record: one per
    /// `cancelPending`, which runs on a switch, a teardown or a disconnect.
    class CancelRecord {
    public:
        /// Publishes @p reason, then bumps the epoch. One writer: the owner.
        void record(const std::exception_ptr& reason) {
            auto node = std::make_unique<Node>(Node{.reason = reason, .previous = std::move(_chain)});
            _latest.store(node.get(), std::memory_order_release);
            _chain = std::move(node);
            _epoch.fetch_add(1);
        }

        /// @return How many times `cancelPending` has run.
        [[nodiscard]] std::uint64_t epoch() const noexcept { return _epoch.load(); }

        /// @return The reason the last `cancelPending` gave; null before the first.
        [[nodiscard]] std::exception_ptr lastReason() const {
            Node const* const node = _latest.load(std::memory_order_acquire);
            return node == nullptr ? std::exception_ptr{} : node->reason;
        }

    private:
        struct Node {
            std::exception_ptr reason;
            std::unique_ptr<Node> previous;
        };
        std::atomic<std::uint64_t> _epoch{0};
        std::atomic<Node const*> _latest{nullptr};
        std::unique_ptr<Node> _chain;
    };

    /// Everything one local dispatch carries from `executeInto` to the moment
    /// it settles. An ordinary handler's run is held by its strand task, or by
    /// the gate's queue while it waits there; a Task handler's is shared,
    /// because its completion callback holds it too.
    struct LocalRun {
        std::shared_ptr<void> (*localOp)(::morph::model::detail::IModelHolder&, void*) = nullptr;
        decltype(detail::ActionCall::localOpAsync) localOpAsync = nullptr;
        std::shared_ptr<::core::async::StopSource> stopSource;
        std::shared_ptr<void> action;
        ::morph::session::Context session;
        std::string_view modelTypeId;
        std::string_view actionTypeId;
        std::shared_ptr<::morph::model::detail::IModelHolder> holder;
        std::shared_ptr<::morph::async::detail::ISettleSink> sink;
        std::shared_ptr<detail::HydrationState> hydration;
        std::shared_ptr<std::atomic<std::size_t>> inFlightCounter;
        std::shared_ptr<::morph::exec::detail::ModelStrands> strands;
        std::shared_ptr<CancelRecord> cancels;
        std::uint64_t admittedEpoch = 0;
        ::morph::exec::detail::ModelId mid{};
        std::chrono::steady_clock::time_point start;
        ::morph::observe::SpanId spanId{};
    };

    /// Stamps a dispatch's start once it holds its instance's action gate, and
    /// settles it instead if `cancelPending` failed it while it waited there.
    /// @return Whether the handler is to run.
    static bool admitLocal(LocalRun& run) {
        run.start = std::chrono::steady_clock::now();
        run.spanId = ::morph::observe::detail::beginSpan(run.session.requestId, run.modelTypeId, run.actionTypeId);
        if (run.cancels->epoch() == run.admittedEpoch) {
            return true;
        }
        // `cancelPending` failed this call while it waited for the gate;
        // the handler does not run for a caller that has been answered.
        // The sink is settled here, with the reason `cancelPending` gave,
        // because this may be the only place it can be: `cancelPending`
        // reaches sinks through `weak_ptr`s, and when the caller has
        // dropped its `Completion` this run holds the last reference, so
        // the sink is gone by the time `cancelPending` would reach it.
        // Where `cancelPending` got there first, this settle is ignored.
        finishLocal(run, nullptr, run.cancels->lastReason());
        return false;
    }

    /// Runs an ordinary handler's dispatch once it holds its instance's action
    /// gate, on the strand, and finishes it.
    static void startLocal(LocalRun& run) {
        MORPH_ZONE("LocalBackend::startLocal");
        MORPH_ZONE_TEXT(run.session.requestId);
        if (!admitLocal(run)) {
            return;
        }
        std::shared_ptr<void> value;
        std::exception_ptr error;
        try {
            ::morph::session::detail::ScopedContext const scoped{run.session};
            // Explicit, because `localOp` is a function pointer now and
            // a null one is undefined behaviour rather than the
            // `std::bad_function_call` an empty `std::function` used to
            // raise. Same outcome for the caller -- the completion
            // resolves through its error sink -- with a diagnostic that
            // names the field instead of the library.
            if (run.localOp == nullptr) {
                throw std::runtime_error{"ActionCall::localOp is null: nothing to execute"};
            }
            value = run.localOp(*run.holder, run.action.get());
        } catch (...) {
            error = std::current_exception();
        }
        finishLocal(run, std::move(value), error);
    }

    /// Starts a Task handler once its dispatch holds the instance's action
    /// gate, on the strand. It finishes when its Task completes, through the
    /// callback it is handed.
    static void startTaskLocal(const std::shared_ptr<LocalRun>& run) {
        MORPH_ZONE("LocalBackend::startTaskLocal");
        MORPH_ZONE_TEXT(run->session.requestId);
        if (!admitLocal(*run)) {
            return;
        }
        std::shared_ptr<::morph::exec::detail::TaskResumer> executor;
        try {
            ::morph::session::detail::ScopedContext const scoped{run->session};
            executor = std::make_shared<::morph::exec::detail::TaskResumer>(run->strands, run->mid, run->session);
            run->strands->enroll(run->mid, executor);
            auto token = run->stopSource->get_token();
            run->localOpAsync(*run->holder, run->action, executor, std::move(token),
                              [run, resumer = executor.get()](std::shared_ptr<void> value, std::exception_ptr error) {
                                  run->strands->withdraw(run->mid, resumer);
                                  // A handler whose last await resumed on
                                  // another executor -- a `core::net` loop --
                                  // ends there; what follows its end belongs
                                  // on the strand.
                                  run->strands->runOnStrand(run->mid,
                                                            [run, value = std::move(value), error = std::move(error)] {
                                                                finishLocal(*run, value, error);
                                                            });
                              });
        } catch (...) {
            run->strands->withdraw(run->mid, executor.get());
            finishLocal(*run, nullptr, std::current_exception());
        }
    }

    /// Records a finished dispatch and settles its sink, then leaves the action
    /// gate so the next action on the instance can start. On the strand.
    static void finishLocal(LocalRun& run, std::shared_ptr<void> value, const std::exception_ptr& error) {
        MORPH_ZONE("LocalBackend::finishLocal");
        MORPH_ZONE_TEXT(run.session.requestId);
        bool const succeeded = error == nullptr;
        // Resolve the sink only after every metric and `endSpan` below are
        // recorded — nothing synchronizes a `.then()`/`.onError()` callback
        // (delivered via `cbExec`, which may run inline/synchronously) with
        // anything after `setValue`/`setException` returns, so resolving first
        // would let the caller observe completion before these metrics are
        // emitted. This is a real race, not just a theoretical one.
        //
        // Settle hydration the moment the first action's outcome is known —
        // before `endSpan`, before any metric, and before the `Completion`
        // resolves. Each of those hands control to host code that is free
        // to attach to this instance's key, and an attacher reaching the
        // directory while the outcome is known but unrecorded is handed an
        // instance whose first action has already failed — exactly what
        // docs/spec/core/shared_instances.md's Failure modes section says
        // must not happen. Only the *first* action settles it; `settle` is
        // a single compare-exchange and ignores every later call.
        if (run.hydration) {
            run.hydration->settle(succeeded);
        }
        ::morph::observe::detail::endSpan(run.spanId, succeeded);
        auto const elapsedMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - run.start).count();
        std::array<std::pair<std::string_view, std::string_view>, 2> const tags{
            {{"modelType", run.modelTypeId}, {"actionType", run.actionTypeId}}};
        ::morph::observe::detail::emitMetric(::morph::observe::Metric::executeLatencyMs, elapsedMs, tags);
        if (!succeeded) {
            ::morph::observe::detail::emitMetric(::morph::observe::Metric::executeErrors, 1.0, tags);
        }
        auto const inFlightAfterDec = run.inFlightCounter->fetch_sub(1, std::memory_order_relaxed) - 1;
        ::morph::observe::detail::emitMetric(::morph::observe::Metric::executeInFlight,
                                             static_cast<double>(inFlightAfterDec));
        // Resolve last: the sink is still settled exactly once, only its
        // position relative to the now-recorded instrumentation moved.
        if (succeeded) {
            run.sink->settleValue(std::move(value));
        } else {
            run.sink->settleException(error);
        }
        run.holder->actionGate().leave();
    }

    /// Records @p run among the Task runs the destructor stops, sweeping the
    /// ones that have finished amortised, as `trackPending` does.
    void rememberTaskRun(const std::shared_ptr<LocalRun>& run) {
        if (_taskRuns.size() >= _taskRunsCompactAt) {
            std::erase_if(_taskRuns, [](const auto& weak) { return weak.expired(); });
            _taskRunsCompactAt = std::max(kPendingCompactFloor, _taskRuns.size() * 2);
        }
        _taskRuns.emplace_back(run);
    }

    // One strand per model instance. Shared with the Task handlers started
    // here, whose resumers outlive the backend; closed by the destructor.
    std::shared_ptr<::morph::exec::detail::ModelStrands> _strands;
    // The owner every verb is called from, once `setOwner` has named it.
    std::optional<::morph::exec::detail::OwnerAffinity> _affinity;
    // Every live instance, private and shared alike, plus the shared-instance
    // directory over them — holder, attach count, directory key and hydration
    // state as one record per instance rather than five parallel ModelId-keyed
    // maps held in lockstep by convention. Owner-only.
    detail::InstanceDirectory _instances;
    // Ids of models whose holder answered `isBackendChangeAware() == true` at
    // registration time. An index over `_instances`, maintained on the owner
    // (inserted in `createHolder`, erased in `deregisterModel` when the instance
    // is actually destroyed) so `notifyBackendChanged()` never needs to inspect
    // a model it doesn't have to. Always a subset of `_instances`' keys.
    //
    // Not folded into `InstanceDirectory`: backend-change awareness is a
    // LocalBackend-only concern with no `RemoteServer` counterpart, and the
    // directory is the state the two backends genuinely share.
    std::unordered_set<::morph::exec::detail::ModelId, ::morph::exec::detail::ModelIdHash> _changeAware;
    std::atomic<uint64_t> _nextId{0};
    // Smallest size at which `trackPending` will sweep. Below it the sweep costs
    // more than the handful of dead `weak_ptr`s it could reclaim, and a backend
    // that only ever has a few calls in flight never sweeps at all.
    static constexpr std::size_t kPendingCompactFloor = 32;
    // Sinks, not completion states: a dispatch's settle point is whatever the
    // caller handed down, which for `Bridge` is its own typed completion state.
    // A `weak_ptr`, so this list cannot keep a finished dispatch alive.
    std::vector<std::weak_ptr<::morph::async::detail::ISettleSink>> _pending;
    // Size at which `trackPending` next sweeps `_pending` for expired entries;
    // re-armed at twice the surviving count after each sweep. See `trackPending`.
    std::size_t _compactAt = kPendingCompactFloor;
    // Recorded by `cancelPending` as it takes the pending list. Shared with every run, which notes the epoch it was
    // admitted under and skips its handler if the epoch has moved on by the time it starts.
    std::shared_ptr<CancelRecord> _cancels = std::make_shared<CancelRecord>();
    // The Task runs `cancelPending` and the destructor stop. Weak, so a
    // finished run is not kept. Owner-only.
    std::vector<std::weak_ptr<LocalRun>> _taskRuns;
    std::size_t _taskRunsCompactAt = kPendingCompactFloor;
    // Concurrent in-flight executes, for the executeInFlight metric. A
    // shared_ptr (not a plain atomic member) so strand tasks hold their own
    // reference instead of capturing `this` — see execute()'s comment.
    std::shared_ptr<std::atomic<std::size_t>> _inFlight = std::make_shared<std::atomic<std::size_t>>(0);
};

}  // namespace morph::backend
