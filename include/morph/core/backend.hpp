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
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../attributes.hpp"
#include "../session/session.hpp"
#include "completion.hpp"
#include "detail/instance_directory.hpp"
#include "model.hpp"
#include "observability.hpp"
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

/// @brief The two identities a model instance can carry, passed together.
///
/// Bundled into one struct rather than passed as two adjacent `string_view`
/// parameters because they are trivially swappable at a call site and mean
/// entirely different things: transposing them would silently file journal
/// entries under the directory key and share instances under the log's entity
/// key. Keeping them named at every call site makes that mistake unwritable.
struct InstanceIdentity {
    /// @brief Entity key for the action log; empty if none. See `journal::LogEntry::entityKey`.
    std::string_view contextKey;

    /// @brief Canonical string encoding of the primary key; empty if the
    ///        instance is anonymous and therefore unshareable.
    std::string_view primary;
};

/// @brief One *bind* request: everything needed to acquire a model instance.
///
/// The request half of the structural registration surface
/// (`IBackend::bindModel`). It replaces the three separate acquire verbs
/// (`registerModelWithContext`, `registerModelShared`, `attachModel`) with one
/// request whose *shape* selects the behaviour, because the three differ only
/// in which fields are populated:
///
/// | `primary` | `current` | Equivalent legacy verb |
/// |---|---|---|
/// | empty | `0` | `registerModelWithContext(typeId, factory, contextKey)` |
/// | non-empty | `0` | `registerModelShared(typeId, factory, identity)` |
/// | non-empty | non-zero | `attachModel(typeId, factory, identity, current)` |
///
/// Unlike `InstanceIdentity`, every string here is **owned**. That is not a
/// style preference: a bind may outlive the frame that issued it, so a
/// `string_view` into the caller's stack is a dangling read waiting for a
/// backend that copies its envelope after the dispatch call returns rather
/// than before it. The synchronous verbs below take views and are safe only
/// because they do not return until the backend has finished with them.
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
    ///        this bind a *re-point*: the backend names what it is re-pointing
    ///        from, exactly as `attachModel` does.
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

/// @brief Whether a caller holding an unsettled `bindModel`/`promoteModel`
///        `Completion` may block its own thread until that `Completion`
///        settles.
///
/// This is the one thing `bindModel`'s signature cannot say, and it has to be
/// said: two shipped backends both return an unsettled `Completion` from
/// `bindModel`, and `Bridge::registerHandlerImpl` — a synchronous entry point
/// that hands its caller a `BridgeHandler` usable on the next line — must wait
/// for one of them and must not wait for the other. From the `Completion`
/// alone the two are indistinguishable.
///
/// It is deliberately **not** a `bool` selecting a verb (see "The structural
/// registration surface" below, point 1). Such a `bool` would make every call
/// site carry two paths and let a backend be half-migrated. This chooses
/// nothing: there is still exactly
/// one verb, called unconditionally, and exactly one continuation. It says
/// only whether the thread that issued the call is allowed to stop and wait
/// for the continuation it already registered.
enum class BindWait : std::uint8_t {
    /// @brief The completion settles without the calling thread's
    ///        participation — inside the call, or on a thread the caller does
    ///        not own. A caller may block until it settles.
    ///
    /// The default, and correct for every backend that has not overridden
    /// `bindModel` (the default settles before it returns) as well as for
    /// `SocketBackend`, whose I/O thread settles the completion. A backend
    /// that returns this **must** settle every `Completion` it hands out
    /// exactly once without further calls from the caller — including on
    /// transport failure and on `cancelPending`/destruction — or a caller that
    /// waits will wait forever.
    kCallerMayBlock,

    /// @brief The completion cannot settle while the calling thread is blocked
    ///        in a wait, or blocking it would defeat the point of this
    ///        backend. A caller must register its continuation and return.
    ///
    /// Two backends say this, for two different reasons:
    ///
    /// - `QtWebSocketBackend` with `Config::asyncRegistrationEnabled` set: the
    ///   reply arrives through the Qt event loop of the thread that issued the
    ///   call, so waiting is a deadlock. On a WASM main thread it aborts the
    ///   page, which is the case this surface exists for.
    /// - `SynchronousBackendAdapter`: it exists precisely to move a blocking
    ///   call off the caller's thread, so a caller that then waits for it has
    ///   bought nothing — and if the caller happens to be running on the
    ///   adapter's own executor, has deadlocked.
    kCallerMustNotBlock,
};

/// @brief Abstract interface for execution backends (local, remote, …).
///
/// A backend owns model instances and dispatches actions against them.
/// `Bridge` holds one active backend at a time and can swap it atomically
/// via `Bridge::switchBackend()`.
// NOLINTBEGIN(cppcoreguidelines-special-member-functions)
struct IBackend {
    virtual ~IBackend() = default;

    /// @brief Registers a new model instance and returns its opaque id.
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
    /// see `wire::Envelope::contextKey` and `RemoteServer::setLogProvider`.
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

    /// @brief Registers or attaches to the shared instance holding @p primary.
    ///
    /// A *register-or-attach*: if an instance for `(typeId, primary)` is already
    /// live in the backend's shared directory, its id is returned and its attach
    /// count incremented — no new instance is created and @p factory is not
    /// called. Otherwise a new instance is created, entered in the directory,
    /// and returned with an attach count of one.
    ///
    /// An empty @p primary means "no identity": the call degrades to
    /// `registerModelWithContext`, producing a private instance that never
    /// enters the directory and can never be shared.
    ///
    /// The default implementation ignores @p primary and forwards to
    /// `registerModelWithContext`, so a backend that has not implemented sharing
    /// keeps its existing one-instance-per-caller behaviour rather than silently
    /// handing two callers the same instance.
    ///
    /// @param typeId     String type-id of the model.
    /// @param factory    Callable that constructs the `IModelHolder` (local path only).
    /// @param identity   Entity key for the action log plus the directory primary key.
    /// @return Id of the shared (or newly created) instance.
    virtual ::morph::exec::detail::ModelId registerModelShared(
        const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
        InstanceIdentity identity) {
        return registerModelWithContext(typeId, std::move(factory), identity.contextKey);
    }

    /// @brief Re-points from @p current to the shared instance holding @p primary.
    ///
    /// The default implementation acquires the replacement via
    /// `registerModelShared` first and only then releases @p current (when
    /// non-zero), so a same-key re-attach never destroys and recreates the
    /// instance it already holds, and a throwing acquire never strands the
    /// caller with neither instance. Backends behind a wire protocol override
    /// this with the single `attach` request so a re-pointing client cannot
    /// lose its slot to `LimitPolicy::maxLiveModels` between the release and
    /// the acquire.
    ///
    /// @param typeId     String type-id of the model.
    /// @param factory    Callable that constructs the `IModelHolder` (local path only).
    /// @param identity   Entity key for the action log plus the directory primary key.
    /// @param current    Instance currently held, or `ModelId{0}` if none.
    /// @return Id of the instance now attached to.
    virtual ::morph::exec::detail::ModelId attachModel(
        const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
        InstanceIdentity identity, ::morph::exec::detail::ModelId current) {
        // Acquire the replacement before releasing `current`, not after: a
        // same-key re-attach (registerModelShared finds `current` already
        // live in the directory and takes a second reference to it) then
        // hands back the identical id instead of destroying and recreating
        // it, and a throwing acquire never touches `current` at all, so the
        // caller's existing instance is never stranded by a failed attach.
        // Either way, exactly one reference on `current` needs releasing
        // afterward: the genuinely old instance's, if this re-pointed to a
        // different key; or the redundant one registerModelShared just took,
        // if it did not.
        auto next = registerModelShared(typeId, std::move(factory), identity);
        if (current.v != 0U) {
            deregisterModel(current);
        }
        return next;
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

    // ── The structural registration surface ──────────────────────────────
    //
    // `bindModel`/`promoteModel` are the five verbs above with the continuation
    // made mandatory. Two properties carry the design, and both are visible in
    // the signature rather than only in a comment:
    //
    //   1. **The continuation is not opt-in.** There is no `bool` saying "I
    //      have no async path, call the other one" — so no call site carries a
    //      second path, and a backend cannot be *half* migrated. A blocking
    //      backend satisfies the surface through the default implementations
    //      below, or without blocking the caller at all through
    //      `SynchronousBackendAdapter`.
    //
    //      Whether the *calling thread* may wait for that continuation is a
    //      separate question, and the signature cannot answer it either: two
    //      shipped backends return an unsettled `Completion` and give opposite
    //      answers (`SocketBackend`: yes, its I/O thread settles it;
    //      `QtWebSocketBackend` under `asyncRegistrationEnabled`: no, waiting
    //      deadlocks the event loop the reply arrives on). `bindWaitPolicy()`
    //      below carries exactly that one bit and nothing else — it never
    //      selects a verb, so it creates no second path.
    //
    //   2. **The delivery thread is a parameter.** `Completion<T>` posts its
    //      handlers to the executor it was built with, so the continuation runs
    //      where @p cbExec says and nowhere else — the backend does not choose.
    //      A threading contract stated only in prose would ask every backend
    //      author to deliver on a thread from which `~Bridge` cannot run
    //      concurrently, and nothing could check it. See
    //      docs/spec/core/backend.md, "The threading contract, and the half
    //      the surface does not close".
    //      It does not by itself make a `~Bridge` race impossible: it moves the
    //      choice of delivery thread from fifteen backend implementors, none of
    //      which knows what the caller's teardown looks like, to the one caller
    //      that does. `cbExec` is a reference precisely so that "deliver
    //      nowhere" cannot be expressed — a null executor would silently drop
    //      every continuation (see `Completion`'s constructor).
    //
    // The synchronous verbs above remain, but not as a surface any caller
    // chooses: they are what `bindModelBlocking` — and therefore the *default*
    // `bindModel` — dispatches to, one request shape at a time. Nothing in the
    // tree calls them directly, which is why a backend that overrides only
    // `registerModel` still works through `bindModel`.

    /// @brief Acquires a model instance: the structural counterpart of
    ///        `registerModelWithContext` / `registerModelShared` / `attachModel`.
    ///
    /// One verb for all three, selected by @p request's shape — see
    /// `BindRequest`'s table. The default implementation dispatches to exactly
    /// the legacy verb each shape corresponds to and settles the returned
    /// `Completion` from this thread, so a backend that has not overridden
    /// anything keeps its current behaviour bit for bit, including its current
    /// blocking behaviour: the default **blocks the calling thread** for as
    /// long as the underlying synchronous verb does. A backend with a genuine
    /// non-blocking path (`QtWebSocketBackend`) overrides this and
    /// settles the `Completion` when its reply arrives; a blocking backend that
    /// must not block its caller is wrapped in `SynchronousBackendAdapter`,
    /// which moves the blocking call to an executor it names.
    ///
    /// An exception thrown by the underlying verb is delivered to the
    /// `Completion`'s `onError` rather than propagated, so a caller has one
    /// failure channel instead of two.
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
            promise.resolve(bindModelBlocking(std::move(request)));
        } catch (...) {
            promise.reject(std::current_exception());
        }
        return std::move(completion);
    }

    /// @brief Files an already-live instance under a key: the structural
    ///        counterpart of `assignPrimary`.
    ///
    /// The default implementation calls `assignPrimary` and settles the
    /// returned `Completion` from this thread, echoing @p request's `mid`
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

    /// @brief Whether a caller may block until this backend's
    ///        `bindModel`/`promoteModel` completions settle.
    ///
    /// Answers the one question `Completion` cannot: an unsettled `Completion`
    /// looks the same whether the reply is coming from a thread the caller does
    /// not own or from the caller's own event loop. Only a frame that would
    /// otherwise stop and wait asks it — `Bridge::registerHandlerImpl` (a
    /// synchronous entry point that must hand back a *bound* instance),
    /// `Bridge::switchBackend`'s staging phase, and
    /// `Bridge::installReconnectHandler`'s handler, which is the one that runs
    /// on the backend's own transport thread and so is the one a wrong answer
    /// deadlocks outright. The asynchronous entry points never wait and never
    /// consult it.
    ///
    /// A backend that returns `kCallerMayBlock` (the default) commits to
    /// settling every `Completion` it returns exactly once without any further
    /// call from the caller. Every backend that does not override `bindModel`
    /// satisfies that trivially: the default settles before it returns.
    ///
    /// @return `BindWait::kCallerMayBlock` unless overridden.
    [[nodiscard]] virtual BindWait bindWaitPolicy() const noexcept { return BindWait::kCallerMayBlock; }

    /// @brief Runs @p request against the legacy synchronous verbs, blocking.
    ///
    /// Factored out of `bindModel`'s default implementation so that
    /// `SynchronousBackendAdapter` runs the *same* dispatch on its own executor
    /// instead of restating it — one definition of "which legacy verb does this
    /// request shape mean", not two that can drift apart.
    ///
    /// Each branch is the verb the corresponding call site uses today, so
    /// routing a plain registration through `attachModel` (which would also
    /// have worked, since its default degrades) cannot change behaviour for a
    /// backend that overrides only some of the three.
    ///
    /// @param request Owning bind request; moved from.
    /// @return The bound `ModelId`.
    ::morph::exec::detail::ModelId bindModelBlocking(BindRequest request) {
        InstanceIdentity const identity{.contextKey = request.contextKey, .primary = request.primary};
        if (request.current.v != 0U) {
            return attachModel(request.typeId, std::move(request.factory), identity, request.current);
        }
        if (!request.primary.empty()) {
            return registerModelShared(request.typeId, std::move(request.factory), identity);
        }
        return registerModelWithContext(request.typeId, std::move(request.factory), request.contextKey);
    }

    /// @brief Lists the primary keys of live shared instances of @p typeId.
    ///
    /// Only instances created through `registerModelShared`/`attachModel` with a
    /// non-empty primary appear; a private instance is invisible to the
    /// directory by construction. The result is a snapshot and is stale the
    /// moment it is returned.
    ///
    /// Synchronous, matching `registerModel`, which already blocks on remote
    /// backends. The asynchronous surface users see is
    /// `BridgeHandler::instances()`, which wraps this in a `Completion` so the
    /// call site reads identically local and remote.
    ///
    /// @param typeId String type-id to enumerate.
    /// @return Canonical key strings of the live shared instances; empty by default.
    virtual std::vector<std::string> listInstances(const std::string& typeId) {
        (void)typeId;
        return {};
    }

    /// @brief Removes the model identified by @p mid from the backend.
    ///
    /// For a shared instance this *decrements* its attach count and destroys the
    /// instance only when the count reaches zero, so one caller releasing an
    /// instance never tears it out from under another that is still attached.
    virtual void deregisterModel(::morph::exec::detail::ModelId mid) = 0;

    /// @brief Dispatches @p call against the model identified by @p mid.
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
    /// in-flight server replies cannot resurrect a cancelled completion.
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
    /// Default implementation: store-and-ignore. Backends with no transport (e.g.
    /// `LocalBackend`) never invoke it.
    /// @param handler Callable invoked on the backend's transport thread after a
    ///                successful reconnect. Pass `nullptr` to clear.
    virtual void setReconnectHandler(const std::function<void()>& handler) { (void)handler; }

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
    /// are built directly by the concrete backend (`registerModelWithContext`,
    /// `registerModelShared`, `attachModel`, `assignPrimary`, `deregisterModel`),
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

/// @brief Makes a blocking backend satisfy the structural registration surface
///        without blocking the caller — and without touching the backend.
///
/// A decorator, not a base class: it wraps an existing `IBackend` and forwards
/// every verb to it, overriding only `bindModel`/`promoteModel` to run the
/// wrapped backend's *synchronous* control call on an executor this adapter
/// names, then settle the returned `Completion`. That is the whole trick, and
/// it is why a backend with no non-blocking path of its own (every backend in
/// the tree except `QtWebSocketBackend`) reaches the structural surface by
/// being wrapped rather than by being rewritten.
///
/// @par What it does and does not change
/// The wrapped backend still blocks — nothing here makes a nested event loop
/// or a socket round trip non-blocking. What changes is **which thread pays**:
/// the blocking call happens on the executor passed at construction, so the
/// caller's thread returns from `bindModel` immediately with an unresolved
/// `Completion`. A single-threaded WASM main thread has no such executor to
/// offer and is therefore not what this adapter is for; that case needs a
/// backend with a genuinely non-blocking path.
///
/// @par Why the executor is required rather than optional
/// "Where does the blocking happen" is the only question this class exists to
/// answer, so it is a constructor parameter with no default. An adapter that
/// silently ran the call inline when handed nothing would be a `bindModel`
/// that blocks on some configurations and not others — the same
/// contract-by-configuration the surface is replacing.
///
/// @par Ordering
/// Control calls are serialised onto one strand, so the wrapped backend sees
/// them one at a time, as it did when the blocking call itself serialised
/// callers. `~SynchronousBackendAdapter` waits for every queued and in-flight
/// control call to finish, so a reply can never land in a destroyed adapter;
/// the executor must therefore still be running tasks when this adapter is
/// destroyed (see docs/spec/concurrency_and_lifetimes.md, "Destruction
/// ordering"). The single-threaded WebAssembly build has no thread to wait
/// for: there the control calls still queued are dropped.
///
/// @par Reconnect handlers
/// A control call issued from a reconnect handler runs on the strand, never on
/// the wrapped backend's transport thread, so a backend whose reply can only
/// be delivered by the thread that is running the reconnect handler does not
/// wait on itself. Whether that is enough to settle `SocketBackend`'s
/// documented reconnect hazard is not a claim made here.
class SynchronousBackendAdapter : public detail::IBackend {
public:
    /// @brief Wraps @p inner, running its blocking control calls on @p blockingExec.
    /// @param inner        Backend to wrap. Owned (shared): the adapter keeps it
    ///                     alive for as long as any control call it dispatched is
    ///                     still in flight. Must not be null.
    /// @param blockingExec Executor the wrapped backend's blocking control calls
    ///                     run on. Borrowed: it must outlive this adapter and
    ///                     keep running tasks until the destructor's wait has
    ///                     completed.
    /// @throws std::invalid_argument if @p inner is null.
    SynchronousBackendAdapter(std::shared_ptr<detail::IBackend> inner,
                              ::morph::exec::IExecutor& blockingExec MORPH_LIFETIMEBOUND)
        : _inner{std::move(inner)}, _control{blockingExec} {
        if (_inner == nullptr) {
            throw std::invalid_argument{"SynchronousBackendAdapter requires a backend to wrap"};
        }
    }

    /// @brief Waits for every queued and in-flight control call; see
    ///        "Ordering" above.
    ~SynchronousBackendAdapter() override { _control.drain(); }

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
    /// Posts `IBackend::bindModelBlocking(request)` — the same legacy-verb
    /// dispatch the default `bindModel` performs inline — to this adapter's
    /// control strand and returns immediately. Exactly one of the returned
    /// `Completion`'s `then`/`onError` handlers runs, on @p cbExec.
    /// @param request Owning bind request; moved from.
    /// @param cbExec  Executor the continuation is delivered on. Borrowed: it
    ///                must outlive the returned `Completion`.
    /// @return A `Completion` resolved with the bound `ModelId`, or rejected
    ///         with whatever the wrapped backend threw.
    ::morph::async::Completion<::morph::exec::detail::ModelId> bindModel(detail::BindRequest request,
                                                                         ::morph::exec::IExecutor& cbExec) override {
        return dispatch(cbExec, [inner = _inner, request = std::move(request)]() mutable {
            return inner->bindModelBlocking(std::move(request));
        });
    }

    /// @brief Files an already-live instance under a key without blocking the caller.
    ///
    /// The promote counterpart of `bindModel` above, on the same strand and
    /// with the same settling rules: resolves with `request.mid` (echoed, as
    /// `IBackend::promoteModel` documents) or rejects with what the wrapped
    /// backend threw.
    /// @param request Owning promote request; moved from.
    /// @param cbExec  Executor the continuation is delivered on. Borrowed: it
    ///                must outlive the returned `Completion`.
    /// @return A `Completion` resolved with `request.mid`, or rejected.
    ::morph::async::Completion<::morph::exec::detail::ModelId> promoteModel(
        detail::PromoteRequest request, ::morph::exec::IExecutor& cbExec) override {
        return dispatch(cbExec, [inner = _inner, request = std::move(request)]() mutable {
            inner->assignPrimary(request.mid, request.typeId, request.primary);
            return request.mid;
        });
    }

    /// @brief This adapter's whole purpose is that the caller does not block.
    ///
    /// Not forwarded to the wrapped backend, unlike everything below:
    /// `bindModel`/`promoteModel` are the two verbs this adapter *reshapes*, so
    /// the policy describing them describes this adapter, not what it wraps. A
    /// caller that waited would pay exactly the blocking cost the adapter was
    /// interposed to move elsewhere, and — if it happens to be running on
    /// `blockingExec` — would deadlock against the strand it is waiting on.
    ///
    /// @return `BindWait::kCallerMustNotBlock`, always.
    [[nodiscard]] detail::BindWait bindWaitPolicy() const noexcept override {
        return detail::BindWait::kCallerMustNotBlock;
    }

    // ── Everything else is forwarded unchanged ───────────────────────────
    //
    // A decorator has to forward every verb it does not reshape. Those are the
    // synchronous verbs only: a wrapped backend has no non-blocking path of
    // its own left to forward, because the one verb
    // that could carry one — `bindModel` — is the verb this adapter
    // reshapes, and a backend that already has a non-blocking `bindModel`
    // has no reason to be wrapped.

    /// @brief Forwards to the wrapped backend.
    /// @param typeId  String type-id of the model to instantiate.
    /// @param factory Callable that constructs the `IModelHolder`.
    /// @return The wrapped backend's `ModelId`.
    ::morph::exec::detail::ModelId registerModel(
        const std::string& typeId,
        std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory) override {
        return _inner->registerModel(typeId, std::move(factory));
    }

    /// @brief Forwards to the wrapped backend.
    /// @param typeId     String type-id of the model to instantiate.
    /// @param factory    Callable that constructs the `IModelHolder`.
    /// @param contextKey Stable identity of the new instance; empty if none.
    /// @return The wrapped backend's `ModelId`.
    ::morph::exec::detail::ModelId registerModelWithContext(
        const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
        std::string_view contextKey) override {
        return _inner->registerModelWithContext(typeId, std::move(factory), contextKey);
    }

    /// @brief Forwards to the wrapped backend.
    /// @param typeId   String type-id of the model.
    /// @param factory  Callable that constructs the `IModelHolder`.
    /// @param identity Entity key for the action log plus the directory primary key.
    /// @return The wrapped backend's `ModelId`.
    ::morph::exec::detail::ModelId registerModelShared(
        const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
        detail::InstanceIdentity identity) override {
        return _inner->registerModelShared(typeId, std::move(factory), identity);
    }

    /// @brief Forwards to the wrapped backend.
    /// @param typeId   String type-id of the model.
    /// @param factory  Callable that constructs the `IModelHolder`.
    /// @param identity Entity key for the action log plus the directory primary key.
    /// @param current  Instance currently held, or `ModelId{0}` if none.
    /// @return The wrapped backend's `ModelId`.
    ::morph::exec::detail::ModelId attachModel(
        const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
        detail::InstanceIdentity identity, ::morph::exec::detail::ModelId current) override {
        return _inner->attachModel(typeId, std::move(factory), identity, current);
    }

    /// @brief Forwards to the wrapped backend.
    /// @param mid     Live instance to promote.
    /// @param typeId  Model type id — the directory's first key component.
    /// @param primary Canonical string encoding of the key to file it under.
    void assignPrimary(::morph::exec::detail::ModelId mid, const std::string& typeId,
                       std::string_view primary) override {
        _inner->assignPrimary(mid, typeId, primary);
    }

    /// @brief Forwards to the wrapped backend.
    /// @param typeId String type-id to enumerate.
    /// @return The wrapped backend's snapshot of live shared instance keys.
    std::vector<std::string> listInstances(const std::string& typeId) override {
        return _inner->listInstances(typeId);
    }

    /// @brief Forwards to the wrapped backend.
    /// @param mid Instance to release.
    void deregisterModel(::morph::exec::detail::ModelId mid) override { _inner->deregisterModel(mid); }

    /// @brief Forwards to the wrapped backend.
    /// @param mid    Instance to dispatch against.
    /// @param call   The action call to dispatch.
    /// @param cbExec Executor the resulting `Completion`'s callbacks are posted on.
    /// @return The wrapped backend's `Completion`.
    ::morph::async::Completion<std::shared_ptr<void>> execute(::morph::exec::detail::ModelId mid,
                                                              detail::ActionCall call,
                                                              ::morph::exec::IExecutor* cbExec) override {
        return _inner->execute(mid, std::move(call), cbExec);
    }

    /// @brief Forwards to the wrapped backend.
    void notifyBackendChanged() override { _inner->notifyBackendChanged(); }

    /// @brief Rejects the completions *this adapter* produced, then forwards.
    ///
    /// Not a plain forward, unlike everything else in this block, and for the
    /// same reason `bindWaitPolicy()` is not: the two verbs this adapter
    /// reshapes produce completions the wrapped backend has never heard of.
    /// A `bindModel` here settles from a task on `_control`, so
    /// `_inner->cancelPending` reaches nothing of it: without this override a
    /// bind dispatched through this adapter goes on to resolve **successfully**
    /// after cancellation, which is the exact opposite of what
    /// `IBackend::cancelPending` promises ("after this call, any later
    /// `setValue`/`setException` on those states is a no-op").
    ///
    /// The adapter's own promises are rejected first and the forward happens
    /// second: the strand task is still running `op()` while this executes, and
    /// rejecting before handing control to the wrapped backend keeps that
    /// window as short as the caller's own call. A task that finished first
    /// wins — its completion was not "still pending" — and a task that finishes
    /// after finds the promise settled, which `CompletionState::setValue`'s
    /// `if (ready) return;` makes a no-op.
    ///
    /// Settling the promise is only half of it, because it cannot stop work the
    /// strand has already been handed. Each dispatched task therefore carries a
    /// cancellation flag next to its promise, and this verb **sets that flag
    /// before rejecting**: a task still queued on `_control` sees it when it
    /// reaches the head of the strand and returns without calling `op()`, so
    /// the blocking control call never reaches the wrapped backend at all.
    /// Without the flag the caller is told the bind was cancelled while the
    /// registration goes through anyway — a live instance on a backend whose
    /// `Bridge` is gone, which nothing will ever `deregisterModel`.
    ///
    /// **What this still does not do:** a task already *inside* `op()` cannot
    /// be recalled. Only the queued-but-not-started window is closed, which is
    /// all this adapter can close — it has no way to interrupt a blocking verb
    /// it does not implement. A task that wins the race by a few instructions
    /// (flag read, then this store) registers exactly as one that had already
    /// entered `op()`, and its completion stays rejected either way.
    /// @param exc Exception delivered to every still-pending completion, this
    ///            adapter's own and then the wrapped backend's.
    void cancelPending(const std::exception_ptr& exc) override {
        std::vector<std::weak_ptr<PendingControl>> snapshot;
        {
            std::scoped_lock const lock{_pendingMtx};
            snapshot.swap(_pending);
            _compactAt = kPendingCompactFloor;
        }
        for (auto& pendingWeak : snapshot) {
            if (auto pending = pendingWeak.lock()) {
                // Flag first, promise second. A task that reads the flag after
                // this store declines to run; one that read it just before
                // finds its promise already rejected by the line below. That
                // is the narrowest window this ordering leaves.
                pending->cancelled.store(true, std::memory_order_release);
                pending->promise.reject(exc);
            }
        }
        _inner->cancelPending(exc);
    }

    /// @brief Forwards to the wrapped backend.
    /// @param handler Callable invoked after a successful reconnect; `nullptr` clears.
    void setReconnectHandler(const std::function<void()>& handler) override { _inner->setReconnectHandler(handler); }

    /// @brief Forwards to the wrapped backend.
    /// @param handler Callable invoked after every successful connect; `nullptr` clears.
    void setConnectHandler(const std::function<void()>& handler) override { _inner->setConnectHandler(handler); }

    /// @brief Forwards to the wrapped backend.
    /// @param handler Callable invoked whenever the transport drops; `nullptr` clears.
    void setDisconnectHandler(const std::function<void()>& handler) override { _inner->setDisconnectHandler(handler); }

    /// @brief Forwards to the wrapped backend.
    /// @param session Session stamped onto every subsequently built control envelope.
    void setSession(::morph::session::Context session) override { _inner->setSession(std::move(session)); }

private:
    /// @brief One dispatched control call: its promise and its cancellation flag.
    ///
    /// The two travel together because `cancelPending` has to act on both:
    /// acting on the promise alone leaves the queued task free to make the
    /// blocking control call the caller has just been told was cancelled.
    /// The strand task holds the only `shared_ptr` to this
    /// record; `_pending` holds `weak_ptr`s, so an entry expires by itself when
    /// the task is destroyed.
    struct PendingControl {
        /// @brief Takes ownership of the dispatched call's promise.
        /// @param dispatched Producer side of the `Completion` handed to the caller.
        explicit PendingControl(BindPromise dispatched) : promise{std::move(dispatched)} {}

        /// @brief Producer side of the completion this call settles.
        ///
        /// Touched by the strand task (resolve/reject) and by `cancelPending`
        /// (reject); `Promise`'s own `CompletionState` is internally
        /// synchronised, so no further lock is needed here.
        BindPromise promise;

        /// @brief Set by `cancelPending` before it rejects; read by the task
        ///        before it calls `op()`.
        ///
        /// Atomic rather than guarded by `_pendingMtx`, so the strand task
        /// never has to take a lock the caller's thread also takes just to
        /// learn whether it should run.
        std::atomic_bool cancelled{false};
    };

    /// @brief Posts @p op to the control strand and settles a `Completion` with its outcome.
    ///
    /// The posted task captures the wrapped backend's `shared_ptr` and the
    /// promise, never `this`, so nothing it touches depends on the adapter
    /// still existing.
    /// @tparam Op     Callable returning the `ModelId` the completion resolves with.
    /// @param  cbExec Executor the continuation is delivered on.
    /// @param  op     The blocking control call to run on the strand.
    /// @return A `Completion` settled by @p op's outcome.
    template <typename Op>
    ::morph::async::Completion<::morph::exec::detail::ModelId> dispatch(::morph::exec::IExecutor& cbExec, Op op) {
        using Settled = ::morph::async::Completion<::morph::exec::detail::ModelId>;
        auto [completion, promise] = Settled::makeSettleable(&cbExec);
        auto pending = std::make_shared<PendingControl>(std::move(promise));
        // Tracked *before* the post, not after: a `cancelPending` that lands in
        // between would otherwise find an empty list and leave a completion
        // that is genuinely pending uncancelled. Rejecting a promise whose task
        // has not started yet is safe — the task's own `resolve` then finds the
        // state ready and returns.
        trackPending(pending);
        _control.post(kControlStrand, [pending, op = std::move(op)]() mutable {
            // Checked *before* `op()`: a
            // promise settled by `cancelPending` makes the reply a no-op but
            // says nothing about the call, and this task is the last place that
            // can decline to make it. Read with acquire against
            // `cancelPending`'s release store, so a task that observes the flag
            // also observes everything the cancelling thread did before setting
            // it.
            if (pending->cancelled.load(std::memory_order_acquire)) {
                return;
            }
            try {
                pending->promise.resolve(op());
            } catch (...) {
                pending->promise.reject(std::current_exception());
            }
        });
        return std::move(completion);
    }

    /// @brief Records @p pending as cancellable until its task settles it.
    ///
    /// The strand task holds the only `shared_ptr` to the record, so an entry
    /// here expires exactly when that task is destroyed — "still pending" needs
    /// no separate bookkeeping and no erase on the success path.
    ///
    /// Swept on the same amortised schedule as `LocalBackend::trackPending`:
    /// dead entries are reclaimed only when the list reaches
    /// `_compactAt`, which each sweep re-arms at twice the surviving count, so
    /// the per-dispatch cost is O(1) and the list stays bounded at twice the
    /// live count plus the floor. Control calls are serialised onto one strand,
    /// so in practice the live count is one and the floor is never reached;
    /// without the sweep the list would still grow without bound on an adapter
    /// whose `cancelPending` is never called.
    /// @param pending Record to cancel and reject if `cancelPending` runs before
    ///                 its task settles it.
    void trackPending(const std::shared_ptr<PendingControl>& pending) {
        std::scoped_lock const lock{_pendingMtx};
        if (_pending.size() >= _compactAt) {
            std::erase_if(_pending, [](const auto& weak) { return weak.expired(); });
            _compactAt = std::max(kPendingCompactFloor, _pending.size() * 2);
        }
        _pending.emplace_back(pending);
    }

    /// @brief The single strand key every control call shares, so they run one
    ///        at a time. Not a real model id: these strands are private to the
    ///        adapter and share no key space with any backend's own.
    static constexpr ::morph::exec::detail::ModelId kControlStrand{1};

    /// @brief Smallest size at which `trackPending` sweeps; see `LocalBackend`'s.
    static constexpr std::size_t kPendingCompactFloor = 32;

    std::shared_ptr<detail::IBackend> _inner;
    ::morph::exec::detail::ModelStrands _control;
    mutable std::mutex _pendingMtx;
    // Every `bindModel`/`promoteModel` record handed to a `_control` task and
    // not yet settled by it. Weak, so a settled task's record drops out on its
    // own; guarded by `_pendingMtx`, because `cancelPending` is called from
    // `Bridge`'s thread while `dispatch` runs on whichever thread called it.
    std::vector<std::weak_ptr<PendingControl>> _pending;
    // Size at which `trackPending` next sweeps `_pending`; re-armed at twice
    // the surviving count. Guarded by `_pendingMtx` with `_pending` itself.
    std::size_t _compactAt = kPendingCompactFloor;
};

/// @brief In-process backend that executes model actions on a thread pool strand.
///
/// Each model instance gets its own strand so actions are serialised per-model
/// without a global lock on the pool.
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
    ///        and only then closes them. See `docs/spec/core/coroutines.md`,
    ///        "Teardown".
    ///
    /// In that order, because each step needs the one before it:
    /// 1. Every live Task run's stop is requested. A handler suspended in an
    ///    awaitable that resumes on the current executor -- morph's own,
    ///    `core::async::AsyncQueue::pop` -- resumes, cancelled, through its
    ///    strand, which is still open. One suspended on a `core::net` socket or
    ///    timer resumes on that loop instead, and unwinds there.
    /// 2. The strands are drained: the resumptions queued on them, and every
    ///    queued action -- skipped, if `cancelPending` already failed it. A
    ///    handler's end, and with it leaving the gate and starting the next
    ///    action, always runs on the strand, wherever the handler finished; the
    ///    drain does not wait for a handler still unwinding on another executor.
    /// 3. The strands are closed. A handler that ends after this -- one that
    ///    unwound elsewhere, or ignored the stop -- finishes inline, where
    ///    nothing on the drained strands can race it.
    ///
    /// Must not run on one of this backend's strand threads, whose drain it
    /// would wait for; a debug build asserts that. The single-threaded
    /// WebAssembly build waits for nothing: step 2 is skipped, and closing
    /// drops what is still queued (see `ModelStrands::drain`).
    ~LocalBackend() override {
        std::vector<std::weak_ptr<LocalRun>> runs;
        {
            std::scoped_lock const lock{_taskRunsMtx};
            runs.swap(_taskRuns);
        }
        for (auto const& weak : runs) {
            if (auto const run = weak.lock()) {
                run->stopSource->request_stop();
            }
        }
        _strands->drain();
        _strands->close();
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
        ::morph::observe::detail::emitMetric(::morph::observe::Metric::registerCount, 1.0);
        std::scoped_lock const lock{_regMtx};
        return createAndTrack(std::move(factory));
    }

    /// @brief Registers or attaches to the shared instance holding @p primary.
    ///
    /// An empty @p primary bypasses the directory entirely and produces a
    /// private instance, exactly as `registerModel` does.
    /// @param typeId     String type-id of the model — the directory's first key component.
    /// @param factory    Callable that constructs the `IModelHolder`; not called on an attach.
    /// @param identity   Entity key for the action log plus the directory primary key.
    /// @return Id of the shared (or newly created) instance.
    ::morph::exec::detail::ModelId registerModelShared(
        const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
        detail::InstanceIdentity identity) override {
        if (identity.primary.empty()) {
            return registerModelWithContext(typeId, std::move(factory), identity.contextKey);
        }
        ::morph::observe::detail::emitMetric(::morph::observe::Metric::registerCount, 1.0);
        detail::DirectoryKey dirKey{typeId, std::string{identity.primary}};
        std::scoped_lock const lock{_regMtx};
        // Register-or-attach, including the lazy eviction of an instance whose
        // first action failed, now lives in `InstanceDirectory::attach` — the
        // one place `RemoteServer` reaches for it too.
        if (auto const attached = _instances.attach(dirKey)) {
            return *attached;
        }
        auto [mid, holder] = createHolder(std::move(factory));
        _instances.insertShared(mid, std::move(holder), std::move(dirKey));
        return mid;
    }

    /// @brief Enters an already-live, still-anonymous instance into the
    ///        directory under @p primary. Thread-safe.
    /// @param mid     Live instance to promote.
    /// @param typeId  Model type id — the directory's first key component.
    /// @param primary Canonical string encoding of the key to file it under.
    void assignPrimary(::morph::exec::detail::ModelId mid, const std::string& typeId,
                       std::string_view primary) override {
        if (primary.empty()) {
            return;
        }
        std::scoped_lock const lock{_regMtx};
        // A no-op unless `mid` is live, has never held a directory key, and the
        // key is free — `InstanceDirectory::promote` holds all the guards and
        // the reasons for them.
        (void)_instances.promote(mid, detail::DirectoryKey{typeId, std::string{primary}});
    }

    /// @brief Lists the primary keys of live shared instances of @p typeId. Thread-safe.
    /// @param typeId String type-id to enumerate.
    /// @return Canonical key strings of the live shared instances, in unspecified order.
    std::vector<std::string> listInstances(const std::string& typeId) override {
        std::scoped_lock const lock{_regMtx};
        return _instances.keysOfType(typeId);
    }

    /// @brief Removes the model with @p mid, or releases one attachment to it. Thread-safe.
    ///
    /// A private instance is erased outright. A shared instance has its attach
    /// count decremented and is erased — and removed from the directory — only
    /// when that count reaches zero, so releasing one handler never destroys an
    /// instance another handler still holds.
    /// @param mid Id returned by a prior `registerModel()`/`registerModelShared()` call.
    void deregisterModel(::morph::exec::detail::ModelId mid) override {
        ::morph::observe::detail::emitMetric(::morph::observe::Metric::deregisterCount, 1.0);
        std::scoped_lock const lock{_regMtx};
        // One lookup, not five: the attach count, the directory key and the
        // holder are one record, so releasing the last reference unfiles and
        // destroys it in a single step that cannot half-happen.
        if (_instances.release(mid) == detail::InstanceDirectory::Release::destroyed) {
            _changeAware.erase(mid);
        }
    }

    /// @brief Schedules `onBackendChanged()` on each change-aware model's strand. Thread-safe.
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
    /// - **Not under `Bridge::_mtx`.** `Bridge::switchBackend` calls this while
    ///   holding `_mtx`, but only the cheap `post()` runs there; the model body
    ///   runs later on a pool thread. A model that re-enters the bridge from
    ///   `onBackendChanged()` (`switchBackend`/`registerHandler`/`deregisterHandler`)
    ///   therefore acquires `_mtx` freshly on the strand thread instead of
    ///   deadlocking on a lock the switch caller still holds.
    ///
    /// A `shared_ptr` copy of each holder is captured into the posted task so a
    /// concurrent `deregisterModel` cannot free the model out from under its
    /// pending notification (mirrors `execute`'s holder capture).
    void notifyBackendChanged() override {
        std::vector<std::pair<::morph::exec::detail::ModelId, std::shared_ptr<::morph::model::detail::IModelHolder>>>
            aware;
        {
            std::scoped_lock const lock{_regMtx};
            aware.reserve(_changeAware.size());
            for (auto modelId : _changeAware) {
                if (const auto* inst = _instances.find(modelId)) {
                    aware.emplace_back(modelId, inst->holder);
                }
            }
        }
        for (auto& [modelId, holder] : aware) {
            _strands->post(modelId, [held = std::move(holder)]() mutable { held->onBackendChanged(); });
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

        std::shared_ptr<::morph::model::detail::IModelHolder> holder;
        std::shared_ptr<detail::HydrationState> hydration;
        {
            std::scoped_lock const lock{_regMtx};
            // One lookup for the holder and its hydration state together: they
            // are fields of one record now, not entries in two maps kept in
            // lockstep by convention.
            if (const auto* inst = _instances.find(mid)) {
                holder = inst->holder;
                hydration = inst->hydration;
            }
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

    /// @brief Resolves every still-pending completion this backend produced with @p exc.
    /// @param exc Exception delivered to every pending completion's error sink.
    void cancelPending(const std::exception_ptr& exc) override {
        std::vector<std::weak_ptr<::morph::async::detail::ISettleSink>> snapshot;
        {
            std::scoped_lock const lock{_pendingMtx};
            snapshot.swap(_pending);
            _compactAt = kPendingCompactFloor;
            // Under the same lock as the swap: a run admitted before this
            // point is in `snapshot` and is failed below, and one admitted
            // after it is not. See `startLocal`.
            _cancels->record(exc);
        }
        for (auto& weak : snapshot) {
            if (auto sink = weak.lock()) {
                // May race a reply settling the same sink; `ISettleSink`'s
                // contract makes the second call a no-op, which is what
                // `CompletionState`'s first-result-wins gave this loop before
                // the sink existed.
                sink->settleException(exc);
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
        std::scoped_lock const lock{_pendingMtx};
        return _pending.size();
    }

private:
    /// @brief Builds a holder via @p factory, records it under a fresh id, and
    ///        returns that id. Caller holds `_regMtx`.
    ///
    /// Shared by `registerModel`'s private-instance path and
    /// `registerModelShared`'s fresh-instance path: both need exactly this —
    /// construct, note change-awareness, file into `_instances` — before going
    /// on to their own, differing bookkeeping (`registerModelShared` files the
    /// instance under its directory key instead of as a private one).
    /// @param factory Callable that constructs the `IModelHolder`.
    /// @return Newly assigned `ModelId`.
    ::morph::exec::detail::ModelId createAndTrack(
        std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory) {
        auto [mid, holder] = createHolder(std::move(factory));
        _instances.insertPrivate(mid, std::move(holder));
        return mid;
    }

    /// @brief Allocates an id, builds the holder and notes change-awareness,
    ///        without filing it anywhere. Caller holds `_regMtx`.
    ///
    /// The half `registerModel`'s private path and `registerModelShared`'s
    /// fresh-instance path have in common; they differ only in how the result is
    /// filed, which is `InstanceDirectory`'s job.
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
    /// `weak_ptr::expired()` loads under `_pendingMtx` to admit one call with
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
        std::scoped_lock const lock{_pendingMtx};
        if (_pending.size() >= _compactAt) {
            std::erase_if(_pending, [](const auto& weak) { return weak.expired(); });
            _compactAt = std::max(kPendingCompactFloor, _pending.size() * 2);
        }
        _pending.emplace_back(sink);
        return _cancels->epoch();
    }

    /// What `cancelPending` has done so far, shared with every run: how many
    /// times it has run, and with what reason the last time.
    class CancelRecord {
    public:
        /// Stores @p reason, then bumps the epoch. Called under `_pendingMtx`.
        void record(const std::exception_ptr& reason) {
            {
                std::scoped_lock const lock{_mtx};
                _reason = reason;
            }
            _epoch.fetch_add(1);
        }

        /// @return How many times `cancelPending` has run.
        [[nodiscard]] std::uint64_t epoch() const noexcept { return _epoch.load(); }

        /// @return The reason the last `cancelPending` gave.
        [[nodiscard]] std::exception_ptr lastReason() {
            std::scoped_lock const lock{_mtx};
            return _reason;
        }

    private:
        std::atomic<std::uint64_t> _epoch{0};
        std::mutex _mtx;
        std::exception_ptr _reason;
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
        std::scoped_lock const lock{_taskRunsMtx};
        if (_taskRuns.size() >= _taskRunsCompactAt) {
            std::erase_if(_taskRuns, [](const auto& weak) { return weak.expired(); });
            _taskRunsCompactAt = std::max(kPendingCompactFloor, _taskRuns.size() * 2);
        }
        _taskRuns.emplace_back(run);
    }

    // One strand per model instance. Shared with the Task handlers started
    // here, whose resumers outlive the backend; closed by the destructor.
    std::shared_ptr<::morph::exec::detail::ModelStrands> _strands;
    std::mutex _regMtx;
    // Every live instance, private and shared alike, plus the shared-instance
    // directory over them — holder, attach count, directory key and hydration
    // state as one record per instance rather than five parallel ModelId-keyed
    // maps held in lockstep by convention. Guarded by `_regMtx`;
    // `InstanceDirectory` is caller-locked by design, see its doc comment.
    detail::InstanceDirectory _instances;
    // Ids of models whose holder answered `isBackendChangeAware() == true` at
    // registration time. An index over `_instances`, maintained under `_regMtx`
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
    mutable std::mutex _pendingMtx;
    // Sinks, not completion states: a dispatch's settle point is whatever the
    // caller handed down, which for `Bridge` is its own typed completion state.
    // A `weak_ptr`, so this list cannot keep a finished dispatch alive.
    std::vector<std::weak_ptr<::morph::async::detail::ISettleSink>> _pending;
    // Size at which `trackPending` next sweeps `_pending` for expired entries;
    // re-armed at twice the surviving count after each sweep. Guarded by
    // `_pendingMtx` along with `_pending` itself. See `trackPending`.
    std::size_t _compactAt = kPendingCompactFloor;
    // Recorded by `cancelPending` under `_pendingMtx`, as it takes the pending
    // list. Shared with every run, which notes the epoch it was admitted under
    // and skips its handler if the epoch has moved on by the time it starts.
    std::shared_ptr<CancelRecord> _cancels = std::make_shared<CancelRecord>();
    // The Task runs the destructor stops. Weak, so a finished run is not kept.
    std::mutex _taskRunsMtx;
    std::vector<std::weak_ptr<LocalRun>> _taskRuns;
    std::size_t _taskRunsCompactAt = kPendingCompactFloor;
    // Concurrent in-flight executes, for the executeInFlight metric. A
    // shared_ptr (not a plain atomic member) so strand tasks hold their own
    // reference instead of capturing `this` — see execute()'s comment.
    std::shared_ptr<std::atomic<std::size_t>> _inFlight = std::make_shared<std::atomic<std::size_t>>(0);
};

}  // namespace morph::backend
