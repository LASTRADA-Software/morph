// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../journal/action_log.hpp"
#include "../session/session.hpp"
#include "backend.hpp"
#include "logger.hpp"
#include "wire.hpp"

namespace morph::backend {

/// @brief Opt-in, connection-agnostic resource limits enforced by `RemoteServer`.
///
/// Every field defaults to `0`, meaning "unbounded" — installing no policy (or
/// installing a default-constructed one) reproduces today's behavior exactly.
/// Install via `RemoteServer::setLimitPolicy()`. See `docs/spec/core/backend.md`.
struct LimitPolicy {
    /// @brief Max wall-clock time a single `execute` may take before the server
    ///        sends an `err "timeout"` reply and discards the eventual strand
    ///        result. `0` = no timeout (today's behavior).
    ///
    /// The model action itself is never interrupted — it keeps running to
    /// completion on its strand. This bounds the *caller's wait*, not the model.
    std::chrono::milliseconds executeTimeout{0};

    /// @brief Max models this `RemoteServer` will hold live at once, across all
    ///        callers. A `register` beyond this cap replies `err "too many
    ///        models"`. `0` = unbounded (today's behavior).
    std::size_t maxLiveModels{0};

    /// @brief Max concurrent in-flight `execute` calls this server will accept
    ///        before replying `err "server busy"` instead of dispatching.
    ///        `0` = unbounded (today's behavior).
    std::size_t maxInFlightExecutes{0};
};

namespace detail {

/// @brief Background scheduler that invokes a callback once after a delay, unless cancelled first.
///
/// `RemoteServer` is transport-agnostic and its `IExecutor` has no delayed-post
/// primitive, so a single dedicated thread per instance tracks pending
/// deadlines and fires callbacks when they elapse. Used to enforce
/// `LimitPolicy::executeTimeout` — see `docs/spec/core/backend.md`.
class TimeoutScheduler {
public:
    /// @brief Opaque identifier for one scheduled callback.
    using Handle = std::uint64_t;

    /// @brief Starts the background thread.
    TimeoutScheduler() : _thread{[this] { run(); }} {}

    /// @brief Stops the background thread and joins it.
    ~TimeoutScheduler() {
        {
            std::scoped_lock const lock{_mtx};
            _stop = true;
        }
        _cv.notify_all();
        _thread.join();
    }

    TimeoutScheduler(const TimeoutScheduler&) = delete;
    TimeoutScheduler& operator=(const TimeoutScheduler&) = delete;
    TimeoutScheduler(TimeoutScheduler&&) = delete;
    TimeoutScheduler& operator=(TimeoutScheduler&&) = delete;

    /// @brief Schedules @p callback to run after @p delay on the scheduler's
    ///        background thread, unless cancelled first via `cancel()`.
    /// @param delay    Time to wait before firing.
    /// @param callback Invoked on the scheduler thread if not cancelled in time.
    ///                 Exceptions it throws are logged and swallowed.
    /// @return Handle usable with `cancel()`.
    Handle schedule(std::chrono::milliseconds delay, std::function<void()> callback) {
        auto const deadline = std::chrono::steady_clock::now() + delay;
        std::scoped_lock const lock{_mtx};
        Handle const handle = ++_nextHandle;
        auto iter = _entries.emplace(deadline, Entry{handle, std::move(callback)});
        _index[handle] = iter;
        _cv.notify_all();
        return handle;
    }

    /// @brief Cancels a previously scheduled callback immediately.
    ///
    /// If @p handle has not fired yet, its entry (and anything its callback
    /// captured) is erased right away — the caller does not have to wait for
    /// the original deadline for that memory to be released. A no-op if
    /// @p handle already fired or was already cancelled.
    /// @param handle Handle returned by a prior `schedule()` call.
    void cancel(Handle handle) {
        std::scoped_lock const lock{_mtx};
        auto found = _index.find(handle);
        if (found == _index.end()) {
            return;
        }
        _entries.erase(found->second);
        _index.erase(found);
    }

private:
    struct Entry {
        Handle handle;
        std::function<void()> callback;
    };

    void run() {
        std::unique_lock lock{_mtx};
        while (!_stop) {
            if (_entries.empty()) {
                _cv.wait(lock);
                continue;
            }
            auto const nextDeadline = _entries.begin()->first;
            _cv.wait_until(lock, nextDeadline);
            if (_stop) {
                break;
            }
            auto now = std::chrono::steady_clock::now();
            while (!_entries.empty() && _entries.begin()->first <= now) {
                auto iter = _entries.begin();
                Entry entry = std::move(iter->second);
                _index.erase(entry.handle);
                _entries.erase(iter);
                lock.unlock();
                try {
                    entry.callback();
                } catch (const std::exception& exc) {
                    ::morph::log::logError("[timeout-scheduler] callback threw: " + std::string{exc.what()});
                } catch (...) {
                    ::morph::log::logError("[timeout-scheduler] callback threw unknown exception");
                }
                lock.lock();
                now = std::chrono::steady_clock::now();
            }
        }
    }

    std::mutex _mtx;
    std::condition_variable _cv;
    std::multimap<std::chrono::steady_clock::time_point, Entry> _entries;
    std::unordered_map<Handle, std::multimap<std::chrono::steady_clock::time_point, Entry>::iterator> _index;
    Handle _nextHandle{0};
    bool _stop{false};
    std::thread _thread;
};

/// @brief Keyed 64-bit bijection that turns a monotonic counter into an
///        unguessable, non-sequential id.
///
/// Implements a 4-round Feistel network over two 32-bit halves. A Feistel
/// network is a bijection over its full domain for *any* round function —
/// that is what guarantees `RemoteServer` never hands out the same id twice
/// for two different counter values. What makes the permutation *opaque*
/// rather than merely "scrambled" is that each round's mixing function folds
/// in a secret round key drawn once, at construction, from
/// `std::random_device`: an *unkeyed* public mixing function would be
/// invertible by anyone reading the source, letting an attacker who observes
/// one id recover the counter and predict the next; the secret per-round keys
/// prevent that without needing the mixing function itself to be secret.
///
/// This is a self-contained reference construction (no external crypto
/// dependency), in the same spirit as the hand-rolled HMAC-SHA256 in
/// `session_auth.hpp`: adequate for the stated defence-in-depth goal (opaque
/// ids are not the authorization boundary — `IAuthorizer::authorizeInstance`
/// is), not a cryptographically-audited primitive.
class OpaqueIdGenerator {
public:
    /// @brief Draws four independent 32-bit round keys from `std::random_device`.
    OpaqueIdGenerator() {
        std::random_device rd;
        for (auto& key : _roundKeys) {
            key = static_cast<uint32_t>(rd());
        }
    }

    /// @brief Applies the keyed permutation to @p counter.
    ///
    /// Bijective over the full 64-bit domain: distinct @p counter values
    /// always produce distinct results (the Feistel structure guarantees
    /// this regardless of the round function), so a monotonically
    /// increasing, non-repeating @p counter can never yield a collision.
    /// @param counter Monotonic input, e.g. from an atomic counter.
    /// @return A 64-bit value that is a bijective function of @p counter.
    [[nodiscard]] uint64_t permute(uint64_t counter) const noexcept {
        auto lo = static_cast<uint32_t>(counter & 0xffffffffULL);
        auto hi = static_cast<uint32_t>(counter >> 32);
        for (const uint32_t roundKey : _roundKeys) {
            const uint32_t nextHi = lo;
            lo = hi ^ mix(lo, roundKey);
            hi = nextHi;
        }
        return (static_cast<uint64_t>(hi) << 32) | static_cast<uint64_t>(lo);
    }

private:
    /// @brief Keyed avalanche mix (fmix32-style) used as the Feistel round function.
    /// @param half Current 32-bit half being folded into the other half.
    /// @param key  This round's secret key.
    /// @return A well-mixed 32-bit value depending non-linearly on both @p half and @p key.
    [[nodiscard]] static uint32_t mix(uint32_t half, uint32_t key) noexcept {
        uint32_t val = half ^ key;
        val ^= val >> 16;
        val *= 0x7feb352dU;
        val ^= val >> 15;
        val *= 0x846ca68bU;
        val ^= val >> 16;
        return val;
    }

    std::array<uint32_t, 4> _roundKeys{};
};

}  // namespace detail

/// @brief Opaque id a transport uses to scope model registrations to one
///        connection so `RemoteServer::closeConnection` can reclaim them.
///
/// `0` is reserved and means *unscoped* — the meaning today's two-argument
/// `RemoteServer::handle()`/`handleInline()` calls always have. Non-zero
/// values are minted by `RemoteServer::openConnection()`.
using ConnectionId = std::uint64_t;

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

    /// @brief Like `handle(msg, reply)`, but additionally attributes any
    ///        `register` decoded from @p msg to a connection scope.
    ///
    /// A `register` processed under a non-zero @p cid records the new model in
    /// that connection's scope, so a later `closeConnection(cid)` reclaims it.
    /// Passing `cid == 0` is exactly the unscoped, two-argument `handle()` —
    /// nothing is recorded and nothing is ever cleaned up automatically.
    ///
    /// Thread-safe. Safe to call before the previous call's reply has been delivered.
    ///
    /// @param msg   JSON-encoded `morph::wire::Envelope` (via `wire::encode`).
    /// @param reply Callback invoked with the JSON-encoded reply envelope.
    /// @param cid   Connection scope to attribute a `register` in @p msg to;
    ///              `0` means unscoped.
    void handle(std::string msg, std::function<void(std::string)> reply, ConnectionId cid) {
        auto self = shared_from_this();
        _pool.post([self, msg = std::move(msg), reply = std::move(reply), cid]() mutable {
            self->dispatchMessage(msg, reply, cid);
        });
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

    /// @brief Opens a new connection scope and returns its id.
    ///
    /// Call once per accepted transport connection (e.g. from a WebSocket
    /// server's "new connection" callback). The returned id is never `0`, so
    /// it can always be distinguished from the reserved "unscoped" value. Pass
    /// it to the scoped `handle(msg, reply, cid)` overload for every message
    /// received on that connection, and to `closeConnection(cid)` once the
    /// connection is gone.
    ///
    /// Thread-safe.
    /// @return A fresh, non-zero `ConnectionId`.
    [[nodiscard]] ConnectionId openConnection() {
        ConnectionId const cid{_nextConnectionId.fetch_add(1) + 1};
        std::scoped_lock const lock{_regMtx};
        _connectionScopes.try_emplace(cid);
        return cid;
    }

    /// @brief Reclaims every model still registered under @p cid, then drops the scope.
    ///
    /// Call once the transport observes the connection is gone (disconnect,
    /// close, error). Erases every surviving model in `cid`'s scope from the
    /// registry exactly as an explicit `deregister` would (so a later
    /// `execute` against one of those ids replies `err "model not found"`),
    /// then drops the scope itself.
    ///
    /// Idempotent: `cid == 0`, an unknown `cid`, or a `cid` already closed is a
    /// no-op. Deliberately does **not** consult `IAuthorizer` — this is the
    /// server's own housekeeping in reaction to a transport-level event, not
    /// an action attributable to any caller (see docs/spec/core/backend.md).
    ///
    /// Safe to call while a model in the scope has an `execute` in flight: the
    /// strand task already holds its own `shared_ptr` to the model holder, so
    /// erasing the registry entry here only prevents *new* lookups (see
    /// docs/spec/concurrency_and_lifetimes.md).
    ///
    /// Thread-safe.
    /// @param cid Connection scope to close, as returned by `openConnection()`.
    void closeConnection(ConnectionId cid) {
        if (cid == 0) {
            return;
        }
        std::scoped_lock const lock{_regMtx};
        auto scopeIter = _connectionScopes.find(cid);
        if (scopeIter == _connectionScopes.end()) {
            return;
        }
        for (const auto& mid : scopeIter->second) {
            _models.erase(mid);
            _owners.erase(mid);
            _modelConnection.erase(mid);
        }
        _connectionScopes.erase(scopeIter);
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

    /// @brief Installs @p policy, consulted by every subsequent `register` and
    ///        `execute`. Thread-safe.
    ///
    /// All-zero fields (the default-constructed value) mean "unbounded" — the
    /// server behaves exactly as it did before this method was ever called.
    /// @param policy Resource limits to apply from this call onward.
    void setLimitPolicy(LimitPolicy policy) {
        std::scoped_lock const lock{_limitsMtx};
        _limits = policy;
        if (_limits.executeTimeout.count() > 0 && !_timeoutScheduler) {
            _timeoutScheduler = std::make_unique<detail::TimeoutScheduler>();
        }
    }

    /// @brief Sets the inclusive protocol-version range this server advertises
    ///        in reply to a `"hello"` envelope.
    ///
    /// Defaults to `{::morph::wire::kProtocolVersion, ::morph::wire::kProtocolVersion}`
    /// — this build's single supported version. Widen the range when a future
    /// `kProtocolVersion` bump must keep serving older clients through their
    /// deprecation window (see docs/spec/core/wire.md, "Action-evolution policy").
    /// Thread-safe.
    ///
    /// @param min Oldest protocol version this server accepts.
    /// @param max Newest protocol version this server accepts.
    /// @throws std::invalid_argument if `min > max`.
    void setSupportedVersionRange(std::uint32_t min, std::uint32_t max) {
        if (min > max) {
            throw std::invalid_argument("setSupportedVersionRange: min must not exceed max");
        }
        _minVersion.store(min);
        _maxVersion.store(max);
    }

private:
    void dispatchMessage(const std::string& msg, std::function<void(std::string)>& reply, ConnectionId cid = 0) {
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
                LimitPolicy limits;
                {
                    std::scoped_lock const lock{_limitsMtx};
                    limits = _limits;
                }
                if (limits.maxLiveModels != 0) {
                    std::scoped_lock const lock{_regMtx};
                    if (_models.size() >= limits.maxLiveModels) {
                        reply(::morph::wire::encode(::morph::wire::makeErr("too many models", env.callId)));
                        return;
                    }
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
                ::morph::exec::detail::ModelId const mid{nextOpaqueId()};
                {
                    std::scoped_lock const lock{_regMtx};
                    _models[mid] = std::move(holder);
                    _owners[mid] = std::move(env.session.principal);
                    // A non-zero cid attributes the new instance to that
                    // connection's scope, next to _models/_owners under the
                    // same lock so scope membership can never desync from
                    // instance existence. cid == 0 (the unscoped default)
                    // records nothing, matching today's behavior byte-for-byte.
                    if (cid != 0) {
                        _connectionScopes[cid].insert(mid);
                        _modelConnection[mid] = cid;
                    }
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
                    // Keep the connection scope's membership set in sync: an
                    // explicit wire deregister removes the id from its scope
                    // too, so a later closeConnection never double-erases it.
                    if (auto connIter = _modelConnection.find(mid); connIter != _modelConnection.end()) {
                        if (auto scopeIter = _connectionScopes.find(connIter->second);
                            scopeIter != _connectionScopes.end()) {
                            scopeIter->second.erase(mid);
                        }
                        _modelConnection.erase(connIter);
                    }
                }
                reply(::morph::wire::encode(::morph::wire::makeOk(env.callId)));
            } else if (env.kind == "execute") {
                dispatchExecute(std::move(env), reply);
            } else if (env.kind == "hello") {
                const std::uint32_t minV = _minVersion.load();
                const std::uint32_t maxV = _maxVersion.load();
                if (env.protocolVersion < minV || env.protocolVersion > maxV) {
                    reply(::morph::wire::encode(::morph::wire::makeErr("protocol version unsupported", env.callId)));
                } else {
                    std::string body;
                    (void)glz::write_json(::morph::wire::ProtocolRange{.min = minV, .max = maxV}, body);
                    reply(::morph::wire::encode(::morph::wire::makeOk(env.callId, std::move(body))));
                }
            } else {
                reply(::morph::wire::encode(::morph::wire::makeErr("unknown envelope kind: " + env.kind, env.callId)));
            }
        } catch (const std::exception& exc) {
            reply(::morph::wire::encode(::morph::wire::makeErr(exc.what(), env.callId)));
        }
    }

    void dispatchExecute(::morph::wire::Envelope env, std::function<void(std::string)> reply) {
        LimitPolicy limits;
        {
            std::scoped_lock const lock{_limitsMtx};
            limits = _limits;
        }
        if (limits.maxInFlightExecutes != 0 &&
            _inFlightExecutes.load(std::memory_order_relaxed) >= limits.maxInFlightExecutes) {
            reply(::morph::wire::encode(::morph::wire::makeErr("server busy", env.callId)));
            return;
        }
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
        _inFlightExecutes.fetch_add(1, std::memory_order_relaxed);
        auto self = shared_from_this();
        std::uint64_t const callId = env.callId;

        // `finished` fires the caller's `reply` exactly once — whichever of the
        // timeout path or the strand path gets there first — and always
        // decrements the in-flight counter exactly once, regardless of which
        // path won. This preserves handle()'s reply-exactly-once contract even
        // though two independent paths can now race to resolve the same call.
        auto finished = std::make_shared<std::atomic_flag>();
        auto replySlot = std::make_shared<std::function<void(std::string)>>(std::move(reply));
        auto complete = [self, finished, replySlot](std::string msg) {
            if (!finished->test_and_set()) {
                self->_inFlightExecutes.fetch_sub(1, std::memory_order_relaxed);
                (*replySlot)(std::move(msg));
            }
        };

        detail::TimeoutScheduler::Handle timeoutHandle{};
        if (limits.executeTimeout.count() > 0) {
            std::scoped_lock const lock{_limitsMtx};
            if (_timeoutScheduler) {
                timeoutHandle = _timeoutScheduler->schedule(limits.executeTimeout, [complete, callId]() mutable {
                    complete(::morph::wire::encode(::morph::wire::makeErr("timeout", callId)));
                });
            }
        }

        _strand.post(mid, [self, env = std::move(env), holder = std::move(holder), complete, timeoutHandle]() mutable {
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
                {
                    std::scoped_lock const lock{self->_limitsMtx};
                    if (self->_timeoutScheduler) {
                        self->_timeoutScheduler->cancel(timeoutHandle);
                    }
                }
                complete(::morph::wire::encode(::morph::wire::makeOk(env.callId, std::move(result))));
            } catch (const std::exception& exc) {
                {
                    std::scoped_lock const lock{self->_limitsMtx};
                    if (self->_timeoutScheduler) {
                        self->_timeoutScheduler->cancel(timeoutHandle);
                    }
                }
                complete(::morph::wire::encode(::morph::wire::makeErr(exc.what(), env.callId)));
            }
        });
    }

    /// @brief Returns the next opaque model id.
    ///
    /// Runs an internal monotonic counter through `detail::OpaqueIdGenerator`,
    /// so distinct calls never collide (the permutation is a bijection) but
    /// the returned values are not sequential. Skips the one counter value
    /// (if any) whose permutation is exactly `0` — `ModelId`'s reserved
    /// "unbound" sentinel (see `strand.hpp`) — which is possible in principle
    /// (the permutation is a bijection over the *entire* 64-bit domain, so
    /// exactly one input maps to `0`) but has probability 1-in-2^64 for a
    /// random key; guarded defensively rather than ever handed out.
    /// @return A freshly-generated, non-zero, opaque `ModelId`.
    [[nodiscard]] ::morph::exec::detail::ModelId nextOpaqueId() {
        uint64_t id = 0;
        do {
            id = _idGen.permute(_nextId.fetch_add(1) + 1);
        } while (id == 0);
        return ::morph::exec::detail::ModelId{id};
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
    // Connection-scope bookkeeping (opt-in; see openConnection/closeConnection
    // and the scoped handle(msg, reply, cid) overload). Guarded by _regMtx —
    // the same lock as _models/_owners — so scope membership can never desync
    // from instance existence.
    std::unordered_map<ConnectionId,
                       std::unordered_set<::morph::exec::detail::ModelId, ::morph::exec::detail::ModelIdHash>>
        _connectionScopes;
    // Owning connection recorded per scoped instance; absent means unscoped
    // (registered via the two-argument handle()/handleInline()).
    std::unordered_map<::morph::exec::detail::ModelId, ConnectionId, ::morph::exec::detail::ModelIdHash>
        _modelConnection;
    std::atomic<uint64_t> _nextId{0};
    std::atomic<uint64_t> _nextConnectionId{0};
    std::atomic<std::uint32_t> _minVersion{::morph::wire::kProtocolVersion};
    std::atomic<std::uint32_t> _maxVersion{::morph::wire::kProtocolVersion};
    detail::OpaqueIdGenerator _idGen;
    std::mutex _logProviderMtx;
    LogProvider _logProvider;
    std::mutex _limitsMtx;
    LimitPolicy _limits;
    std::atomic<std::size_t> _inFlightExecutes{0};
    std::unique_ptr<detail::TimeoutScheduler> _timeoutScheduler;
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
                               } else if (reply.message == "timeout") {
                                   throw TimeoutError{};
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
