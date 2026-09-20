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
struct ActionCall {
    /// @brief String id of the target model type (from `ModelTraits`).
    std::string modelTypeId;

    /// @brief String id of the action type (from `ActionTraits`).
    std::string actionTypeId;

    /// @brief Serialises the action to JSON. Called only on the remote path.
    std::function<std::string()> serializeAction;

    /// @brief Deserialises a JSON reply into the opaque result `shared_ptr<void>`.
    std::function<std::shared_ptr<void>(std::string_view)> deserializeResult;

    /// @brief Executes the action directly against a model holder. Used on the local path.
    std::function<std::shared_ptr<void>(::morph::model::detail::IModelHolder&)> localOp;

    /// @brief Session context attached to this call.
    ///
    /// Local backends thread it through a thread-local before invoking `localOp`;
    /// remote backends serialise it into the wire envelope.
    ::morph::session::Context session;
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
/// than before it. The legacy `*Async` verbs take views and are safe only
/// because their one implementor happens to encode before returning.
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
/// This is the one thing `bindModel`'s signature cannot say, and morph#593 is
/// what happens when it is not said: two shipped backends both return an
/// unsettled `Completion` from `bindModel`, and `Bridge::registerHandlerImpl`
/// — a synchronous entry point that hands its caller a `BridgeHandler` usable
/// on the next line — must wait for one of them and must not wait for the
/// other. From the `Completion` alone the two are indistinguishable.
///
/// It is deliberately **not** the `bool` the surface removed (see "The
/// structural registration surface" below, point 1). That `bool` chose
/// *which verb to call*, so every call site carried two paths and a backend
/// could be half-migrated. This one chooses nothing: there is still exactly
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
    ///   page (morph#568), which is the case that surface exists for.
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

    /// @brief Optional non-blocking counterpart to `registerModelWithContext`.
    ///
    /// `registerModelWithContext`/`registerModel` are synchronous: a backend
    /// whose registration requires a round-trip (a socket backend) can only
    /// implement that by blocking the calling thread until the reply arrives —
    /// `QtWebSocketBackend` does this via a nested `QEventLoop`. On a WASM main
    /// thread, Qt refuses to spin a nested loop at all
    /// (`WaitForMoreEvents is not supported on the main thread without asyncify`),
    /// so that blocking call aborts the page — the very first `registerModel`
    /// a WASM client makes.
    ///
    /// Overriding this lets such a backend register without blocking: send the
    /// request and return `true` immediately, then invoke exactly one of
    /// @p onRegistered / @p onError once the reply arrives, on the backend's own
    /// thread (unless the backend is destroyed first, in which case neither
    /// fires). `Bridge::registerHandler()` prefers this path when it is
    /// available and falls back to the synchronous `registerModelWithContext`
    /// otherwise, so every backend that has not opted in (every backend as of
    /// this writing, other than `QtWebSocketBackend`) is unaffected.
    ///
    /// The default implementation offers no async path and returns `false`
    /// without calling either callback — the caller (`Bridge::registerHandler`)
    /// falls back to `registerModelWithContext` in that case, so a backend
    /// with no override behaves synchronously.
    ///
    /// @note **Threading contract, shared by all four `*Async` hooks: the
    ///       callback's thread must not be able to run `~Bridge` concurrently.**
    ///       Three of the four `Bridge` continuations behind these hooks —
    ///       `attachHandlerAsync`, `ensureBoundAsync` and `assignHandlerPrimary`
    ///       in `core/bridge.hpp` — test `CallbackToken::active()` and then
    ///       dereference `this`. Those are two steps, so a `~Bridge` that
    ///       completes between them is morph#486's use-after-free.
    ///       (`registerHandlerImpl`'s callback is the exception: it holds
    ///       `detail::BridgeLifetime` across its whole touch of `this`, so it
    ///       does not depend on this contract.)
    ///
    ///       Those three cannot take that same gate. It makes `~Bridge` *block*
    ///       for the gated span, and each span acquires `_attachMtx` — which
    ///       the synchronous `Bridge::attachHandler` holds across a full
    ///       `attachModel` round trip, unbounded on a wire backend. What closes
    ///       the window instead is delivery on the thread that owns the
    ///       `Bridge`. `QtWebSocketBackend` — the only backend in the tree
    ///       overriding any of these — satisfies that by construction: it must
    ///       itself be used from the Qt event loop thread
    ///       (`qt/qt_websocket_backend.hpp`), and every *reply-driven* callback
    ///       fires from `onTextMessage` on that same thread, so check and use
    ///       cannot straddle a destructor. Its two non-reply paths do not weaken
    ///       this: a disconnected or no-op dispatch invokes the callback inline,
    ///       still inside the caller's own frame (which `detail::parkIfInFrame`
    ///       exists to handle), and `cancelPending` fires the remainder from
    ///       `~Bridge` itself — which is not a *concurrent* destructor. **A backend that delivers these callbacks on
    ///       a thread the `Bridge`'s owner does not control breaks this contract
    ///       and reopens that use-after-free** — it is a contract break, not a
    ///       latent race to be discovered. See morph#489 and
    ///       docs/spec/concurrency_and_lifetimes.md.
    ///
    /// @note Scope: only `Bridge::registerHandler()`'s plain (non-shared)
    ///       registration path — a `BridgeHandler`'s initial construction —
    ///       uses this. Shared/keyed registration has its own opt-in async
    ///       pair, `registerModelSharedAsync`/`attachModelAsync` below,
    ///       preferred by `Bridge::ensureBoundAsync`/`attachHandlerAsync`.
    ///       The re-registration `switchBackend()`/the reconnect handler
    ///       perform after a backend swap remains synchronous; see
    ///       docs/spec/core/backend.md.
    /// @param typeId       String type-id of the model to instantiate.
    /// @param factory      Callable that constructs the `IModelHolder` (local path only).
    /// @param contextKey   Stable identity of the new instance; empty if none.
    /// @param onRegistered Invoked with the assigned `ModelId` on success.
    /// @param onError      Invoked with a diagnostic message on failure.
    /// @return `true` if this backend accepted the request and will invoke
    ///         exactly one callback later; `false` if it has no async path
    ///         (neither callback is invoked in that case).
    virtual bool registerModelAsync(const std::string& typeId,
                                    std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
                                    std::string_view contextKey,
                                    std::function<void(::morph::exec::detail::ModelId)> onRegistered,
                                    std::function<void(const std::string&)> onError) {
        (void)typeId;
        (void)factory;
        (void)contextKey;
        (void)onRegistered;
        (void)onError;
        return false;
    }

    /// @brief Optional non-blocking counterpart to `registerModelShared`.
    ///
    /// Same rationale and shape as `registerModelAsync` (see its doc comment
    /// immediately above): `registerModelShared`'s synchronous default
    /// implementations block the calling thread until a reply arrives, which
    /// aborts a WASM main thread the moment a shared/keyed handler makes its
    /// first attach. A backend that overrides this sends the request and
    /// returns `true` immediately, then invokes exactly one of
    /// @p onRegistered / @p onError once the reply arrives, on the backend's
    /// own thread (unless the backend is destroyed first, in which case
    /// neither fires) — subject to `registerModelAsync`'s threading contract,
    /// which applies here unchanged: that thread must not be able to run
    /// `~Bridge` concurrently.
    ///
    /// The default implementation offers no async path and returns `false`
    /// without calling either callback — the caller (`Bridge::ensureBoundAsync`)
    /// falls back to the synchronous `registerModelShared` in that case, so a
    /// backend with no override behaves synchronously.
    ///
    /// @param typeId     String type-id of the model.
    /// @param factory    Callable that constructs the `IModelHolder` (local path only).
    /// @param identity   Entity key for the action log plus the directory primary key.
    /// @param onRegistered Invoked with the assigned/attached `ModelId` on success.
    /// @param onError    Invoked with a diagnostic message on failure.
    /// @return `true` if this backend accepted the request and will invoke
    ///         exactly one callback later; `false` if it has no async path.
    // NOLINTBEGIN(performance-unnecessary-value-param) — by-value matches
    // registerModelAsync's signature exactly; overriding backends move the
    // callbacks into their pending-reply map.
    virtual bool registerModelSharedAsync(
        const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
        InstanceIdentity identity, std::function<void(::morph::exec::detail::ModelId)> onRegistered,
        std::function<void(const std::string&)> onError) {
        (void)typeId;
        (void)factory;
        (void)identity;
        (void)onRegistered;
        (void)onError;
        return false;
    }
    // NOLINTEND(performance-unnecessary-value-param)

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

    /// @brief Optional non-blocking counterpart to `attachModel`.
    ///
    /// Same rationale and shape as `registerModelSharedAsync` immediately
    /// above (itself mirroring `registerModelAsync`) — see that doc comment
    /// for the full opt-in/fallback contract, and `registerModelAsync`'s for
    /// the threading contract the callback's delivery thread must satisfy.
    ///
    /// @note Unlike the synchronous `attachModel` default above, this method
    ///       does *not* release @p current itself: an overriding backend is
    ///       behind a wire protocol, whose single `attach` request re-points
    ///       server-side and therefore leaves nothing to deregister — exactly
    ///       the division of responsibility `QtWebSocketBackend::attachModel`
    ///       already follows for a non-empty `identity.primary`. @p current is
    ///       passed so that request can name what it is re-pointing from.
    ///
    /// @param typeId     String type-id of the model.
    /// @param factory    Callable that constructs the `IModelHolder` (local path only).
    /// @param identity   Entity key for the action log plus the directory primary key.
    /// @param current    Instance currently held, or `ModelId{0}` if none.
    /// @param onRegistered Invoked with the `ModelId` now attached to, on success.
    /// @param onError    Invoked with a diagnostic message on failure.
    /// @return `true` if this backend accepted the request and will invoke
    ///         exactly one callback later; `false` if it has no async path.
    // NOLINTBEGIN(performance-unnecessary-value-param) — see registerModelSharedAsync above.
    virtual bool attachModelAsync(const std::string& typeId,
                                  std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
                                  InstanceIdentity identity, ::morph::exec::detail::ModelId current,
                                  std::function<void(::morph::exec::detail::ModelId)> onRegistered,
                                  std::function<void(const std::string&)> onError) {
        (void)typeId;
        (void)factory;
        (void)identity;
        (void)current;
        (void)onRegistered;
        (void)onError;
        return false;
    }
    // NOLINTEND(performance-unnecessary-value-param)

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

    /// @brief Optional non-blocking counterpart to `assignPrimary`.
    ///
    /// `assignPrimary` is synchronous: a backend whose promote step requires a
    /// round-trip (a socket backend) can only implement that by blocking the
    /// calling thread until the reply arrives — `QtWebSocketBackend` does this
    /// via a nested `QEventLoop`, exactly the blocking shape
    /// `registerModelAsync` exists to let a caller avoid for the bind step.
    /// The promote step (the second half of a shared handler's result-keyed
    /// `execute()`, after `Bridge::ensureBound`) reaches the same nested loop
    /// on a single-threaded WASM build, which cannot spin one at all.
    ///
    /// Overriding this lets such a backend promote without blocking: send the
    /// request and return `true` immediately, then invoke exactly one of
    /// @p onRegistered / @p onError once the reply arrives, on the backend's
    /// own thread (unless the backend is destroyed first, in which case
    /// neither fires) — subject to `registerModelAsync`'s threading contract,
    /// which applies here unchanged: that thread must not be able to run
    /// `~Bridge` concurrently. `Bridge::assignHandlerPrimary` prefers this path when
    /// it is available and falls back to the synchronous `assignPrimary`
    /// otherwise, so every backend that has not opted in (every backend as of
    /// this writing, other than `QtWebSocketBackend`) is unaffected.
    ///
    /// The default implementation offers no async path and returns `false`
    /// without calling either callback — the caller (`Bridge::assignHandlerPrimary`)
    /// falls back to `assignPrimary` in that case, so a backend with no
    /// override behaves synchronously.
    ///
    /// @param mid          Live instance to promote.
    /// @param typeId       Model type id — the directory's first key component.
    /// @param primary      Canonical string encoding of the key to file it under.
    /// @param onRegistered Invoked with @p mid (echoed back, for symmetry with
    ///                     `registerModelAsync`'s callback shape) on success —
    ///                     including the no-op cases `assignPrimary` documents
    ///                     (empty primary, dead `mid`, key already taken, `mid`
    ///                     already keyed differently): those are not backend
    ///                     failures, so they resolve `onRegistered` exactly as
    ///                     the synchronous path returns normally for them.
    /// @param onError      Invoked with a diagnostic message on a genuine
    ///                     backend/transport failure.
    /// @return `true` if this backend accepted the request and will invoke
    ///         exactly one callback later; `false` if it has no async path
    ///         (neither callback is invoked in that case).
    virtual bool assignPrimaryAsync(::morph::exec::detail::ModelId mid, const std::string& typeId,
                                    std::string_view primary,
                                    std::function<void(::morph::exec::detail::ModelId)> onRegistered,
                                    std::function<void(const std::string&)> onError) {
        (void)mid;
        (void)typeId;
        (void)primary;
        (void)onRegistered;
        (void)onError;
        return false;
    }

    // ── The structural registration surface ──────────────────────────────
    //
    // `bindModel`/`promoteModel` are what the five verbs above become once the
    // continuation stops being optional. Two differences carry the whole
    // change, and both are visible in the signature rather than in a comment:
    //
    //   1. **The continuation is not opt-in.** There is no `bool` saying "I
    //      have no async path, call the other one" — so no call site carries a
    //      second path, and a backend cannot be *half* migrated. A blocking
    //      backend satisfies the surface unchanged through the default
    //      implementations below, or without blocking the caller at all
    //      through `SynchronousBackendAdapter`.
    //
    //      What morph#593 established is that removing *that* bool also
    //      removed something else the call site needed and that is not the
    //      same question: whether the thread that called `bindModel` is
    //      allowed to wait for the continuation it just registered. Two
    //      shipped backends return an unsettled `Completion` and give opposite
    //      answers (`SocketBackend`: yes, its I/O thread settles it;
    //      `QtWebSocketBackend` under `asyncRegistrationEnabled`: no, waiting
    //      deadlocks the event loop the reply arrives on). `bindWaitPolicy()`
    //      below restores exactly that one bit and nothing else — it never
    //      selects a verb, so the "second path" the removed bool created does
    //      not come back with it.
    //
    //   2. **The delivery thread is a parameter.** `Completion<T>` posts its
    //      handlers to the executor it was built with, so the continuation runs
    //      where @p cbExec says and nowhere else — the backend does not choose.
    //      That is the whole of the threading contract the four `*Async` verbs
    //      could only state in prose (see `registerModelAsync`'s `@note`, and
    //      docs/spec/core/backend.md, "The structural registration surface").
    //      It does not by itself make a `~Bridge` race impossible: it moves the
    //      choice of delivery thread from fifteen backend implementors, none of
    //      which knows what the caller's teardown looks like, to the one caller
    //      that does. `cbExec` is a reference precisely so that "deliver
    //      nowhere" cannot be expressed — a null executor would silently drop
    //      every continuation (see `Completion`'s constructor).
    //
    // Retiring the five older verbs is morph#571; until then both surfaces
    // exist and nothing in the tree has moved off the old one.

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
    /// non-blocking path (morph#568's `QtWebSocketBackend`) overrides this and
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
    /// returned `Completion` from this thread, echoing @p request's `mid` back
    /// exactly as `assignPrimaryAsync` documents — including for every
    /// documented no-op case (empty primary, dead `mid`, key already taken,
    /// `mid` already keyed differently), which are not backend failures and
    /// therefore resolve rather than reject.
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
    /// not own or from the caller's own event loop. Only a synchronous entry
    /// point that must hand back a *bound* instance asks it —
    /// `Bridge::registerHandlerImpl` is the single caller in the framework; the
    /// asynchronous entry points never wait and never consult it.
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
/// it is why migrating the rest of morph#522's set is a set of migrations
/// rather than a set of rewrites: a backend with no non-blocking path of its
/// own (every backend in the tree except `QtWebSocketBackend`) reaches the new
/// surface by being wrapped, not by being rewritten.
///
/// @par What it does and does not change
/// The wrapped backend still blocks — nothing here makes a nested event loop
/// or a socket round trip non-blocking. What changes is **which thread pays**:
/// the blocking call happens on the executor passed at construction, so the
/// caller's thread returns from `bindModel` immediately with an unresolved
/// `Completion`. A single-threaded WASM main thread has no such executor to
/// offer and is therefore not what this adapter is for; that case needs a
/// backend with a genuinely non-blocking path (morph#568).
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
/// callers. `~SynchronousBackendAdapter` waits for any in-flight control call
/// to finish (`StrandExecutor`'s destructor does), so a reply can never land
/// in a destroyed adapter; the executor must therefore still be running tasks
/// when this adapter is destroyed, on the same terms as `StrandExecutor`'s own
/// `base` (see docs/spec/concurrency_and_lifetimes.md, "Destruction ordering").
///
/// @par Reconnect handlers
/// A control call issued from a reconnect handler runs on the strand, never on
/// the wrapped backend's transport thread, so a backend whose reply can only
/// be delivered by the thread that is running the reconnect handler does not
/// wait on itself. Whether that is enough to settle `SocketBackend`'s
/// documented reconnect hazard is morph#569's question, not a claim made here.
// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions)
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
    // A decorator has to forward every verb it does not reshape, including the
    // four legacy `*Async` twins: wrapping a backend that *does* have a
    // non-blocking path must not quietly take it away.

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
    /// @param typeId       String type-id of the model to instantiate.
    /// @param factory      Callable that constructs the `IModelHolder`.
    /// @param contextKey   Stable identity of the new instance; empty if none.
    /// @param onRegistered Invoked with the assigned `ModelId` on success.
    /// @param onError      Invoked with a diagnostic message on failure.
    /// @return Whatever the wrapped backend returned.
    bool registerModelAsync(const std::string& typeId,
                            std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
                            std::string_view contextKey,
                            std::function<void(::morph::exec::detail::ModelId)> onRegistered,
                            std::function<void(const std::string&)> onError) override {
        return _inner->registerModelAsync(typeId, std::move(factory), contextKey, std::move(onRegistered),
                                          std::move(onError));
    }

    // NOLINTBEGIN(performance-unnecessary-value-param) — the overridden
    // signatures take these by value; see `IBackend::registerModelSharedAsync`.
    /// @brief Forwards to the wrapped backend.
    /// @param typeId       String type-id of the model.
    /// @param factory      Callable that constructs the `IModelHolder`.
    /// @param identity     Entity key for the action log plus the directory primary key.
    /// @param onRegistered Invoked with the assigned/attached `ModelId` on success.
    /// @param onError      Invoked with a diagnostic message on failure.
    /// @return Whatever the wrapped backend returned.
    bool registerModelSharedAsync(const std::string& typeId,
                                  std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
                                  detail::InstanceIdentity identity,
                                  std::function<void(::morph::exec::detail::ModelId)> onRegistered,
                                  std::function<void(const std::string&)> onError) override {
        return _inner->registerModelSharedAsync(typeId, std::move(factory), identity, std::move(onRegistered),
                                                std::move(onError));
    }

    /// @brief Forwards to the wrapped backend.
    /// @param typeId       String type-id of the model.
    /// @param factory      Callable that constructs the `IModelHolder`.
    /// @param identity     Entity key for the action log plus the directory primary key.
    /// @param current      Instance currently held, or `ModelId{0}` if none.
    /// @param onRegistered Invoked with the `ModelId` now attached to, on success.
    /// @param onError      Invoked with a diagnostic message on failure.
    /// @return Whatever the wrapped backend returned.
    bool attachModelAsync(const std::string& typeId,
                          std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
                          detail::InstanceIdentity identity, ::morph::exec::detail::ModelId current,
                          std::function<void(::morph::exec::detail::ModelId)> onRegistered,
                          std::function<void(const std::string&)> onError) override {
        return _inner->attachModelAsync(typeId, std::move(factory), identity, current, std::move(onRegistered),
                                        std::move(onError));
    }
    // NOLINTEND(performance-unnecessary-value-param)

    /// @brief Forwards to the wrapped backend.
    /// @param mid     Live instance to promote.
    /// @param typeId  Model type id — the directory's first key component.
    /// @param primary Canonical string encoding of the key to file it under.
    void assignPrimary(::morph::exec::detail::ModelId mid, const std::string& typeId,
                       std::string_view primary) override {
        _inner->assignPrimary(mid, typeId, primary);
    }

    /// @brief Forwards to the wrapped backend.
    /// @param mid          Live instance to promote.
    /// @param typeId       Model type id — the directory's first key component.
    /// @param primary      Canonical string encoding of the key to file it under.
    /// @param onRegistered Invoked with @p mid on success.
    /// @param onError      Invoked with a diagnostic message on failure.
    /// @return Whatever the wrapped backend returned.
    bool assignPrimaryAsync(::morph::exec::detail::ModelId mid, const std::string& typeId, std::string_view primary,
                            std::function<void(::morph::exec::detail::ModelId)> onRegistered,
                            std::function<void(const std::string&)> onError) override {
        return _inner->assignPrimaryAsync(mid, typeId, primary, std::move(onRegistered), std::move(onError));
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
    /// `_inner->cancelPending` reaches nothing of it — before morph#619 a bind
    /// dispatched through this adapter went on to resolve **successfully**
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
    /// **What this does not do:** a task already queued on `_control` still
    /// runs its blocking control call against the wrapped backend after this
    /// returns. The caller is told the bind was cancelled, but the registration
    /// may still happen — morph#636, since stopping it needs the task to check
    /// before calling `op()`, not the promise to be settled after it.
    /// @param exc Exception delivered to every still-pending completion, this
    ///            adapter's own and then the wrapped backend's.
    void cancelPending(const std::exception_ptr& exc) override {
        std::vector<std::weak_ptr<BindPromise>> snapshot;
        {
            std::scoped_lock const lock{_pendingMtx};
            snapshot.swap(_pending);
            _compactAt = kPendingCompactFloor;
        }
        for (auto& weak : snapshot) {
            if (auto promise = weak.lock()) {
                promise->reject(exc);
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
        auto shared = std::make_shared<Settled::Promise>(std::move(promise));
        // Tracked *before* the post, not after: a `cancelPending` that lands in
        // between would otherwise find an empty list and leave a completion
        // that is genuinely pending uncancelled. Rejecting a promise whose task
        // has not started yet is safe — the task's own `resolve` then finds the
        // state ready and returns (morph#619).
        trackPending(shared);
        _control.post(kControlStrand, [shared, op = std::move(op)]() mutable {
            try {
                shared->resolve(op());
            } catch (...) {
                shared->reject(std::current_exception());
            }
        });
        return std::move(completion);
    }

    /// @brief Records @p promise as cancellable until its task settles it.
    ///
    /// The strand task holds the only `shared_ptr` to the promise, so an entry
    /// here expires exactly when that task is destroyed — "still pending" needs
    /// no separate bookkeeping and no erase on the success path.
    ///
    /// Swept on the same amortised schedule as `LocalBackend::trackPending`
    /// (morph#528): dead entries are reclaimed only when the list reaches
    /// `_compactAt`, which each sweep re-arms at twice the surviving count, so
    /// the per-dispatch cost is O(1) and the list stays bounded at twice the
    /// live count plus the floor. Control calls are serialised onto one strand,
    /// so in practice the live count is one and the floor is never reached;
    /// without the sweep the list would still grow without bound on an adapter
    /// whose `cancelPending` is never called.
    /// @param promise Promise to reject if `cancelPending` runs before its task settles it.
    void trackPending(const std::shared_ptr<BindPromise>& promise) {
        std::scoped_lock const lock{_pendingMtx};
        if (_pending.size() >= _compactAt) {
            std::erase_if(_pending, [](const auto& weak) { return weak.expired(); });
            _compactAt = std::max(kPendingCompactFloor, _pending.size() * 2);
        }
        _pending.emplace_back(promise);
    }

    /// @brief The single strand key every control call shares, so they run one
    ///        at a time. Not a real model id: this `StrandExecutor` is private
    ///        to the adapter and shares no key space with any backend's own.
    static constexpr ::morph::exec::detail::ModelId kControlStrand{1};

    /// @brief Smallest size at which `trackPending` sweeps; see `LocalBackend`'s.
    static constexpr std::size_t kPendingCompactFloor = 32;

    std::shared_ptr<detail::IBackend> _inner;
    ::morph::exec::detail::StrandExecutor _control;
    mutable std::mutex _pendingMtx;
    // Every `bindModel`/`promoteModel` promise handed to a `_control` task and
    // not yet settled by it. Weak, so a settled task's promise drops out on its
    // own; guarded by `_pendingMtx`, because `cancelPending` is called from
    // `Bridge`'s thread while `dispatch` runs on whichever thread called it.
    std::vector<std::weak_ptr<BindPromise>> _pending;
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
    ///                   work. Borrowed, not owned: it is handed to this
    ///                   backend's `StrandExecutor`, so it must outlive the
    ///                   backend *and* keep running tasks until teardown
    ///                   completes — destroying it first deadlocks (see
    ///                   `docs/spec/concurrency_and_lifetimes.md`, "Destruction
    ///                   ordering").
    explicit LocalBackend(::morph::exec::IExecutor& workerPool MORPH_LIFETIMEBOUND) : _strand{workerPool} {}

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
            _strand.post(modelId, [h = std::move(holder)]() mutable { h->onBackendChanged(); });
        }
    }

    /// @brief Schedules `call.localOp` on the model's strand and returns a `Completion`.
    ///
    /// The completion resolves with the opaque result on the strand thread and
    /// the callbacks are delivered via @p cbExec.
    ///
    /// @param mid    Target model id.
    /// @param call   Bundled action; `localOp` is the only field used here.
    /// @param cbExec Executor for delivering callbacks.
    /// @return Completion that will carry the result or an exception.
    ::morph::async::Completion<std::shared_ptr<void>> execute(::morph::exec::detail::ModelId mid,
                                                              detail::ActionCall call,
                                                              ::morph::exec::IExecutor* cbExec) override {
        auto compState = std::make_shared<::morph::async::detail::CompletionState<std::shared_ptr<void>>>();
        ::morph::async::Completion<std::shared_ptr<void>> comp{compState, cbExec};

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
            compState->setException(
                std::make_exception_ptr(std::runtime_error("model not found: id=" + std::to_string(mid.v))));
            return comp;
        }
        trackPending(compState);
        auto localOp = std::move(call.localOp);
        auto session = std::move(call.session);
        auto modelTypeId = std::move(call.modelTypeId);
        auto actionTypeId = std::move(call.actionTypeId);
        // Captured by shared_ptr, never by raw `this`: see the Global
        // Constraints note on `~StrandExecutor`'s member-destruction-order
        // subtlety. A shared_ptr copy has its own lifetime, independent of
        // LocalBackend's, so it stays valid even if the backend is torn down
        // while this task is still queued or running. `hydration` follows the
        // same rule and may be null (a private instance has no entry).
        auto inFlightCounter = _inFlight;
        auto const inFlightAfterInc = inFlightCounter->fetch_add(1, std::memory_order_relaxed) + 1;
        ::morph::observe::detail::emitMetric(::morph::observe::Metric::executeInFlight,
                                             static_cast<double>(inFlightAfterInc));
        _strand.post(mid, [localOp = std::move(localOp), holder = std::move(holder), compState,
                           session = std::move(session), modelTypeId = std::move(modelTypeId),
                           actionTypeId = std::move(actionTypeId), inFlightCounter, hydration]() mutable {
            auto const start = std::chrono::steady_clock::now();
            auto const spanId = ::morph::observe::detail::beginSpan(session.requestId, modelTypeId, actionTypeId);
            bool ok = false;
            // Resolve `compState` only after every metric and `endSpan` below are
            // recorded — nothing synchronizes a `.then()`/`.onError()` callback
            // (delivered via `cbExec`, which may run inline/synchronously) with
            // anything after `setValue`/`setException` returns, so resolving first
            // would let the caller observe completion before these metrics are
            // emitted. This is a real race, not just a theoretical one.
            std::shared_ptr<void> value;
            std::exception_ptr error;
            try {
                ::morph::session::detail::ScopedContext const scoped{session};
                value = localOp(*holder);
                ok = true;
            } catch (...) {
                error = std::current_exception();
            }
            // Settle hydration the moment the first action's outcome is known —
            // before `endSpan`, before any metric, and before the `Completion`
            // resolves. Each of those hands control to host code that is free
            // to attach to this instance's key, and an attacher reaching the
            // directory while the outcome is known but unrecorded is handed an
            // instance whose first action has already failed — exactly what
            // docs/spec/core/shared_instances.md's Failure modes section says
            // must not happen. Only the *first* action settles it; `settle` is
            // a single compare-exchange and ignores every later call.
            if (hydration) {
                hydration->settle(ok);
            }
            ::morph::observe::detail::endSpan(spanId, ok);
            auto const elapsedMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            std::array<std::pair<std::string_view, std::string_view>, 2> const tags{
                {{"modelType", modelTypeId}, {"actionType", actionTypeId}}};
            ::morph::observe::detail::emitMetric(::morph::observe::Metric::executeLatencyMs, elapsedMs, tags);
            if (!ok) {
                ::morph::observe::detail::emitMetric(::morph::observe::Metric::executeErrors, 1.0, tags);
            }
            auto const inFlightAfterDec = inFlightCounter->fetch_sub(1, std::memory_order_relaxed) - 1;
            ::morph::observe::detail::emitMetric(::morph::observe::Metric::executeInFlight,
                                                 static_cast<double>(inFlightAfterDec));
            // Resolve last: the Completion is still settled exactly once, only
            // its position relative to the now-recorded instrumentation moved.
            if (ok) {
                compState->setValue(std::move(value));
            } else {
                compState->setException(error);
            }
        });
        return comp;
    }

    /// @brief Resolves every still-pending completion this backend produced with @p exc.
    /// @param exc Exception delivered to every pending completion's error sink.
    void cancelPending(const std::exception_ptr& exc) override {
        std::vector<std::weak_ptr<::morph::async::detail::CompletionState<std::shared_ptr<void>>>> snapshot;
        {
            std::scoped_lock const lock{_pendingMtx};
            snapshot.swap(_pending);
            _compactAt = kPendingCompactFloor;
        }
        for (auto& weak : snapshot) {
            if (auto state = weak.lock()) {
                state->setException(exc);
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
    /// Before morph#528 this swept on *every* append, so admitting one call with
    /// `n` in flight cost `n` atomic `weak_ptr::expired()` loads under
    /// `_pendingMtx` and a burst of `n` cost O(n²) — measured at 362ms of pure
    /// admission time for 32k queued executes against one slow model, against
    /// 7.6ms without the sweep.
    ///
    /// Purely a cost change: `cancelPending` still sees every live state,
    /// because a dead entry is one whose `CompletionState` is already gone and
    /// which `cancelPending`'s `weak.lock()` has always skipped. Carrying dead
    /// entries for longer changes nothing it observes.
    /// @param state Completion state to track until it expires or is cancelled.
    void trackPending(const std::shared_ptr<::morph::async::detail::CompletionState<std::shared_ptr<void>>>& state) {
        std::scoped_lock const lock{_pendingMtx};
        if (_pending.size() >= _compactAt) {
            std::erase_if(_pending, [](const auto& weak) { return weak.expired(); });
            _compactAt = std::max(kPendingCompactFloor, _pending.size() * 2);
        }
        _pending.emplace_back(state);
    }

    ::morph::exec::detail::StrandExecutor _strand;
    std::mutex _regMtx;
    // Every live instance, private and shared alike, plus the shared-instance
    // directory over them — holder, attach count, directory key and hydration
    // state as one record per instance rather than five parallel ModelId-keyed
    // maps held in lockstep by convention (morph#523). Guarded by `_regMtx`;
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
    std::vector<std::weak_ptr<::morph::async::detail::CompletionState<std::shared_ptr<void>>>> _pending;
    // Size at which `trackPending` next sweeps `_pending` for expired entries;
    // re-armed at twice the surviving count after each sweep. Guarded by
    // `_pendingMtx` along with `_pending` itself. See `trackPending` (morph#528).
    std::size_t _compactAt = kPendingCompactFloor;
    // Concurrent in-flight executes, for the executeInFlight metric. A
    // shared_ptr (not a plain atomic member) so strand tasks hold their own
    // reference instead of capturing `this` — see execute()'s comment and the
    // Global Constraints note on ~StrandExecutor's destruction order.
    std::shared_ptr<std::atomic<std::size_t>> _inFlight = std::make_shared<std::atomic<std::size_t>>(0);
};

}  // namespace morph::backend
