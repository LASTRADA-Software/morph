// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../journal/action_log.hpp"
#include "../session/session.hpp"
#include "backend.hpp"
#include "wire.hpp"

namespace morph::backend {

/// @brief Server-side message handler that owns model instances and dispatches actions.
///
/// `RemoteServer` receives JSON envelopes (`morph::wire::Envelope`) from any
/// transport (WebSocket, in-process simulation, …) and executes the corresponding
/// model operations via an `ActionDispatcher`. Authorization is delegated to an
/// `IAuthorizer` that defaults to allow-all.
///
/// @par Heap allocation requirement
/// `RemoteServer` **must** be heap-allocated via `std::make_shared`. `handle()`
/// captures `shared_from_this()` to prevent use-after-free when the worker pool
/// outlives the server object.
///
/// @par Wire format
/// All requests and replies are encoded as `morph::wire::Envelope` JSON. See
/// `wire.hpp` for the field semantics. The `kind` field is the discriminator.
class RemoteServer : public std::enable_shared_from_this<RemoteServer> {
public:
    /// @brief Constructs a server backed by @p workerPool with allow-all authorization.
    ///
    /// @param workerPool Pool used to process messages asynchronously.
    /// @param dispatcher Action dispatcher; defaults to the process-level singleton.
    /// @param registry   Model factory registry; defaults to the process-level singleton.
    explicit RemoteServer(
        ::morph::exec::IExecutor& workerPool,
        ::morph::model::detail::ActionDispatcher& dispatcher = ::morph::model::detail::defaultDispatcher(),
        ::morph::model::detail::ModelRegistryFactory& registry = ::morph::model::detail::defaultRegistry())
        : _pool{workerPool},
          _strand{workerPool},
          _dispatcher{dispatcher},
          _registry{registry},
          _authorizer{::morph::session::allowAllAuthorizer()} {}

    /// @brief Constructs a server with a custom authorizer.
    ///
    /// @param workerPool Pool used to process messages asynchronously.
    /// @param authorizer Authorizer consulted for every `execute` envelope.
    /// @param dispatcher Action dispatcher; defaults to the process-level singleton.
    /// @param registry   Model factory registry; defaults to the process-level singleton.
    RemoteServer(::morph::exec::IExecutor& workerPool, std::shared_ptr<::morph::session::IAuthorizer> authorizer,
                 ::morph::model::detail::ActionDispatcher& dispatcher = ::morph::model::detail::defaultDispatcher(),
                 ::morph::model::detail::ModelRegistryFactory& registry = ::morph::model::detail::defaultRegistry())
        : _pool{workerPool},
          _strand{workerPool},
          _dispatcher{dispatcher},
          _registry{registry},
          _authorizer{std::move(authorizer)} {
        if (!_authorizer) {
            _authorizer = ::morph::session::allowAllAuthorizer();
        }
    }

    /// @brief Asynchronously processes a JSON `Envelope` and calls @p reply with the response.
    ///
    /// The message is dispatched to the worker pool. @p reply is called exactly
    /// once from the pool thread when processing completes.
    ///
    /// Thread-safe. Safe to call before the previous call's reply has been delivered.
    ///
    /// @param msg   JSON-encoded `morph::wire::Envelope` (via `wire::encode`).
    /// @param reply Callback invoked with the JSON-encoded reply envelope.
    void handle(std::string msg, std::function<void(std::string)> reply) {
        auto self = shared_from_this();
        _pool.post(
            [self, msg = std::move(msg), reply = std::move(reply)]() mutable { self->dispatchMessage(msg, reply); });
    }

    /// @brief Synchronously processes a JSON `Envelope` on the calling thread and returns the reply.
    ///
    /// Equivalent to `handle()` but never posts to the worker pool, so it is safe
    /// to call from a thread that *is* the worker pool — for example, from a
    /// `BridgeHandler` constructor invoked from inside an action handler.
    ///
    /// Only safe for control messages (`register`, `deregister`). An `execute`
    /// envelope posts to the strand and produces its reply asynchronously, after
    /// this synchronous call has already returned and destroyed the local reply
    /// buffer the deferred callback would write into. To keep that from becoming a
    /// dangling write, `execute` is rejected up front with an `err` reply.
    ///
    /// @param msg JSON-encoded `morph::wire::Envelope` (via `wire::encode`).
    /// @return JSON-encoded reply envelope.
    std::string handleInline(const std::string& msg) {
        try {
            auto env = ::morph::wire::decode(msg);
            if (env.kind == "execute") {
                return ::morph::wire::encode(::morph::wire::makeErr(
                    "handleInline does not support execute (reply is asynchronous)", env.callId));
            }
        } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
            // Malformed input: fall through so dispatchMessage produces the
            // canonical decode-error reply (avoids duplicating that path here).
        }
        std::string reply;
        std::function<void(std::string)> capture = [&reply](std::string out) noexcept { reply = std::move(out); };
        dispatchMessage(msg, capture);
        return reply;
    }

    /// @brief Callable that supplies the action log to attach to a newly
    ///        registered instance, given its model type and `contextKey`.
    ///
    /// Return `nullptr` to register the instance with no log attached (e.g. for
    /// model types or context keys the host app doesn't want journaled).
    using LogProvider = std::function<std::shared_ptr<::morph::journal::IActionLog>(std::string_view modelType,
                                                                                    std::string_view contextKey)>;

    /// @brief Installs @p provider, consulted on every `register` envelope whose
    ///        `contextKey` is non-empty.
    ///
    /// This is what closes the gap `IModelHolder::attachActionLog` leaves open
    /// for remote topologies: `RemoteServer` owns the actual model instances for
    /// every remote/simulated-remote client, so it is the only place that can
    /// attach a log to them. Pass `nullptr` to remove a previously installed
    /// provider (new registrations get no log). Thread-safe.
    /// @param provider Callable invoked synchronously while handling `register`.
    void setLogProvider(LogProvider provider) {
        std::scoped_lock const lock{_logProviderMtx};
        _logProvider = std::move(provider);
    }

private:
    void dispatchMessage(const std::string& msg, std::function<void(std::string)>& reply) {
        ::morph::wire::Envelope env;
        try {
            env = ::morph::wire::decode(msg);
        } catch (const std::exception& exc) {
            reply(::morph::wire::encode(::morph::wire::makeErr(exc.what())));
            return;
        }
        try {
            if (env.kind == "register") {
                if (env.typeId.empty()) {
                    throw std::runtime_error("register requires a typeId");
                }
                // Authenticate the caller and make the verified identity
                // authoritative, exactly as dispatchExecute does for execute: a
                // verifying authorizer's returned principal overwrites
                // env.session.principal; a non-authenticating authorizer
                // (including allow-all) clears it, so the register decision
                // below — and the owner recorded from it — never key on the
                // client's unverified claim.
                if (auto verified = _authorizer->authenticate(env.session)) {
                    env.session.principal = std::move(*verified);
                } else {
                    env.session.principal.clear();
                }
                // Bound *who may create* an instance. The default hook allows
                // all, so an unconfigured server registers any known type
                // exactly as before; a deployer opts into gating registration
                // by overriding authorizeRegister. No instance is constructed
                // on denial.
                if (!_authorizer->authorizeRegister(env.session, env.typeId)) {
                    reply(::morph::wire::encode(::morph::wire::makeErr("unauthorized", env.callId)));
                    return;
                }
                auto holder = _registry.create(env.typeId);
                if (!env.contextKey.empty()) {
                    LogProvider provider;
                    {
                        std::scoped_lock const lock{_logProviderMtx};
                        provider = _logProvider;
                    }
                    if (provider) {
                        if (auto log = provider(env.typeId, env.contextKey)) {
                            holder->attachActionLog(std::move(log), env.contextKey);
                        }
                    }
                }
                // Record the owner principal for per-instance authorization:
                // env.session's principal is already the verified identity
                // stamped above (empty if the authorizer does not
                // authenticate), never the client's raw claim. This is what
                // lets `authorizeInstance` later deny a different principal.
                ::morph::exec::detail::ModelId const mid{_nextId.fetch_add(1) + 1};
                {
                    std::scoped_lock const lock{_regMtx};
                    _models[mid] = std::move(holder);
                    _owners[mid] = std::move(env.session.principal);
                }
                reply(::morph::wire::encode(::morph::wire::makeOk(env.callId, {}, mid.v)));
            } else if (env.kind == "deregister") {
                ::morph::exec::detail::ModelId const mid{env.modelId};
                // Per-instance authorization also gates deregister: consult the
                // hook with the recorded owner before destroying the instance.
                // The default hook allows all, so unconfigured behaviour is
                // unchanged; an ownership-enforcing authorizer can reject a
                // caller tearing down an instance it does not own.
                std::string owner;
                bool known = false;
                {
                    std::scoped_lock const lock{_regMtx};
                    auto iter = _owners.find(mid);
                    if (iter != _owners.end()) {
                        owner = iter->second;
                        known = true;
                    }
                }
                if (known && !_authorizer->authorizeInstance(env.session, {}, {}, mid.v, owner)) {
                    reply(::morph::wire::encode(::morph::wire::makeErr("unauthorized", env.callId)));
                    return;
                }
                {
                    std::scoped_lock const lock{_regMtx};
                    _models.erase(mid);
                    _owners.erase(mid);
                }
                reply(::morph::wire::encode(::morph::wire::makeOk(env.callId)));
            } else if (env.kind == "execute") {
                dispatchExecute(std::move(env), reply);
            } else {
                reply(::morph::wire::encode(::morph::wire::makeErr("unknown envelope kind: " + env.kind, env.callId)));
            }
        } catch (const std::exception& exc) {
            reply(::morph::wire::encode(::morph::wire::makeErr(exc.what(), env.callId)));
        }
    }

    void dispatchExecute(::morph::wire::Envelope env, std::function<void(std::string)> reply) {
        if (!_authorizer->authorize(env.session, env.modelType, env.actionType)) {
            reply(::morph::wire::encode(::morph::wire::makeErr("unauthorized", env.callId)));
            return;
        }
        // Make the identity authoritative. A verifying authorizer returns the
        // principal it extracted from a valid token; we stamp it so model code
        // reading session::current()->principal can trust it. If authenticate()
        // returns nullopt the authorizer cannot vouch for the caller, so we CLEAR
        // the client-asserted principal rather than passing it through unverified.
        // This closes two holes: (1) the TOCTOU window where a token that passed
        // authorize() expires before authenticate() (worst case is now an empty
        // principal, never the attacker's claim), and (2) an authorize-only or
        // allow-all authorizer that never authenticates (the model never sees an
        // untrusted principal as authoritative). See docs/spec/security.md.
        if (auto verified = _authorizer->authenticate(env.session)) {
            env.session.principal = std::move(*verified);
        } else {
            env.session.principal.clear();
        }
        ::morph::exec::detail::ModelId const mid{env.modelId};
        std::shared_ptr<::morph::model::detail::IModelHolder> holder;
        std::string owner;
        bool known = false;
        {
            std::scoped_lock const lock{_regMtx};
            auto iter = _models.find(mid);
            if (iter != _models.end()) {
                holder = iter->second;
                if (auto ownerIter = _owners.find(mid); ownerIter != _owners.end()) {
                    owner = ownerIter->second;
                    known = true;
                }
            }
        }
        if (!holder) {
            reply(::morph::wire::encode(::morph::wire::makeErr("model not found", env.callId)));
            return;
        }
        // Per-instance (row-level) authorization. `authorize` above only saw the
        // model *type*; this consults the optional ownership hook with the target
        // instance id and its recorded owner. The default hook allows all, so
        // behaviour is unchanged unless an authorizer overrides it. env.session
        // now carries the verified principal (stamped just above), so an
        // ownership authorizer compares the recorded owner against it.
        if (known && !_authorizer->authorizeInstance(env.session, env.modelType, env.actionType, mid.v, owner)) {
            reply(::morph::wire::encode(::morph::wire::makeErr("unauthorized", env.callId)));
            return;
        }
        // Capture a strong self-reference so the server (and therefore
        // `_dispatcher`, which is a reference member) stays alive until this
        // strand task runs and delivers its reply. `handle()`'s task only holds
        // `self` until it enqueues onto the strand; without this capture the last
        // external shared_ptr could drop before the strand task executes, leaving
        // `_dispatcher` dangling (use-after-free) or the reply silently lost so a
        // client Completion hangs forever. See docs/spec/concurrency_and_lifetimes.md.
        auto self = shared_from_this();
        _strand.post(mid, [self = std::move(self), env = std::move(env), holder = std::move(holder),
                           reply = std::move(reply)]() mutable {
            try {
                ::morph::session::detail::ScopedContext const scoped{env.session};
                // `dispatch` (registry.hpp, ActionDispatcher::registerAction's runner)
                // now throws morph::model::ValidationError when the decoded action
                // fails ActionValidator<Action>::ready(...), before Model::execute
                // runs. No special-casing is needed here: ValidationError derives
                // from std::runtime_error, so it is caught by the handler below and
                // turned into an ordinary `err` reply carrying its message and
                // callId, exactly like any other dispatch failure. See
                // docs/spec/core/registry.md.
                auto result = self->_dispatcher.dispatch(env.modelType, env.actionType, *holder, env.body);
                reply(::morph::wire::encode(::morph::wire::makeOk(env.callId, std::move(result))));
            } catch (const std::exception& exc) {
                reply(::morph::wire::encode(::morph::wire::makeErr(exc.what(), env.callId)));
            }
        });
    }

    ::morph::exec::IExecutor& _pool;
    ::morph::exec::detail::StrandExecutor _strand;
    ::morph::model::detail::ActionDispatcher& _dispatcher;
    ::morph::model::detail::ModelRegistryFactory& _registry;
    std::shared_ptr<::morph::session::IAuthorizer> _authorizer;
    std::mutex _regMtx;
    std::unordered_map<::morph::exec::detail::ModelId, std::shared_ptr<::morph::model::detail::IModelHolder>,
                       ::morph::exec::detail::ModelIdHash>
        _models;
    // Owner principal recorded per instance at register time, consulted by
    // IAuthorizer::authorizeInstance on execute/deregister. Guarded by _regMtx
    // (same lock as _models); empty string means "no recorded owner".
    std::unordered_map<::morph::exec::detail::ModelId, std::string, ::morph::exec::detail::ModelIdHash> _owners;
    std::atomic<uint64_t> _nextId{0};
    std::mutex _logProviderMtx;
    LogProvider _logProvider;
};

/// @brief `IBackend` adapter that routes all calls through a `RemoteServer` as
///        wire `Envelope` messages.
///
/// Intended for testing and in-process simulation of remote execution.
/// `registerModel()` and `deregisterModel()` are processed inline on the calling
/// thread via `RemoteServer::handleInline`. `execute()` is asynchronous: it sends
/// the message through `RemoteServer::handle` and resolves the returned
/// `Completion` when the reply arrives (there is no `std::promise` and no
/// blocking wait).
class SimulatedRemoteBackend : public detail::IBackend {
public:
    /// @brief Constructs the backend targeting @p server.
    /// @param server The `RemoteServer` instance to forward calls to.
    explicit SimulatedRemoteBackend(RemoteServer& server) : _server{server} {}

    /// @brief Registers the model type on the server and returns its assigned id.
    ///
    /// Processed inline on the calling thread (no pool round-trip), so it is safe
    /// to call from any thread including a worker in the same pool that backs the
    /// `RemoteServer`. The @p factory argument is ignored — model construction is
    /// delegated to the server's `ModelRegistryFactory`.
    ///
    /// @param typeId String type-id sent in the `register` message.
    /// @return `ModelId` assigned by the server.
    /// @throws std::runtime_error if the server replies with an error.
    ::morph::exec::detail::ModelId registerModel(
        const std::string& typeId,
        std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> /*factory*/) override {
        return registerModelWithContext(typeId, {}, {});
    }

    /// @brief Registers the model type on the server, carrying @p contextKey across
    ///        the wire so the server's `RemoteServer::LogProvider` (if configured)
    ///        can attach an action log to the instance it creates.
    ///
    /// @p factory is still ignored — model construction is delegated to the
    /// server's `ModelRegistryFactory`, same as `registerModel()`.
    /// @param typeId     String type-id sent in the `register` message.
    /// @param contextKey Stable identity of the new instance; empty if none.
    /// @return `ModelId` assigned by the server.
    /// @throws std::runtime_error if the server replies with an error.
    ::morph::exec::detail::ModelId registerModelWithContext(
        const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> /*factory*/,
        std::string_view contextKey) override {
        auto reply = ::morph::wire::decode(
            _server.handleInline(::morph::wire::encode(::morph::wire::makeRegister(typeId, std::string{contextKey}))));
        if (reply.kind == "ok") {
            return ::morph::exec::detail::ModelId{reply.modelId};
        }
        throw std::runtime_error("register failed: " + reply.message);
    }

    /// @brief Deregisters the model on the server. Processed inline; safe from any thread.
    /// @param mid Id of the model to deregister.
    void deregisterModel(::morph::exec::detail::ModelId mid) override {
        (void)_server.handleInline(::morph::wire::encode(::morph::wire::makeDeregister(mid.v)));
    }

    /// @brief Serialises the action, sends it to the server, and returns a `Completion`.
    ///
    /// The `Completion` resolves when the server's reply is received and
    /// deserialized. Callbacks are posted via @p cbExec. The session attached to
    /// the call (via `Bridge::setDefaultSession()` or the per-call API) is
    /// serialised into the envelope.
    ///
    /// @param mid    Target model id on the server.
    /// @param call   Bundled action; `serializeAction` and `deserializeResult` are used.
    /// @param cbExec Executor for delivering the completion callbacks.
    /// @return Completion that resolves with the deserialized result or an error.
    ::morph::async::Completion<std::shared_ptr<void>> execute(::morph::exec::detail::ModelId mid,
                                                              detail::ActionCall call,
                                                              ::morph::exec::IExecutor* cbExec) override {
        auto state = std::make_shared<::morph::async::detail::CompletionState<std::shared_ptr<void>>>();
        ::morph::async::Completion<std::shared_ptr<void>> comp{state, cbExec};
        trackPending(state);

        ::morph::wire::Envelope env;
        env.kind = "execute";
        env.modelId = mid.v;
        env.modelType = call.modelTypeId;
        env.actionType = call.actionTypeId;
        env.body = call.serializeAction();
        env.session = std::move(call.session);
        auto deser = std::move(call.deserializeResult);

        _server.handle(::morph::wire::encode(env),
                       [state, deser = std::move(deser)](const std::string& replyJson) mutable {
                           try {
                               auto reply = ::morph::wire::decode(replyJson);
                               if (reply.kind == "ok") {
                                   state->setValue(deser(reply.body));
                               } else {
                                   throw std::runtime_error(reply.message.empty() ? "malformed reply" : reply.message);
                               }
                           } catch (...) {
                               state->setException(std::current_exception());
                           }
                       });
        return comp;
    }

    /// @brief No-op — models live in `RemoteServer`, not locally.
    void notifyBackendChanged() override {}

    /// @brief Resolves every still-pending completion this backend produced with @p exc.
    /// @param exc Exception delivered to every pending completion's error sink.
    void cancelPending(const std::exception_ptr& exc) override {
        std::vector<std::weak_ptr<::morph::async::detail::CompletionState<std::shared_ptr<void>>>> snapshot;
        {
            std::scoped_lock const lock{_pendingMtx};
            snapshot.swap(_pending);
        }
        for (auto& weak : snapshot) {
            if (auto state = weak.lock()) {
                state->setException(exc);
            }
        }
    }

private:
    void trackPending(const std::shared_ptr<::morph::async::detail::CompletionState<std::shared_ptr<void>>>& state) {
        std::scoped_lock const lock{_pendingMtx};
        std::erase_if(_pending, [](const auto& weak) { return weak.expired(); });
        _pending.emplace_back(state);
    }

    RemoteServer& _server;
    std::mutex _pendingMtx;
    std::vector<std::weak_ptr<::morph::async::detail::CompletionState<std::shared_ptr<void>>>> _pending;
};

}  // namespace morph::backend
