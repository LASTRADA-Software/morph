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
#include "observability.hpp"
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
            // std::random_device::result_type is unsigned int on this platform's
            // standard library, so the cast below is a no-op here -- but the
            // standard does not guarantee that, so it stays for portability to a
            // standard library where result_type is wider than uint32_t.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wuseless-cast"
#endif
            key = static_cast<uint32_t>(rd());
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
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

/// @brief Snapshot of a `RemoteServer`'s current health.
struct HealthStatus {
    /// @brief `true` if the server currently accepts and dispatches new work.
    bool ready;
    /// @brief Number of models currently registered on the server.
    std::size_t liveModels;
    /// @brief Number of executes currently dispatched but not yet replied.
    std::size_t inFlight;
};

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
        for (const auto& [mid, refs] : scopeIter->second) {
            // Release exactly as many references as this connection held. A
            // shared instance another connection is still attached to survives;
            // a private one (count 1, no directory entry) is erased outright,
            // which is byte-for-byte the previous behaviour.
            for (std::size_t idx = 0; idx < refs; ++idx) {
                releaseInstanceLocked(mid);
            }
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

    /// @brief Snapshots the server's current health. Cheap; safe from any thread.
    /// @return Current `HealthStatus` (`liveModels` from the registry, `inFlight`
    ///         from the same counter the `executeInFlight` metric reads).
    [[nodiscard]] HealthStatus health() const {
        std::size_t liveModels = 0;
        {
            std::scoped_lock const lock{_regMtx};
            liveModels = _models.size();
        }
        return HealthStatus{
            .ready = _ready.load(std::memory_order_relaxed),
            .liveModels = liveModels,
            .inFlight = _inFlightExecutes.load(std::memory_order_relaxed),
        };
    }

    /// @brief Installs @p handler, invoked immediately with the current
    ///        `health()` snapshot, and again whenever readiness changes.
    ///
    /// Thread-safe. Pass `nullptr` to remove a previously installed handler
    /// (clearing does not itself invoke anything). `beginShutdown()` is
    /// currently the only internal path that flips `HealthStatus::ready` to
    /// `false`; it re-invokes this handler (if installed) with the
    /// post-shutdown snapshot. A deployment's transport (e.g.
    /// `QtWebSocketServer`) can expose `health()`/this handler over an
    /// HTTP/probe endpoint; `morph` does not embed an HTTP server.
    /// @param handler Callback invoked with the current `HealthStatus`.
    void setHealthHandler(std::function<void(const HealthStatus&)> handler) {
        std::function<void(const HealthStatus&)> toCall;
        {
            std::scoped_lock const lock{_healthMtx};
            _healthHandler = std::move(handler);
            toCall = _healthHandler;
        }
        if (toCall) {
            toCall(health());
        }
    }

    /// @brief Enters shutdown: from now on, `register` and `execute` envelopes
    ///        are rejected with `err "server shutting down"`. `deregister` is
    ///        still served so clients can tear down cleanly.
    ///
    /// Idempotent — safe to call more than once, and safe to call while
    /// `handle()`/`handleInline()` calls are concurrently in flight on other
    /// threads. There is no way back: a restarted service constructs a fresh
    /// `RemoteServer` rather than un-shutting-down this one.
    ///
    /// Also flips `health().ready` to `false` and, if a handler is installed
    /// via `setHealthHandler()`, re-invokes it with the post-shutdown
    /// snapshot — the same "invoked again whenever readiness changes"
    /// contract `setHealthHandler` documents. This is what lets an
    /// orchestrator stop routing to this server while `drainedWithin()`'s
    /// drain runs.
    void beginShutdown() {
        _shuttingDown.store(true, std::memory_order_release);
        _ready.store(false, std::memory_order_release);
        std::function<void(const HealthStatus&)> handler;
        {
            std::scoped_lock const lock{_healthMtx};
            handler = _healthHandler;
        }
        if (handler) {
            handler(health());
        }
    }

    /// @brief Blocks until every in-flight `execute` has delivered its reply,
    ///        or @p deadline elapses.
    ///
    /// "In-flight" is one counter (`_inFlightExecutes`), incremented when
    /// `dispatchExecute` admits a call for dispatch (right before posting to
    /// the model's strand) and decremented right before its reply is sent, on
    /// every resolving path (`ok`, `err`, or a `LimitPolicy::executeTimeout`
    /// firing first) — the same state `LimitPolicy::maxInFlightExecutes` (if
    /// configured) gates and `health()`'s `inFlight` field reads, shared
    /// rather than double-counted. Waits on a condition variable signalled
    /// when the counter reaches zero — not a busy poll. Safe to call from any
    /// thread, independently of `beginShutdown()` (it only observes the
    /// counter; it does not itself stop new work from arriving).
    /// @param deadline Maximum time to wait.
    /// @return `true` if the in-flight count reached zero before @p deadline
    ///         elapsed; `false` on timeout.
    [[nodiscard]] bool drainedWithin(std::chrono::milliseconds deadline) {
        std::unique_lock lock{_drainMtx};
        return _drainCv.wait_for(lock, deadline,
                                 [this] { return _inFlightExecutes.load(std::memory_order_acquire) == 0; });
    }

private:
    /// @brief Directory key: the `(model type id, primary)` pair an instance is filed under.
    using DirectoryKey = std::pair<std::string, std::string>;

    /// @brief Releases one reference to @p mid, destroying it at zero. Caller holds `_regMtx`.
    ///
    /// A private instance has no `_attachCount` entry and is erased outright —
    /// byte-for-byte the pre-sharing behaviour. A shared instance is erased, and
    /// removed from the directory, only when its last attachment goes away, so
    /// one client's `deregister` or dropped connection never tears an instance
    /// out from under another client still using it.
    /// @param mid Instance to release.
    /// @return `true` if this call destroyed the instance.
    bool releaseInstanceLocked(::morph::exec::detail::ModelId mid) {
        if (auto refIter = _attachCount.find(mid); refIter != _attachCount.end()) {
            refIter->second -= 1;
            if (refIter->second > 0) {
                return false;
            }
            _attachCount.erase(refIter);
            if (auto keyIter = _sharedKeyOf.find(mid); keyIter != _sharedKeyOf.end()) {
                _directory.erase(keyIter->second);
                _sharedKeyOf.erase(keyIter);
            }
        }
        _models.erase(mid);
        _owners.erase(mid);
        _firstActionPending.erase(mid);
        _poisoned.erase(mid);
        return true;
    }

    /// @brief Drops one of @p cid's references to @p mid, then releases the instance.
    ///        Caller holds `_regMtx`.
    /// @param mid Instance to release.
    /// @param cid Connection whose reference is being dropped; `0` for unscoped.
    void releaseScopedLocked(::morph::exec::detail::ModelId mid, ConnectionId cid) {
        if (cid != 0) {
            if (auto scopeIter = _connectionScopes.find(cid); scopeIter != _connectionScopes.end()) {
                if (auto refIter = scopeIter->second.find(mid); refIter != scopeIter->second.end()) {
                    refIter->second -= 1;
                    if (refIter->second == 0) {
                        scopeIter->second.erase(refIter);
                    }
                }
            }
        }
        releaseInstanceLocked(mid);
    }

    /// @brief Records a new attachment of @p mid to @p cid. Caller holds `_regMtx`.
    /// @param mid Instance being attached.
    /// @param cid Connection attaching it; `0` for unscoped (records nothing).
    /// @return `false` if @p cid's scope was already closed, in which case nothing was recorded.
    bool noteScopeAttachLocked(::morph::exec::detail::ModelId mid, ConnectionId cid) {
        if (cid == 0) {
            return true;
        }
        auto scopeIter = _connectionScopes.find(cid);
        if (scopeIter == _connectionScopes.end()) {
            return false;
        }
        scopeIter->second[mid] += 1;
        return true;
    }

    /// @brief Attaches a configured `LogProvider`'s log to a freshly created holder.
    /// @param holder Newly created instance.
    /// @param env    Envelope carrying `typeId` and `contextKey`.
    void attachLogIfConfigured(::morph::model::detail::IModelHolder& holder, const ::morph::wire::Envelope& env) {
        if (env.contextKey.empty()) {
            return;
        }
        LogProvider provider;
        {
            std::scoped_lock const lock{_logProviderMtx};
            provider = _logProvider;
        }
        if (provider) {
            if (auto log = provider(env.typeId, env.contextKey)) {
                holder.attachActionLog(std::move(log), env.contextKey);
            }
        }
    }

    /// @brief Attaches to an already-live directory entry, if there is one. Caller holds `_regMtx`.
    ///
    /// Factored out because `acquireSharedInstance` checks the directory twice —
    /// once on entry, and again under the insert lock after building a holder
    /// outside it, in case a concurrent request for the same key won the race.
    /// The second check is by nature almost never taken, so duplicating the body
    /// would leave a block that is both untested and free to drift from the one
    /// that is.
    ///
    /// @param dirKey Directory key being acquired.
    /// @param env    Decoded request, for `callId`.
    /// @param reply  Reply sink; invoked only when this returns `true`.
    /// @param cid    Connection scope, or `0` for unscoped.
    /// @return `true` if an entry existed and @p reply was invoked; `false` to keep going.
    bool attachExistingLocked(const DirectoryKey& dirKey, const ::morph::wire::Envelope& env,
                              const std::function<void(std::string)>& reply, ConnectionId cid) {
        auto found = _directory.find(dirKey);
        if (found == _directory.end()) {
            return false;
        }
        auto const mid = found->second;
        if (_poisoned.contains(mid)) {
            // This instance's first action already failed; it must not be
            // handed to a new attacher. Evict it from the directory -- its
            // own eventual release still tears it down normally -- and report
            // a miss so the caller falls through to creating a fresh
            // instance.
            _directory.erase(found);
            _sharedKeyOf.erase(mid);
            return false;
        }
        _attachCount[mid] += 1;
        if (!noteScopeAttachLocked(mid, cid)) {
            releaseInstanceLocked(mid);
            reply(::morph::wire::encode(::morph::wire::makeErr("connection closed", env.callId)));
            return true;
        }
        reply(::morph::wire::encode(::morph::wire::makeOk(env.callId, {}, mid.v)));
        return true;
    }

    /// @brief Acquires (or creates) the shared instance for `(typeId, primary)` and replies.
    ///
    /// The register-or-attach core shared by the `register` branch (when
    /// `shared` is set) and by `attach`. A shared instance is recorded with an
    /// **empty owner principal**: `IAuthorizer::authorizeInstance`'s documented
    /// `ownerPrincipal == ctx.principal` policy would otherwise reject every
    /// client but the one that created it, defeating cross-client sharing
    /// outright. Gating access to a shared model is therefore `authorize`'s job
    /// (per type and action) or the model's own — see docs/spec/security.md.
    ///
    /// @param env            Decoded request; uses `typeId`, `primary`, `contextKey`, `callId`.
    /// @param reply          Reply sink; always invoked exactly once.
    /// @param cid            Connection scope, or `0` for unscoped.
    /// @param releaseCurrent Instance to release once the target is
    ///                       confirmed acquired or confirmed about to be
    ///                       created (an `attach` re-point), or `ModelId{0}`.
    ///                       A throwing *construction* (`_registry.create`)
    ///                       never touches it. Once construction succeeds,
    ///                       release happens in the same locked section as
    ///                       the `maxLiveModels` admission check, so a sole
    ///                       holder's release frees the slot the re-point
    ///                       itself needs rather than losing it to the cap;
    ///                       the only remaining post-release failure is the
    ///                       connection's own scope having closed
    ///                       concurrently, in which case no further request
    ///                       on it will run anyway.
    void acquireSharedInstance(const ::morph::wire::Envelope& env, const std::function<void(std::string)>& reply,
                               ConnectionId cid, ::morph::exec::detail::ModelId releaseCurrent) {
        LimitPolicy limits;
        {
            std::scoped_lock const lock{_limitsMtx};
            limits = _limits;
        }
        DirectoryKey dirKey{env.typeId, env.primary};
        {
            std::scoped_lock const lock{_regMtx};
            if (attachExistingLocked(dirKey, env, reply, cid)) {
                // Acquired (or re-confirmed) the target before touching
                // `releaseCurrent` -- a same-key re-attach lands on the exact
                // same mid attachExistingLocked just incremented, so
                // releasing it here cancels out only the redundant reference
                // that call just took, never the caller's sole hold on the
                // instance it is "re-pointing" to itself. A genuinely
                // different-key re-point releases the real old instance, same
                // as before.
                if (releaseCurrent.v != 0U) {
                    releaseScopedLocked(releaseCurrent, cid);
                }
                return;
            }
        }
        // Directory miss. Construct outside the lock, exactly as the private
        // register path does, then re-check under the insert lock: a concurrent
        // request for the same key may have won the race while we built ours.
        auto holder = _registry.create(env.typeId);
        attachLogIfConfigured(*holder, env);
        ::morph::exec::detail::ModelId const fresh{nextOpaqueId()};
        {
            std::scoped_lock const lock{_regMtx};
            if (attachExistingLocked(dirKey, env, reply, cid)) {
                if (releaseCurrent.v != 0U) {
                    releaseScopedLocked(releaseCurrent, cid);
                }
                return;
            }
            // Confirmed miss: release the old instance now, in the same
            // locked section as the maxLiveModels admission check, so a sole
            // holder's release frees exactly the slot this re-point needs
            // rather than losing it to the cap in between -- the property
            // the single `attach` wire request exists to provide. The only
            // way admission can still fail after this is a concurrently
            // closed connection scope (noteScopeAttachLocked below), which
            // makes "stranding" moot: no further request on that connection
            // will ever run anyway.
            if (releaseCurrent.v != 0U) {
                releaseScopedLocked(releaseCurrent, cid);
            }
            if (limits.maxLiveModels != 0 && _models.size() >= limits.maxLiveModels) {
                reply(::morph::wire::encode(::morph::wire::makeErr("too many models", env.callId)));
                return;
            }
            if (!noteScopeAttachLocked(fresh, cid)) {
                reply(::morph::wire::encode(::morph::wire::makeErr("connection closed", env.callId)));
                return;
            }
            _models[fresh] = std::move(holder);
            _owners[fresh] = std::string{};  // shared instances are ownerless, by design
            _directory.emplace(dirKey, fresh);
            _sharedKeyOf.emplace(fresh, std::move(dirKey));
            _attachCount[fresh] = 1;
            _firstActionPending.insert(fresh);
        }
        reply(::morph::wire::encode(::morph::wire::makeOk(env.callId, {}, fresh.v)));
    }

    /// @brief Files a live, still-anonymous instance under a primary key, in place.
    ///
    /// The existing holder of a key always wins: promoting onto a key another
    /// instance already holds is a silent no-op rather than a displacement.
    /// Symmetrically, an instance that already holds a *different* real key is
    /// left exactly where it is — also a silent no-op — since instances never
    /// change key (docs/spec/core/shared_instances.md); only a still-anonymous
    /// `mid` (no existing `_sharedKeyOf` entry) can ever be promoted.
    /// @param env Decoded request; uses `typeId`, `primary`, `modelId`.
    void applyAssignLocked(const ::morph::wire::Envelope& env) {
        ::morph::exec::detail::ModelId const mid{env.modelId};
        if (env.primary.empty() || !_models.contains(mid)) {
            return;
        }
        DirectoryKey dirKey{env.typeId, env.primary};
        if (_directory.contains(dirKey)) {
            return;
        }
        if (_sharedKeyOf.contains(mid)) {
            return;
        }
        _directory.emplace(dirKey, mid);
        _sharedKeyOf.emplace(mid, std::move(dirKey));
        _attachCount.try_emplace(mid, 1);
    }

    /// @brief Answers an `instances` request with the live shared keys of a type.
    /// @param env   Decoded request; uses `typeId` and `callId`.
    /// @param reply Reply sink; always invoked exactly once.
    void handleInstances(const ::morph::wire::Envelope& env, const std::function<void(std::string)>& reply) {
        std::vector<std::string> keys;
        {
            std::scoped_lock const lock{_regMtx};
            for (const auto& [dirKey, mid] : _directory) {
                if (dirKey.first == env.typeId) {
                    keys.push_back(dirKey.second);
                }
            }
        }
        std::string body;
        (void)glz::write_json(keys, body);
        reply(::morph::wire::encode(::morph::wire::makeOk(env.callId, std::move(body))));
    }

    // One flat switch over the wire's `kind` discriminator. Splitting it would
    // scatter the authorization sequence each branch depends on across helpers,
    // with no reader benefit.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    void dispatchMessage(const std::string& msg, std::function<void(std::string)>& reply, ConnectionId cid = 0) {
        ::morph::wire::Envelope env;
        try {
            env = ::morph::wire::decode(msg);
        } catch (const std::exception& exc) {
            reply(::morph::wire::encode(::morph::wire::makeErr(exc.what())));
            return;
        }
        // Once shutdown has begun, new work is rejected fast — before any of
        // the existing register/execute validation runs — while `deregister`
        // (and any other kind) still flows through unchanged, so a client can
        // still tear its models down cleanly during the drain window.
        if ((env.kind == "register" || env.kind == "execute" || env.kind == "attach") &&
            _shuttingDown.load(std::memory_order_acquire)) {
            reply(::morph::wire::encode(::morph::wire::makeErr("server shutting down", env.callId)));
            return;
        }
        try {
            if (env.kind == "register") {
                ::morph::observe::detail::emitMetric(::morph::observe::Metric::registerCount, 1.0);
                if (env.typeId.empty()) {
                    throw std::runtime_error("register requires a typeId");
                }
                LimitPolicy limits;
                {
                    std::scoped_lock const lock{_limitsMtx};
                    limits = _limits;
                }
                // Cheap early rejection, so a server already at its cap does not
                // pay for authorize()/authenticate() and a model construction it
                // is about to discard. Advisory only — the binding check is the
                // re-test under the insert lock further below.
                //
                // Skipped for a *shared* register, which may well create nothing:
                // if the key is already live it only takes another reference, and
                // `maxLiveModels` caps live models, not attachments to them.
                // Rejecting here would make a loaded server refuse the second
                // client of an instance it is already hosting — exactly when
                // sharing is worth the most. `acquireSharedInstance` re-tests the
                // cap under the insert lock, where it can tell the two apart.
                if (limits.maxLiveModels != 0 && (!env.shared || env.primary.empty())) {
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
                // A `shared` register naming a primary is a register-or-attach
                // against the directory; everything below is the private path,
                // unchanged.
                if (env.shared && !env.primary.empty()) {
                    acquireSharedInstance(env, reply, cid, ::morph::exec::detail::ModelId{0});
                    return;
                }
                auto holder = _registry.create(env.typeId);
                attachLogIfConfigured(*holder, env);
                // Record the owner principal for per-instance authorization:
                // env.session's principal is already the verified identity
                // stamped above (empty if the authorizer does not
                // authenticate), never the client's raw claim. This is what
                // lets `authorizeInstance` later deny a different principal.
                ::morph::exec::detail::ModelId const mid{nextOpaqueId()};
                bool scopeAlreadyClosed = false;
                bool overLiveModelCap = false;
                {
                    std::scoped_lock const lock{_regMtx};
                    // Authoritative cap re-test, in the same critical section as
                    // the insert. The check near the top of this branch releases
                    // _regMtx before authorize(), authenticate() and
                    // _registry.create() run, so concurrent registers all
                    // observed the same under-cap size and every one of them
                    // proceeded to insert -- overshooting maxLiveModels by up to
                    // the worker pool's width. Only a check that cannot be
                    // separated from its insert actually bounds anything.
                    if (limits.maxLiveModels != 0 && _models.size() >= limits.maxLiveModels) {
                        overLiveModelCap = true;
                    }
                    // A non-zero cid attributes the new instance to that
                    // connection's scope, next to _models/_owners under the
                    // same lock so scope membership can never desync from
                    // instance existence. cid == 0 (the unscoped default)
                    // records nothing, matching today's behavior byte-for-byte.
                    //
                    // find(), never operator[]: the scope may already be gone.
                    // handle() *posts* this work to the pool, while
                    // closeConnection() runs synchronously on the transport's
                    // disconnect callback from another thread, so a client that
                    // registers and immediately drops its socket genuinely
                    // interleaves the two. operator[] would default-construct —
                    // resurrecting a scope closeConnection() had already
                    // erased — and nothing ever closes a scope twice, so this
                    // model and every later one on the dead cid would be
                    // unreclaimable: an unbounded leak that, with
                    // `maxLiveModels` set, wedges the server permanently at
                    // `err "too many models"`.
                    if (!overLiveModelCap && cid != 0) {
                        auto scopeIter = _connectionScopes.find(cid);
                        if (scopeIter == _connectionScopes.end()) {
                            scopeAlreadyClosed = true;
                        } else {
                            scopeIter->second[mid] += 1;
                        }
                    }
                    if (!overLiveModelCap && !scopeAlreadyClosed) {
                        _models[mid] = std::move(holder);
                        _owners[mid] = std::move(env.session.principal);
                    }
                }
                if (overLiveModelCap) {
                    // Lost the race for the last slot. `holder` goes out of
                    // scope unregistered.
                    reply(::morph::wire::encode(::morph::wire::makeErr("too many models", env.callId)));
                    return;
                }
                if (scopeAlreadyClosed) {
                    // The connection that asked for this instance is gone. Let
                    // `holder` go out of scope unregistered rather than leak it,
                    // and answer the (already dead) caller honestly.
                    reply(::morph::wire::encode(::morph::wire::makeErr("connection closed", env.callId)));
                    return;
                }
                reply(::morph::wire::encode(::morph::wire::makeOk(env.callId, {}, mid.v)));
            } else if (env.kind == "attach") {
                if (env.typeId.empty()) {
                    throw std::runtime_error("attach requires a typeId");
                }
                if (auto verified = _authorizer->authenticate(env.session)) {
                    env.session.principal = std::move(*verified);
                } else {
                    env.session.principal.clear();
                }
                if (!_authorizer->authorizeRegister(env.session, env.typeId)) {
                    reply(::morph::wire::encode(::morph::wire::makeErr("unauthorized", env.callId)));
                    return;
                }
                acquireSharedInstance(env, reply, cid, ::morph::exec::detail::ModelId{env.modelId});
            } else if (env.kind == "assign") {
                if (env.typeId.empty()) {
                    throw std::runtime_error("assign requires a typeId");
                }
                // Mirrors "attach"'s gate: filing an instance into the shared
                // directory -- whether by creating it (register) or by
                // promoting one already live (assign) -- is bounds-checked
                // identically. Unlike "attach"/"register", assign never
                // constructs a model, but it still changes what a future
                // attacher of `primary` reaches, so it must not be reachable
                // by an unauthenticated or unauthorized caller either.
                if (auto verified = _authorizer->authenticate(env.session)) {
                    env.session.principal = std::move(*verified);
                } else {
                    env.session.principal.clear();
                }
                if (!_authorizer->authorizeRegister(env.session, env.typeId)) {
                    reply(::morph::wire::encode(::morph::wire::makeErr("unauthorized", env.callId)));
                    return;
                }
                {
                    std::scoped_lock const lock{_regMtx};
                    applyAssignLocked(env);
                }
                reply(::morph::wire::encode(::morph::wire::makeOk(env.callId, {}, env.modelId)));
            } else if (env.kind == "instances") {
                if (env.typeId.empty()) {
                    throw std::runtime_error("instances requires a typeId");
                }
                if (auto verified = _authorizer->authenticate(env.session)) {
                    env.session.principal = std::move(*verified);
                } else {
                    env.session.principal.clear();
                }
                // Enumeration is a read channel over the directory: gate it with
                // `authorize` for the model type (empty action id) so a deployer
                // can refuse listing without refusing use. It discloses the live
                // key set to anyone admitted — see docs/spec/security.md.
                if (!_authorizer->authorize(env.session, env.typeId, {})) {
                    reply(::morph::wire::encode(::morph::wire::makeErr("unauthorized", env.callId)));
                    return;
                }
                handleInstances(env, reply);
            } else if (env.kind == "deregister") {
                ::morph::observe::detail::emitMetric(::morph::observe::Metric::deregisterCount, 1.0);
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
                    // Release exactly the reference *this* connection (`cid`,
                    // the scope the deregister request itself carries) holds,
                    // not whichever connection happened to attach the
                    // instance last -- a shared instance may have several
                    // owning connections at once, and crediting the release
                    // to the wrong one either strands a reference nobody will
                    // ever decrement, or lets one connection's deregister
                    // silently consume another's hold.
                    releaseScopedLocked(mid, cid);
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

    // A single ordered gate sequence — limits, authorize, authenticate, lookup,
    // per-instance authorize — whose *order* is the security contract itself
    // (see docs/spec/security.md), so it is deliberately not broken up.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    void dispatchExecute(::morph::wire::Envelope env, std::function<void(std::string)> reply) {
        LimitPolicy limits;
        {
            std::scoped_lock const lock{_limitsMtx};
            limits = _limits;
        }
        // Cheap early shed, so an overloaded server does not pay for
        // authorize()/authenticate() on work it is about to refuse. Advisory
        // only — the binding check is the atomic reservation further below,
        // since this load and that increment are separated by authorization
        // and a registry lookup.
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
        // Concurrent in-flight executes: this is the same counter
        // LimitPolicy::maxInFlightExecutes checks above, reused (not duplicated)
        // as the executeInFlight metric's gauge, health()'s inFlight field, and
        // drainedWithin()'s drain-detection condition — one counter, four
        // consumers, never double-counted.
        //
        // Reserved with a compare-exchange rather than an unconditional
        // fetch_add, so `maxInFlightExecutes` is an actual bound. The advisory
        // check at the top of this function is a plain load, and authorize(),
        // authenticate() and the registry lookup all run between it and here —
        // long enough for every thread in the worker pool to observe the same
        // under-limit value and proceed, overshooting the cap by up to the
        // pool's width. That is exactly the burst the limit exists to prevent.
        // Reserving here, at the last point before the slot is genuinely taken,
        // needs no unwind on the early-return paths above.
        std::size_t inFlightAfterInc = 0;
        if (limits.maxInFlightExecutes != 0) {
            std::size_t current = _inFlightExecutes.load(std::memory_order_relaxed);
            for (;;) {
                if (current >= limits.maxInFlightExecutes) {
                    reply(::morph::wire::encode(::morph::wire::makeErr("server busy", env.callId)));
                    return;
                }
                // compare_exchange_weak refreshes `current` on failure, so a
                // losing thread re-tests the limit rather than forcing its way in.
                if (_inFlightExecutes.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) {
                    inFlightAfterInc = current + 1;
                    break;
                }
            }
        } else {
            inFlightAfterInc = _inFlightExecutes.fetch_add(1, std::memory_order_relaxed) + 1;
        }
        ::morph::observe::detail::emitMetric(::morph::observe::Metric::executeInFlight,
                                             static_cast<double>(inFlightAfterInc));
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
                auto const inFlightAfterDec = self->_inFlightExecutes.fetch_sub(1, std::memory_order_relaxed) - 1;
                ::morph::observe::detail::emitMetric(::morph::observe::Metric::executeInFlight,
                                                     static_cast<double>(inFlightAfterDec));
                if (inFlightAfterDec == 0) {
                    // Wakes any drainedWithin() waiter blocked on the in-flight
                    // count reaching zero. Locking _drainMtx here (rather than
                    // just notifying) closes the classic lost-wakeup window: it
                    // guarantees this notify cannot land between a waiter's
                    // predicate check and its wait_for() call.
                    std::scoped_lock const drainLock{self->_drainMtx};
                    self->_drainCv.notify_all();
                }
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
            ::morph::exec::detail::ModelId const mid{env.modelId};
            auto const start = std::chrono::steady_clock::now();
            auto const spanId =
                ::morph::observe::detail::beginSpan(env.session.requestId, env.modelType, env.actionType);
            // Metrics and endSpan are recorded before `complete(...)` runs (below)
            // so a caller observing completion — via handle()'s reply or the
            // timeout path racing it — can never see the reply before this
            // dispatch's own instrumentation is recorded. This mirrors the
            // reply-exactly-once contract `complete` already provides: whichever
            // path wins the race, the metrics for *this* strand task are always
            // emitted here, exactly once, regardless of which path's reply the
            // caller actually receives.
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
                ::morph::observe::detail::endSpan(spanId, true);
                auto const elapsedMs =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                std::array<std::pair<std::string_view, std::string_view>, 2> const tags{
                    {{"modelType", env.modelType}, {"actionType", env.actionType}}};
                ::morph::observe::detail::emitMetric(::morph::observe::Metric::executeLatencyMs, elapsedMs, tags);
                {
                    std::scoped_lock const lock{self->_regMtx};
                    self->_firstActionPending.erase(mid);
                }
                complete(::morph::wire::encode(::morph::wire::makeOk(env.callId, std::move(result))));
            } catch (const std::exception& exc) {
                {
                    std::scoped_lock const lock{self->_limitsMtx};
                    if (self->_timeoutScheduler) {
                        self->_timeoutScheduler->cancel(timeoutHandle);
                    }
                }
                ::morph::observe::detail::endSpan(spanId, false);
                auto const elapsedMs =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                std::array<std::pair<std::string_view, std::string_view>, 2> const tags{
                    {{"modelType", env.modelType}, {"actionType", env.actionType}}};
                ::morph::observe::detail::emitMetric(::morph::observe::Metric::executeLatencyMs, elapsedMs, tags);
                ::morph::observe::detail::emitMetric(::morph::observe::Metric::executeErrors, 1.0, tags);
                {
                    std::scoped_lock const lock{self->_regMtx};
                    if (auto iter = self->_firstActionPending.find(mid); iter != self->_firstActionPending.end()) {
                        self->_firstActionPending.erase(iter);
                        self->_poisoned.insert(mid);
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
    // mutable: health() is const and must still be able to lock this to read
    // _models.size() safely from any thread.
    mutable std::mutex _regMtx;
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
    // Value is a *count* per instance, not a set: one connection may attach the
    // same shared instance from two handlers, and closing the connection must
    // release both references or the instance leaks. A private instance always
    // has a count of exactly 1.
    std::unordered_map<ConnectionId, std::unordered_map<::morph::exec::detail::ModelId, std::size_t,
                                                        ::morph::exec::detail::ModelIdHash>>
        _connectionScopes;
    // Shared-instance directory: (typeId, primary) -> ModelId, its reverse, and
    // the cross-connection attach count. Guarded by _regMtx alongside
    // _models/_owners so directory membership can never desync from instance
    // existence. Only instances registered with `shared` set and a non-empty
    // primary appear; a private instance has no entry in any of the three.
    std::unordered_map<DirectoryKey, ::morph::exec::detail::ModelId, ::morph::model::detail::PairKeyHash> _directory;
    std::unordered_map<::morph::exec::detail::ModelId, DirectoryKey, ::morph::exec::detail::ModelIdHash> _sharedKeyOf;
    std::unordered_map<::morph::exec::detail::ModelId, std::size_t, ::morph::exec::detail::ModelIdHash> _attachCount;
    // First-action hydration tracking for freshly-created shared instances
    // (docs/spec/core/shared_instances.md's Failure modes: a failed first
    // action on a freshly created shared instance must not be left in the
    // directory in a half-hydrated state). `_firstActionPending` holds a mid
    // while its very first execute hasn't settled yet; `_poisoned` holds a
    // mid whose first action failed. Consulted lazily by
    // attachExistingLocked, which evicts and falls through to creating a
    // fresh instance instead of handing a poisoned one to a new attacher.
    // Guarded by `_regMtx` alongside `_models`/`_directory`. `dispatchExecute`'s
    // strand task safely mutates these directly via its own `self =
    // shared_from_this()` capture (this class's documented heap-allocation
    // contract), unlike LocalBackend's strand task, which must never touch
    // raw `this`.
    std::unordered_set<::morph::exec::detail::ModelId, ::morph::exec::detail::ModelIdHash> _firstActionPending;
    std::unordered_set<::morph::exec::detail::ModelId, ::morph::exec::detail::ModelIdHash> _poisoned;
    std::atomic<uint64_t> _nextId{0};
    std::atomic<uint64_t> _nextConnectionId{0};
    std::atomic<std::uint32_t> _minVersion{::morph::wire::kProtocolVersion};
    std::atomic<std::uint32_t> _maxVersion{::morph::wire::kProtocolVersion};
    detail::OpaqueIdGenerator _idGen;
    std::mutex _logProviderMtx;
    LogProvider _logProvider;
    std::mutex _limitsMtx;
    LimitPolicy _limits;
    // Concurrent in-flight executes: incremented when dispatchExecute admits a
    // call for dispatch (post-authorization), decremented exactly once when
    // its reply is delivered (see `complete`, above) — regardless of whether
    // the winning path was the strand's dispatch or a LimitPolicy::executeTimeout
    // firing first. Shared by the executeInFlight metric, health()'s inFlight
    // field, and drainedWithin(): one counter, never double-counted.
    std::atomic<std::size_t> _inFlightExecutes{0};
    std::unique_ptr<detail::TimeoutScheduler> _timeoutScheduler;
    // Set once by beginShutdown() and never cleared — there is no
    // un-shutdown. Checked at the top of dispatchMessage() for register and
    // execute envelopes only; deregister and any other kind are unaffected.
    std::atomic<bool> _shuttingDown{false};
    // Readiness flag for health(). Flipped to false exactly once, by
    // beginShutdown() — there is no un-shutdown, so once false it stays false.
    std::atomic<bool> _ready{true};
    std::mutex _healthMtx;
    std::function<void(const HealthStatus&)> _healthHandler;
    // Guards the condition variable drainedWithin() waits on; signalled by
    // dispatchExecute's `complete` whenever _inFlightExecutes reaches zero.
    std::mutex _drainMtx;
    std::condition_variable _drainCv;
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

    /// @brief Registers or attaches to the server's shared instance for @p identity.
    ///
    /// Sends a `shared` register, so the server returns the live instance for
    /// `(typeId, primary)` when one exists rather than creating a second. An
    /// empty primary degrades to the private path.
    /// @param typeId   String type-id sent in the `register` message.
    /// @param factory  Ignored — the server constructs via its own registry.
    /// @param identity Entity key for the action log plus the directory primary key.
    /// @return `ModelId` of the shared (or newly created) instance.
    /// @throws std::runtime_error if the server replies with an error.
    ::morph::exec::detail::ModelId registerModelShared(
        const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
        detail::InstanceIdentity identity) override {
        if (identity.primary.empty()) {
            return registerModelWithContext(typeId, std::move(factory), identity.contextKey);
        }
        auto reply =
            ::morph::wire::decode(_server.handleInline(::morph::wire::encode(::morph::wire::makeRegisterShared(
                typeId, std::string{identity.primary}, std::string{identity.contextKey}))));
        if (reply.kind == "ok") {
            return ::morph::exec::detail::ModelId{reply.modelId};
        }
        throw std::runtime_error("register failed: " + reply.message);
    }

    /// @brief Re-points from @p current to the server's shared instance for @p identity.
    ///
    /// One `attach` request rather than a deregister/register pair, so the
    /// re-pointing client cannot lose its slot to `LimitPolicy::maxLiveModels`
    /// between releasing the old instance and acquiring the new one.
    /// @param typeId   String type-id sent in the `attach` message.
    /// @param factory  Ignored — the server constructs via its own registry.
    /// @param identity Entity key for the action log plus the directory primary key.
    /// @param current  Instance currently held, or `ModelId{0}` if none.
    /// @return `ModelId` of the instance now attached to.
    /// @throws std::runtime_error if the server replies with an error.
    ::morph::exec::detail::ModelId attachModel(
        const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
        detail::InstanceIdentity identity, ::morph::exec::detail::ModelId current) override {
        if (identity.primary.empty()) {
            if (current.v != 0U) {
                deregisterModel(current);
            }
            return registerModelWithContext(typeId, std::move(factory), identity.contextKey);
        }
        auto reply = ::morph::wire::decode(_server.handleInline(::morph::wire::encode(::morph::wire::makeAttach(
            typeId, std::string{identity.primary}, current.v, std::string{identity.contextKey}))));
        if (reply.kind == "ok") {
            return ::morph::exec::detail::ModelId{reply.modelId};
        }
        throw std::runtime_error("attach failed: " + reply.message);
    }

    /// @brief Files a live server-side instance under @p primary.
    /// @param mid     Live instance to promote.
    /// @param typeId  Model type id.
    /// @param primary Canonical string encoding of the key to file it under.
    void assignPrimary(::morph::exec::detail::ModelId mid, const std::string& typeId,
                       std::string_view primary) override {
        if (primary.empty() || mid.v == 0U) {
            return;
        }
        (void)_server.handleInline(
            ::morph::wire::encode(::morph::wire::makeAssign(typeId, std::string{primary}, mid.v)));
    }

    /// @brief Asks the server for the live shared primary keys of @p typeId.
    /// @param typeId String type-id to enumerate.
    /// @return Canonical key strings of the live shared instances.
    /// @throws std::runtime_error if the server replies with an error.
    std::vector<std::string> listInstances(const std::string& typeId) override {
        auto reply =
            ::morph::wire::decode(_server.handleInline(::morph::wire::encode(::morph::wire::makeInstances(typeId))));
        if (reply.kind != "ok") {
            throw std::runtime_error("instances failed: " + reply.message);
        }
        std::vector<std::string> keys;
        if (auto errCode = glz::read_json(keys, reply.body)) {
            throw std::runtime_error("instances decode failed: " + glz::format_error(errCode, reply.body));
        }
        return keys;
    }

    /// @brief Deregisters the model on the server. Processed inline; safe from any thread.
    /// @param mid Id of the model to deregister.
    void deregisterModel(::morph::exec::detail::ModelId mid) override {
        (void)_server.handleInline(::morph::wire::encode(::morph::wire::makeDeregister(mid.v)));
    }

    /// @brief Sends a `"hello"` envelope to the server and classifies its reply.
    ///
    /// Processed inline on the calling thread via `RemoteServer::handleInline`,
    /// same as `registerModel`/`deregisterModel`. Intended to be called once,
    /// typically right after construction and before any
    /// `registerModel`/`execute` call — nothing enforces that ordering.
    ///
    /// @return `Negotiated` if the server accepted `kProtocolVersion`;
    ///         `LegacyPeer` if the server does not understand `"hello"` (an
    ///         un-upgraded `RemoteServer`).
    /// @throws std::runtime_error if the server explicitly rejects the version
    ///         (e.g. `"protocol version unsupported"`).
    ::morph::wire::ProtocolNegotiationResult negotiateProtocolVersion() {
        auto reply = ::morph::wire::decode(_server.handleInline(::morph::wire::encode(::morph::wire::makeHello())));
        return ::morph::wire::interpretHelloReply(reply);
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
