// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <core/async/StopToken.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <latch>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../attributes.hpp"
#include "../journal/action_log.hpp"
#include "../session/session.hpp"
#include "backend.hpp"
#include "completion.hpp"
#include "detail/owner_probe.hpp"
#include "detail/reply_router.hpp"
#include "logger.hpp"
#include "observability.hpp"
#include "owner_strand.hpp"
#include "timeout_scheduler.hpp"
#include "wire.hpp"

namespace morph::backend {

/// @brief Opt-in, connection-agnostic resource limits enforced by `RemoteServer`.
///
/// Every field defaults to `0`, meaning "unbounded": a default-constructed
/// policy applies no limit. Given as `ServerConfig::limits`, at construction.
/// See `docs/spec/core/backend.md`.
struct LimitPolicy {
    /// @brief Max wall-clock time a single `execute` may take before the server
    ///        sends an `err "timeout"` reply and discards the eventual strand
    ///        result. `0` = no timeout (today's behavior).
    ///
    /// An ordinary handler is never interrupted — it keeps running to
    /// completion on its strand, so this bounds the *caller's wait*, not the
    /// model. A handler returning `core::async::Task` is also asked to stop: its
    /// stop token is requested, and it unwinds at its next stop-aware
    /// `co_await` (see `docs/spec/core/coroutines.md`).
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

/// @brief Whether `RemoteServer` requires an `execute` body to carry every
///        field the action's served schema marks `required`.
///
/// The one mechanical check `morph::wire`'s published action-evolution policy
/// (docs/spec/core/wire.md, "Action-evolution policy") supports without
/// contradicting itself. The policy's first bullet — *"New fields must be
/// optional … so an older peer that omits them decodes cleanly"* — makes
/// optionality, and only optionality, the wire's marker for *may be absent*.
/// A field that is **not** optional therefore may not be absent, and the
/// lenient `fromJson` cannot notice when it is: an absent field is
/// default-constructed and an unknown one is dropped, so a client/server
/// rename (`amount` → `amountCents`) decodes to a zero-valued action that
/// `validate()` cannot tell from a legitimate zero.
///
/// @warning Not the default, and deliberately so. Turning it on rejects
/// payloads a pre-existing client sends today, which is a non-additive change
/// to the wire contract — precisely what the same policy says requires a
/// `kProtocolVersion` bump. Enabling it by default would break the policy in
/// the act of enforcing it. See docs/spec/core/wire.md, "Enforcing the policy".
enum class PayloadCompleteness : std::uint8_t {
    /// @brief Today's behaviour: the action codec's lenient decode is the only
    ///        gate, and a missing field is a default-constructed one.
    Lenient,
    /// @brief Reject an `execute` whose `body` omits a key the action's served
    ///        schema lists in `required`, with
    ///        `err "payload missing required field(s): …"`.
    RequireDeclaredFields,
};

namespace detail {

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

/// @brief Callable that supplies the action log to attach to a newly
///        registered instance, given its model type and `contextKey`.
///
/// Return `nullptr` to register the instance with no log attached (e.g. for
/// model types or context keys the host app doesn't want journaled).
using LogProvider = std::function<std::shared_ptr<::morph::journal::IActionLog>(std::string_view modelType,
                                                                                std::string_view contextKey)>;

/// @brief Everything about a `RemoteServer` that is configured rather than
///        learned: given once, at construction, and never changed.
///
/// The server reads these fields on its own strand and on its models' strands
/// without a lock, which is sound only because nothing writes them after the
/// constructor. A deployment that wants different limits constructs a
/// different server. See `docs/spec/core/backend.md`, "`ServerConfig`".
struct ServerConfig {
    /// @brief Opt-in resource limits. Default-constructed: unbounded.
    LimitPolicy limits{};

    /// @brief Consulted whenever an instance is constructed with a non-empty
    ///        `contextKey` — every `register` envelope, and every `attach` that
    ///        misses the shared directory and therefore creates the instance.
    ///
    /// `RemoteServer` owns the model instances of every remote client, so it is
    /// the only place that can attach an action log to them. Null: no instance
    /// gets a log. Runs on the server's strand.
    LogProvider logProvider{};

    /// @brief Called with the server's health: once from the constructor, and
    ///        again, on the server's strand, whenever readiness changes
    ///        (`beginShutdown()`). Null: nothing is called.
    ///
    /// A deployment's transport can expose it over a probe endpoint; morph does
    /// not embed an HTTP server.
    std::function<void(const HealthStatus&)> healthHandler{};

    /// @brief Oldest protocol version this server accepts in reply to
    ///        `"hello"`. Must not exceed `maxProtocolVersion`.
    std::uint32_t minProtocolVersion = ::morph::wire::kProtocolVersion;

    /// @brief Newest protocol version this server accepts in reply to
    ///        `"hello"`. Widen the range when a `kProtocolVersion` bump must keep
    ///        serving older clients through their deprecation window (see
    ///        docs/spec/core/wire.md, "Action-evolution policy").
    std::uint32_t maxProtocolVersion = ::morph::wire::kProtocolVersion;

    /// @brief Whether an `execute` body must carry every field the action's
    ///        served schema marks `required`. See `PayloadCompleteness` for why
    ///        `Lenient` is the default.
    PayloadCompleteness payloadCompleteness = PayloadCompleteness::Lenient;
};

/// @brief Server-side message handler that owns model instances and dispatches actions.
///
/// `RemoteServer` receives JSON envelopes (`morph::wire::Envelope`) from any
/// transport (WebSocket, in-process simulation, …) and executes the corresponding
/// model operations via an `ActionDispatcher`. Authorization is delegated to an
/// `IAuthorizer` that defaults to allow-all.
///
/// @par One owner
/// The server's state — the instance registry, the connection scopes, the
/// in-flight count, readiness — belongs to one strand over the worker pool, the
/// *server strand*, and is touched only in its tasks. Every public verb posts
/// to it. An `execute` is admitted there and then posted to its model's own
/// strand, so for one model the order `handle()` was called in is the order the
/// model runs them: both hops are strands, and a strand runs in post order.
/// Configuration is a `ServerConfig`, fixed at construction.
///
/// @par Heap allocation requirement
/// `RemoteServer` **must** be heap-allocated via `std::make_shared`. Every task
/// it posts captures `shared_from_this()`, so the server outlives its queued
/// work however the last external reference is dropped.
///
/// @par Wire format
/// All requests and replies are encoded as `morph::wire::Envelope` JSON. See
/// `wire.hpp` for the field semantics. The `kind` field is the discriminator.
class RemoteServer : public std::enable_shared_from_this<RemoteServer> {
public:
    /// @brief Alias of `morph::backend::LogProvider`, the type of
    ///        `ServerConfig::logProvider`.
    using LogProvider = ::morph::backend::LogProvider;

    /// @brief Constructs a server backed by @p workerPool with allow-all
    ///        authorization and a default `ServerConfig`.
    ///
    /// @param workerPool Pool the server strand and every model strand run on.
    ///                   Borrowed, not owned: it must outlive this server and
    ///                   keep running until teardown completes (see
    ///                   `docs/spec/concurrency_and_lifetimes.md`, "Destruction
    ///                   ordering").
    /// @param dispatcher Action dispatcher; defaults to the process-level
    ///                   singleton. Borrowed: it must outlive this server.
    /// @param registry   Model factory registry; defaults to the process-level
    ///                   singleton. Borrowed: it must outlive this server.
    explicit RemoteServer(::morph::exec::IExecutor& workerPool MORPH_LIFETIMEBOUND,
                          ::morph::model::detail::ActionDispatcher& dispatcher MORPH_LIFETIMEBOUND =
                              ::morph::model::detail::defaultDispatcher(),
                          ::morph::model::detail::ModelRegistryFactory& registry MORPH_LIFETIMEBOUND =
                              ::morph::model::detail::defaultRegistry())
        : RemoteServer{workerPool, ::morph::session::allowAllAuthorizer(), ServerConfig{}, dispatcher, registry} {}

    /// @brief Constructs a server with a custom authorizer and a default
    ///        `ServerConfig`.
    ///
    /// @param workerPool Pool the server's strands run on. Borrowed, on the
    ///                   same terms as the constructor above.
    /// @param authorizer Authorizer consulted for every envelope that makes an
    ///                   authorization decision; null means allow-all.
    /// @param dispatcher Action dispatcher; defaults to the process-level
    ///                   singleton. Borrowed: it must outlive this server.
    /// @param registry   Model factory registry; defaults to the process-level
    ///                   singleton. Borrowed: it must outlive this server.
    RemoteServer(::morph::exec::IExecutor& workerPool MORPH_LIFETIMEBOUND,
                 std::shared_ptr<::morph::session::IAuthorizer> authorizer,
                 ::morph::model::detail::ActionDispatcher& dispatcher MORPH_LIFETIMEBOUND =
                     ::morph::model::detail::defaultDispatcher(),
                 ::morph::model::detail::ModelRegistryFactory& registry MORPH_LIFETIMEBOUND =
                     ::morph::model::detail::defaultRegistry())
        : RemoteServer{workerPool, std::move(authorizer), ServerConfig{}, dispatcher, registry} {}

    /// @brief Constructs a server with allow-all authorization and @p config.
    ///
    /// @param workerPool Pool the server's strands run on. Borrowed, on the
    ///                   same terms as the first constructor.
    /// @param config     Limits, log provider, health handler, protocol range
    ///                   and payload rule, fixed for the server's life.
    /// @param dispatcher Action dispatcher; defaults to the process-level
    ///                   singleton. Borrowed: it must outlive this server.
    /// @param registry   Model factory registry; defaults to the process-level
    ///                   singleton. Borrowed: it must outlive this server.
    /// @throws std::invalid_argument if `config.minProtocolVersion` exceeds
    ///         `config.maxProtocolVersion`.
    RemoteServer(::morph::exec::IExecutor& workerPool MORPH_LIFETIMEBOUND, ServerConfig config,
                 ::morph::model::detail::ActionDispatcher& dispatcher MORPH_LIFETIMEBOUND =
                     ::morph::model::detail::defaultDispatcher(),
                 ::morph::model::detail::ModelRegistryFactory& registry MORPH_LIFETIMEBOUND =
                     ::morph::model::detail::defaultRegistry())
        : RemoteServer{workerPool, ::morph::session::allowAllAuthorizer(), std::move(config), dispatcher, registry} {}

    /// @brief Constructs a server with a custom authorizer and @p config.
    ///
    /// Calls `config.healthHandler`, if set, once, before returning, with the
    /// initial status (ready, nothing registered, nothing in flight).
    ///
    /// @param workerPool Pool the server's strands run on. Borrowed, on the
    ///                   same terms as the first constructor.
    /// @param authorizer Authorizer consulted for every envelope that makes an
    ///                   authorization decision; null means allow-all.
    /// @param config     Limits, log provider, health handler, protocol range
    ///                   and payload rule, fixed for the server's life.
    /// @param dispatcher Action dispatcher; defaults to the process-level
    ///                   singleton. Borrowed: it must outlive this server.
    /// @param registry   Model factory registry; defaults to the process-level
    ///                   singleton. Borrowed: it must outlive this server.
    /// @throws std::invalid_argument if `config.minProtocolVersion` exceeds
    ///         `config.maxProtocolVersion`.
    RemoteServer(::morph::exec::IExecutor& workerPool MORPH_LIFETIMEBOUND,
                 std::shared_ptr<::morph::session::IAuthorizer> authorizer, ServerConfig config,
                 ::morph::model::detail::ActionDispatcher& dispatcher MORPH_LIFETIMEBOUND =
                     ::morph::model::detail::defaultDispatcher(),
                 ::morph::model::detail::ModelRegistryFactory& registry MORPH_LIFETIMEBOUND =
                     ::morph::model::detail::defaultRegistry())
        : _pool{workerPool},
          _strand{workerPool},
          _strands{std::make_shared<::morph::exec::detail::ModelStrands>(workerPool)},
          _dispatcher{dispatcher},
          _registry{registry},
          _authorizer{authorizer ? std::move(authorizer) : ::morph::session::allowAllAuthorizer()},
          _config{validated(std::move(config))},
          _executeTimeouts{_config.limits.executeTimeout.count() > 0
                               ? std::make_unique<::morph::async::detail::TimeoutScheduler>()
                               : nullptr} {
        if (_config.healthHandler) {
            _config.healthHandler(HealthStatus{.ready = true, .liveModels = 0, .inFlight = 0});
        }
    }

    RemoteServer(const RemoteServer&) = delete;
    RemoteServer& operator=(const RemoteServer&) = delete;
    RemoteServer(RemoteServer&&) = delete;
    RemoteServer& operator=(RemoteServer&&) = delete;

    /// @brief Closes the server strand before any other member goes.
    ///
    /// Every task the server posts holds the server, so nothing of it is still
    /// queued here; closing waits for a task still running on another thread
    /// and returns at once from inside the server's own last task.
    ~RemoteServer() {
        _strand.seal();
        _strand.close();
    }

    /// @brief Asynchronously processes a JSON `Envelope` and calls @p reply with the response.
    ///
    /// Decodes on the calling thread and posts the envelope to the server
    /// strand. @p reply is called exactly once, from a pool thread: the server
    /// strand's, or, for an `execute`, the model strand's (or the I/O loop's,
    /// when a `LimitPolicy::executeTimeout` answers first).
    ///
    /// Callable from any thread. For one connection, calls made one after the
    /// other on one thread are handled in that order.
    ///
    /// @param msg   JSON-encoded `morph::wire::Envelope` (via `wire::encode`).
    /// @param reply Callback invoked with the JSON-encoded reply envelope.
    void handle(std::string msg, std::function<void(std::string)> reply) {
        handleImpl(std::move(msg), std::move(reply), 0);
    }

    /// @brief Like `handle(msg, reply)`, but additionally attributes any
    ///        `register` decoded from @p msg to a connection scope.
    ///
    /// A `register` processed under a non-zero @p cid records the new model in
    /// that connection's scope, so a later `closeConnection(cid)` reclaims it.
    /// Passing `cid == 0` is exactly the unscoped, two-argument `handle()` —
    /// nothing is recorded and nothing is ever cleaned up automatically.
    ///
    /// Callable from any thread, on the same terms as `handle(msg, reply)`.
    ///
    /// @param msg   JSON-encoded `morph::wire::Envelope` (via `wire::encode`).
    /// @param reply Callback invoked with the JSON-encoded reply envelope.
    /// @param cid   Connection scope to attribute a `register` in @p msg to;
    ///              `0` means unscoped.
    void handle(std::string msg, std::function<void(std::string)> reply, ConnectionId cid) {
        handleImpl(std::move(msg), std::move(reply), cid);
    }

    /// @brief Processes a control `Envelope` on the server strand and returns
    ///        its reply, blocking the caller until it has one.
    ///
    /// For `register`, `deregister`, `attach`, `assign`, `instances`, `schemas`
    /// and `hello` — the synchronous control path `SimulatedRemoteBackend`
    /// uses. Runs inline when the caller is already on the server strand;
    /// otherwise posts and waits. `execute` is rejected up front with an `err`
    /// reply: its reply is produced later, on the model's strand.
    ///
    /// Callable from a pool thread, including one inside a running action (a
    /// handler registered from an action handler): the server strand then runs
    /// on another pool thread while this one waits. So the pool needs a thread
    /// free for it — a one-thread pool calling this from inside its own task,
    /// or every pool thread blocked here at once, never gets its reply. See
    /// docs/spec/concurrency_and_lifetimes.md, "Synchronous re-entry".
    ///
    /// @param msg JSON-encoded `morph::wire::Envelope` (via `wire::encode`).
    /// @return JSON-encoded reply envelope.
    std::string handleInline(const std::string& msg) { return handleInline(msg, 0); }

    /// @brief Like `handleInline(msg)`, but additionally attributes any
    ///        `register` decoded from @p msg to a connection scope.
    ///
    /// The synchronous counterpart to the scoped `handle(msg, reply, cid)`
    /// overload, on the same terms as the one-argument `handleInline`.
    /// Passing `cid == 0` is exactly the unscoped overload above.
    ///
    /// @param msg JSON-encoded `morph::wire::Envelope` (via `wire::encode`).
    /// @param cid Connection scope to attribute a `register` in @p msg to;
    ///            `0` means unscoped.
    /// @return JSON-encoded reply envelope.
    std::string handleInline(const std::string& msg, ConnectionId cid) {
        Decoded decoded = decodeEnvelope(msg);
        if (decoded.env && decoded.env->kind == "execute") {
            return ::morph::wire::encode(::morph::wire::makeErr(
                "handleInline does not support execute (reply is asynchronous)", decoded.env->callId));
        }
        std::string out;
        std::function<void(std::string)> capture = [&out](std::string reply) noexcept { out = std::move(reply); };
        if (_strand.runningHere()) {
            dispatchDecoded(std::move(decoded), capture, cid);
            return out;
        }
        std::latch done{1};
        _strand.postTask([self = shared_from_this(), &decoded, &capture, cid, &done] {
            // Counted down however the dispatch ends, so the caller is never
            // left waiting on a task that threw.
            try {
                self->dispatchDecoded(std::move(decoded), capture, cid);
            } catch (...) {
                done.count_down();
                throw;
            }
            done.count_down();
        });
        done.wait();
        return out;
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
    /// Callable from any thread. The id is drawn at once; the scope is opened
    /// on the server strand, before any `handle()` the same thread makes next.
    /// @return A fresh, non-zero `ConnectionId`.
    [[nodiscard]] ConnectionId openConnection() {
        ConnectionId const cid{_nextConnectionId.fetch_add(1) + 1};
        _strand.postTask([self = shared_from_this(), cid] {
            self->noteOwner("RemoteServer::openConnection");
            self->_connectionScopes.try_emplace(cid);
        });
        return cid;
    }

    /// @brief Reclaims every model still registered under @p cid, then drops the scope.
    ///
    /// Call once the transport observes the connection is gone (disconnect,
    /// close, error). Releases exactly as many references as this connection
    /// held, exactly as an explicit `deregister` would, then drops the scope
    /// itself. A *private* instance (count 1, no directory entry) is erased
    /// outright, so a later `execute` against its id replies
    /// `err "model not found"`; a *shared* instance another connection is still
    /// attached to survives, and an `execute` against it still succeeds — see
    /// `InstanceDirectory::release`'s `retained` outcome.
    ///
    /// Idempotent: `cid == 0`, an unknown `cid`, or a `cid` already closed is a
    /// no-op. Deliberately does **not** consult `IAuthorizer` — this is the
    /// server's own housekeeping in reaction to a transport-level event, not
    /// an action attributable to any caller (see docs/spec/core/backend.md).
    ///
    /// Safe while a model in the scope has an `execute` in flight: the model
    /// strand's task holds its own `shared_ptr` to the model holder, so erasing
    /// the registry entry here only prevents *new* lookups (see
    /// docs/spec/concurrency_and_lifetimes.md).
    ///
    /// Callable from any thread; runs on the server strand, before any
    /// `handle()` the same thread makes next.
    /// @param cid Connection scope to close, as returned by `openConnection()`.
    void closeConnection(ConnectionId cid) {
        if (cid == 0) {
            return;
        }
        _strand.postTask([self = shared_from_this(), cid] { self->closeConnectionHere(cid); });
    }

    /// @brief The server strand, as an executor.
    ///
    /// What `runningOn(server.strand())` asks about: true inside every task the
    /// server runs on its own state, false elsewhere. A task posted to it runs
    /// in order with the server's own, never beside one.
    /// @return The strand every envelope, verb and `execute` admission runs on.
    [[nodiscard]] ::morph::exec::IExecutor& strand() noexcept { return _strand; }

    /// @brief Returns the configured payload-completeness rule.
    /// @return `ServerConfig::payloadCompleteness`, as given at construction.
    [[nodiscard]] PayloadCompleteness payloadCompleteness() const noexcept { return _config.payloadCompleteness; }

    /// @brief The server's health, answered on the server strand.
    ///
    /// Posted, so the answer reflects every envelope and verb this thread
    /// handed the server before calling. Callable from any thread.
    /// @return A `Completion` settled on the server strand with `ready`, the
    ///         live model count and the in-flight execute count; its callbacks
    ///         run on the worker pool.
    [[nodiscard]] ::morph::async::Completion<HealthStatus> health() {
        auto settleable = ::morph::async::Completion<HealthStatus>::makeSettleable(&_pool);
        _strand.postTask([self = shared_from_this(), promise = std::move(settleable.second)]() mutable {
            self->noteOwner("RemoteServer::health");
            promise.resolve(self->snapshotHealth());
        });
        return std::move(settleable.first);
    }

    /// @brief Enters shutdown: from now on, `register`, `attach` and `execute`
    ///        envelopes are rejected with `err "server shutting down"`.
    ///        `deregister` is still served so clients can tear down cleanly.
    ///        `attach` is refused too, so a client cannot re-attach to a shared
    ///        instance during the drain window.
    ///
    /// Posted to the server strand: an envelope this thread hands `handle()`
    /// after this call returns is refused. Idempotent. There is no way back: a
    /// restarted service constructs a fresh `RemoteServer`.
    ///
    /// Also flips `health()`'s `ready` to `false` and calls
    /// `ServerConfig::healthHandler`, if set, with the post-shutdown status, on
    /// the server strand — what lets an orchestrator stop routing to this
    /// server while `drainedWithin()`'s drain runs.
    void beginShutdown() {
        _strand.postTask([self = shared_from_this()] {
            self->noteOwner("RemoteServer::beginShutdown");
            bool const wasReady = self->_ready;
            self->_shuttingDown = true;
            self->_ready = false;
            if (wasReady && self->_config.healthHandler) {
                self->_config.healthHandler(self->snapshotHealth());
            }
        });
    }

    /// @brief Answers whether every in-flight `execute` delivers its reply
    ///        within @p deadline.
    ///
    /// "In-flight" is one count, kept on the server strand: incremented when an
    /// `execute` is admitted for dispatch and decremented, on the strand, when
    /// its reply is sent — on every resolving path (`ok`, `err`, or a
    /// `LimitPolicy::executeTimeout` firing first). The same count
    /// `LimitPolicy::maxInFlightExecutes` gates and `health()`'s `inFlight`
    /// reads. Independent of `beginShutdown()`: it observes, it does not stop
    /// new work from arriving. Callable from any thread; nothing blocks.
    /// @param deadline Longest time to wait; `0` answers from the count as it
    ///        stands.
    /// @return A `Completion` settled on the server strand: `true` once the
    ///         count is zero, `false` if @p deadline elapses first. Its
    ///         callbacks run on the worker pool.
    [[nodiscard]] ::morph::async::Completion<bool> drainedWithin(std::chrono::milliseconds deadline) {
        auto settleable = ::morph::async::Completion<bool>::makeSettleable(&_pool);
        _strand.postTask(
            [self = shared_from_this(), deadline,
             promise = std::make_shared<::morph::async::Completion<bool>::Promise>(std::move(settleable.second))] {
                self->noteOwner("RemoteServer::drainedWithin");
                self->addDrainWaiter(deadline, promise);
            });
        return std::move(settleable.first);
    }

private:
    /// A settleable `bool` completion, held by a drain waiter.
    using DrainPromise = std::shared_ptr<::morph::async::Completion<bool>::Promise>;

    /// An envelope decoded on the transport's thread, or why it could not be.
    struct Decoded {
        std::optional<::morph::wire::Envelope> env;
        /// The raw message, kept only when the decode failed, for the log line.
        std::string raw;
        /// The decode exception's message, when the decode failed.
        std::string error;
    };

    /// @brief Rejects a configuration whose protocol range is empty.
    /// @param config The configuration to check.
    /// @return @p config, unchanged.
    /// @throws std::invalid_argument if `minProtocolVersion > maxProtocolVersion`.
    static ServerConfig validated(ServerConfig config) {
        if (config.minProtocolVersion > config.maxProtocolVersion) {
            throw std::invalid_argument("ServerConfig: minProtocolVersion must not exceed maxProtocolVersion");
        }
        return config;
    }

    /// @brief Decodes @p msg, keeping the message and the error when it fails.
    /// @param msg JSON-encoded envelope.
    /// @return The envelope, or the raw message and the decode error.
    static Decoded decodeEnvelope(std::string msg) {
        Decoded decoded;
        try {
            decoded.env = ::morph::wire::decode(msg);
        } catch (const std::exception& exc) {
            decoded.raw = std::move(msg);
            decoded.error = exc.what();
        }
        return decoded;
    }

    /// @brief Shared body of both `handle()` overloads: one decode here, on the
    ///        transport's thread, and one post to the server strand.
    /// @param msg   JSON-encoded `morph::wire::Envelope` (via `wire::encode`).
    /// @param reply Callback invoked with the JSON-encoded reply envelope.
    /// @param cid   Connection scope; `0` means unscoped (see `handle()`'s own doc).
    void handleImpl(std::string msg, std::function<void(std::string)> reply, ConnectionId cid) {
        _strand.postTask([self = shared_from_this(), decoded = decodeEnvelope(std::move(msg)),
                          reply = std::move(reply),
                          cid]() mutable { self->dispatchDecoded(std::move(decoded), reply, cid); });
    }

    /// @brief Records that the calling body touches server-strand state; a
    ///        debug build asserts it runs on the server strand.
    /// @param site Static name of the body.
    void noteOwner(char const* site) {
        ::morph::exec::detail::noteOwner(site, _strand.coreExecutor(), _strand.runningHere());
    }

    /// @brief The health snapshot. On the server strand.
    /// @return `ready`, the live instance count, the in-flight count.
    [[nodiscard]] HealthStatus snapshotHealth() const {
        return HealthStatus{.ready = _ready, .liveModels = _instances.size(), .inFlight = _inFlight};
    }

    /// @brief Body of `closeConnection`. On the server strand.
    /// @param cid Connection scope to close.
    void closeConnectionHere(ConnectionId cid) {
        noteOwner("RemoteServer::closeConnection");
        auto scopeIter = _connectionScopes.find(cid);
        if (scopeIter == _connectionScopes.end()) {
            return;
        }
        for (const auto& [mid, refs] : scopeIter->second) {
            // Release exactly as many references as this connection held. A
            // shared instance another connection is still attached to survives;
            // a private one (count 1, no directory entry) is erased outright.
            for (std::size_t idx = 0; idx < refs; ++idx) {
                releaseInstance(mid);
            }
        }
        _connectionScopes.erase(scopeIter);
    }

    /// @brief Registers a drain waiter, or answers it at once. On the server strand.
    /// @param deadline Longest time to wait for the in-flight count to reach zero.
    /// @param promise  Settled `true` at zero, `false` at the deadline.
    void addDrainWaiter(std::chrono::milliseconds deadline, const DrainPromise& promise) {
        if (_inFlight == 0) {
            promise->resolve(true);
            return;
        }
        if (deadline.count() <= 0) {
            promise->resolve(false);
            return;
        }
        std::uint64_t const waiterId = ++_nextDrainWaiter;
        if (!_drainTimer) {
            _drainTimer = std::make_unique<::morph::async::detail::TimeoutScheduler>();
        }
        // Weak: a pending deadline does not keep a server alive that nothing
        // else refers to. The expiry is posted back, since the waiters are
        // strand state and the timer fires on its I/O loop.
        auto const handle = _drainTimer->schedule(deadline, [weak = weak_from_this(), waiterId] {
            if (auto self = weak.lock()) {
                self->_strand.postTask([self, waiterId] { self->expireDrainWaiter(waiterId); });
            }
        });
        _drainWaiters.push_back(DrainWaiter{.id = waiterId, .promise = promise, .timer = handle});
    }

    /// @brief Answers `false` to the drain waiter @p waiterId, if it is still
    ///        waiting. On the server strand.
    /// @param waiterId The waiter whose deadline elapsed.
    void expireDrainWaiter(std::uint64_t waiterId) {
        noteOwner("RemoteServer::drainedWithin");
        auto const found = std::ranges::find(_drainWaiters, waiterId, &DrainWaiter::id);
        if (found == _drainWaiters.end()) {
            return;
        }
        DrainPromise const promise = found->promise;
        _drainWaiters.erase(found);
        promise->resolve(false);
    }

    /// @brief Counts one execute's reply as sent, and answers every drain
    ///        waiter once none is left in flight. On the server strand.
    void executeFinished() {
        noteOwner("RemoteServer::executeFinished");
        _inFlight -= 1;
        ::morph::observe::detail::emitMetric(::morph::observe::Metric::executeInFlight,
                                             static_cast<double>(_inFlight));
        if (_inFlight != 0 || _drainWaiters.empty()) {
            return;
        }
        auto waiters = std::move(_drainWaiters);
        _drainWaiters.clear();
        for (auto& waiter : waiters) {
            _drainTimer->cancel(waiter.timer);
            waiter.promise->resolve(true);
        }
    }

    /// @brief Authenticates @p env's session and makes the verified identity
    ///        authoritative on it.
    ///
    /// A verifying `_authorizer` returns the principal it extracted from a
    /// valid token; that overwrites `env.session.principal` so model code
    /// reading `session::current()->principal` can trust it. If
    /// `authenticate()` returns `nullopt` the authorizer cannot vouch for the
    /// caller, so the client-asserted principal is cleared rather than passed
    /// through unverified — every envelope kind this is called for (register,
    /// attach, assign, instances, schemas, execute) makes an authorization or
    /// ownership decision that must key on the verified identity, never the
    /// client's raw claim. See docs/spec/security.md.
    /// @param env Envelope whose `session.principal` is stamped or cleared in place.
    void stampVerifiedPrincipal(::morph::wire::Envelope& env) {
        if (auto verified = _authorizer->authenticate(env.session)) {
            env.session.principal = std::move(*verified);
        } else {
            env.session.principal.clear();
        }
    }

    /// @brief Releases one reference to @p mid, destroying it at zero. On the server strand.
    ///
    /// A private instance carries no attachments and is erased outright. A
    /// shared instance is erased, and removed from the directory, only when its
    /// last attachment goes away, so one client's `deregister` or dropped
    /// connection never tears an instance out from under another client still
    /// using it.
    /// @param mid Instance to release.
    void releaseInstance(::morph::exec::detail::ModelId mid) { (void)_instances.release(mid); }

    /// @brief Drops one of @p cid's references to @p mid, then releases the
    ///        instance. On the server strand.
    /// @param mid Instance to release.
    /// @param cid Connection whose reference is being dropped; `0` for unscoped.
    void releaseScoped(::morph::exec::detail::ModelId mid, ConnectionId cid) {
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
        releaseInstance(mid);
    }

    /// @brief Records a new attachment of @p mid to @p cid. On the server strand.
    /// @param mid Instance being attached.
    /// @param cid Connection attaching it; `0` for unscoped (records nothing).
    /// @return `false` if @p cid's scope was already closed, in which case nothing was recorded.
    bool noteScopeAttach(::morph::exec::detail::ModelId mid, ConnectionId cid) {
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

    /// @brief Attaches the configured `LogProvider`'s log to a freshly created
    ///        holder. On the server strand.
    /// @param holder Newly created instance.
    /// @param env    Envelope carrying `typeId` and `contextKey`.
    void attachLogIfConfigured(::morph::model::detail::IModelHolder& holder, const ::morph::wire::Envelope& env) {
        if (env.contextKey.empty() || !_config.logProvider) {
            return;
        }
        noteOwner("RemoteServer::attachLog");
        if (auto log = _config.logProvider(env.typeId, env.contextKey)) {
            holder.attachActionLog(std::move(log), env.contextKey);
        }
    }

    /// @brief Attaches to an already-live directory entry, if there is one. On the server strand.
    /// @param dirKey Directory key being acquired.
    /// @param env    Decoded request, for `callId`.
    /// @param reply  Reply sink; invoked only when this returns `true`.
    /// @param cid    Connection scope, or `0` for unscoped.
    /// @return `true` if an entry existed and @p reply was invoked; `false` to keep going.
    bool attachExisting(const detail::DirectoryKey& dirKey, const ::morph::wire::Envelope& env,
                        const std::function<void(std::string)>& reply, ConnectionId cid) {
        // `nullopt` covers both a plain directory miss and an instance whose
        // first action failed: `InstanceDirectory::attach` evicts the latter and
        // reports it as a miss, so the caller falls through to creating a fresh
        // instance either way. The evicted instance stays live and is torn down
        // normally by whoever created it.
        auto const attached = _instances.attach(dirKey);
        if (!attached) {
            return false;
        }
        auto const mid = *attached;
        if (!noteScopeAttach(mid, cid)) {
            releaseInstance(mid);
            reply(::morph::wire::encode(::morph::wire::makeErr("connection closed", env.callId)));
            return true;
        }
        reply(::morph::wire::encode(::morph::wire::makeOk(env.callId, {}, mid.v)));
        return true;
    }

    /// @brief Acquires (or creates) the shared instance for `(typeId, primary)`
    ///        and replies. On the server strand.
    ///
    /// The register-or-attach core shared by the `register` branch (when
    /// `shared` is set) and by `attach`. A shared instance is recorded with an
    /// **empty owner principal**: `IAuthorizer::authorizeInstance`'s documented
    /// `ownerPrincipal == ctx.principal` policy would otherwise reject every
    /// client but the one that created it, defeating cross-client sharing
    /// outright. Gating access to a shared model is therefore `authorize`'s job
    /// (per type and action) or the model's own — see docs/spec/security.md.
    ///
    /// The directory check, the construction, the `maxLiveModels` admission and
    /// the insert all run in this one strand task. Only host code this task
    /// itself calls — the model's construction, the log provider — can file the
    /// key in between, through `handleInline`, so the directory is checked
    /// again after it.
    ///
    /// @param env            Decoded request; uses `typeId`, `primary`, `contextKey`, `callId`.
    /// @param reply          Reply sink; always invoked exactly once.
    /// @param cid            Connection scope, or `0` for unscoped.
    /// @param releaseCurrent Instance to release once the target is confirmed
    ///                       acquired or about to be created (an `attach`
    ///                       re-point), or `ModelId{0}`. A throwing construction
    ///                       never touches it. It is released before the
    ///                       `maxLiveModels` admission check, so a sole holder's
    ///                       release frees the slot the re-point itself needs.
    void acquireSharedInstance(const ::morph::wire::Envelope& env, const std::function<void(std::string)>& reply,
                               ConnectionId cid, ::morph::exec::detail::ModelId releaseCurrent) {
        detail::DirectoryKey dirKey{env.typeId, env.primary};
        if (attachExisting(dirKey, env, reply, cid)) {
            // A same-key re-attach lands on the exact mid attachExisting just
            // incremented, so releasing `releaseCurrent` here cancels only the
            // redundant reference that call took, never the caller's sole hold.
            // A different-key re-point releases the real old instance.
            if (releaseCurrent.v != 0U) {
                releaseScoped(releaseCurrent, cid);
            }
            return;
        }
        // env.primary is this instance's directory key -- always non-empty here
        // (acquireSharedInstance is only reached for a shared/keyed
        // register-or-attach), so the model learns its own key once, at
        // construction, via IModelHolder::attachIdentity.
        auto holder = _registry.create(env.typeId, env.primary);
        attachLogIfConfigured(*holder, env);
        // Checked again: the construction and the log provider are host code,
        // and one that registers or attaches through `handleInline` runs that
        // request inline, on this strand, possibly filing this very key.
        if (attachExisting(dirKey, env, reply, cid)) {
            if (releaseCurrent.v != 0U) {
                releaseScoped(releaseCurrent, cid);
            }
            return;
        }
        ::morph::exec::detail::ModelId const fresh{nextOpaqueId()};
        if (releaseCurrent.v != 0U) {
            releaseScoped(releaseCurrent, cid);
        }
        LimitPolicy const& limits = _config.limits;
        if (limits.maxLiveModels != 0 && _instances.size() >= limits.maxLiveModels) {
            reply(::morph::wire::encode(::morph::wire::makeErr("too many models", env.callId)));
            return;
        }
        if (!noteScopeAttach(fresh, cid)) {
            reply(::morph::wire::encode(::morph::wire::makeErr("connection closed", env.callId)));
            return;
        }
        // Filed with one attachment, an empty owner (shared instances are
        // ownerless, by design -- see this function's doc comment) and its
        // hydration pending, so the first action's outcome decides whether a
        // second client may ever be handed it.
        _instances.insertShared(fresh, std::move(holder), std::move(dirKey));
        reply(::morph::wire::encode(::morph::wire::makeOk(env.callId, {}, fresh.v)));
    }

    /// @brief Files a live, still-anonymous instance under a primary key, in
    ///        place. On the server strand.
    ///
    /// The existing holder of a key always wins: promoting onto a key another
    /// instance already holds is a silent no-op rather than a displacement.
    /// Symmetrically, an instance that already holds a *different* real key is
    /// left exactly where it is — also a silent no-op — since instances never
    /// change key (docs/spec/core/shared_instances.md); only a `mid` that has
    /// never held a directory key can ever be promoted. This server hands every
    /// keyed instance its own key once, at construction, through
    /// `_registry.create(typeId, primary)` -> `IModelHolder::attachIdentity`,
    /// and never updates it — so an instance evicted from its key as poisoned
    /// stays ineligible even though the directory no longer files it anywhere.
    /// @param env Decoded request; uses `typeId`, `primary`, `modelId`.
    void applyAssign(const ::morph::wire::Envelope& env) {
        if (env.primary.empty()) {
            return;
        }
        // A no-op unless the instance is live, has never held a directory key,
        // and the key is free -- `InstanceDirectory::promote` holds the guards.
        (void)_instances.promote(::morph::exec::detail::ModelId{env.modelId},
                                 detail::DirectoryKey{env.typeId, env.primary});
    }

    /// @brief Names the fields `(modelType, actionType)`'s served schema marks
    ///        `required` that @p body does not carry a key for.
    ///
    /// The whole of `PayloadCompleteness::RequireDeclaredFields`. Presence is
    /// judged on the **key**, not on the decoded value, because that is the
    /// only question the action codec cannot answer: `fromJson` turns an
    /// absent field and an explicitly-sent zero into the same action, which is
    /// exactly why a renamed field survives `validate()`.
    ///
    /// Conservative in both directions. A `body` that is not a JSON object at
    /// all reports nothing missing — malformed JSON is `fromJson`'s error to
    /// raise, with a better message, a moment later — and an action with no
    /// published requirement (`requiredFieldsFor` returning `nullptr`)
    /// contributes no rule, since the gate enforces what the schema published
    /// and cannot enforce what it could not publish.
    ///
    /// @param modelType  Model type-id from the envelope.
    /// @param actionType Action type-id from the envelope.
    /// @param body       The `execute` envelope's opaque payload JSON.
    /// @return A comma-separated list of missing field names, or `""` when
    ///         nothing is missing or nothing is checkable.
    [[nodiscard]] std::string missingRequiredFields(const std::string& modelType, const std::string& actionType,
                                                    const std::string& body) const {
        const auto* required = _dispatcher.requiredFieldsFor(modelType, actionType);
        if (required == nullptr || required->empty()) {
            return {};
        }
        glz::generic_u64 dom{};
        if (glz::read_json(dom, body) || !dom.is_object()) {
            return {};
        }
        std::string missing;
        for (const auto& field : *required) {
            if (!dom.contains(field)) {
                if (!missing.empty()) {
                    missing += ", ";
                }
                missing += field;
            }
        }
        return missing;
    }

    /// @brief Answers an `instances` request with the live shared keys of a
    ///        type. On the server strand.
    /// @param env   Decoded request; uses `typeId` and `callId`.
    /// @param reply Reply sink; always invoked exactly once.
    void handleInstances(const ::morph::wire::Envelope& env, const std::function<void(std::string)>& reply) {
        std::string body;
        (void)glz::write_json(_instances.keysOfType(env.typeId), body);
        reply(::morph::wire::encode(::morph::wire::makeOk(env.callId, std::move(body))));
    }

    /// @brief Replies to an envelope that did not decode, and logs it. On the
    ///        server strand.
    ///
    /// The one server-side record of a request that never dispatched at all. A
    /// client that swallows its own error (or is malformed precisely because it
    /// is confused) would otherwise leave no trace here. The payload prefix is
    /// the most useful field for diagnosing *why* the client sent something
    /// malformed -- and the most likely to carry application data, so it is
    /// capped at kLogPayloadPreviewBytes rather than logged in full. See
    /// docs/spec/core/backend.md, "Server-side observability".
    /// @param msg   The raw message.
    /// @param error The decode exception's message.
    /// @param reply Reply sink.
    /// @param cid   Connection the message arrived on.
    static void replyUndecodable(const std::string& msg, const std::string& error,
                                 const std::function<void(std::string)>& reply, ConnectionId cid) {
        constexpr std::size_t kLogPayloadPreviewBytes = 256;
        std::string_view const preview =
            std::string_view{msg}.substr(0, std::min(msg.size(), kLogPayloadPreviewBytes));
        ::morph::log::logError(
            "[dispatchMessage] undecodable envelope from connection {}: {} ({} bytes, "
            "payload prefix: {}{})",
            cid, error, msg.size(), preview, msg.size() > kLogPayloadPreviewBytes ? "..." : "");
        reply(::morph::wire::encode(::morph::wire::makeErr(error)));
    }

    /// @brief Dispatches one decoded envelope, or replies to one that did not
    ///        decode. On the server strand: every envelope's body starts here.
    /// @param decoded The envelope, or its decode failure.
    /// @param reply   Reply sink; invoked exactly once, now or (for an admitted
    ///                `execute`) from the model's strand.
    /// @param cid     Connection scope; `0` means unscoped.
    void dispatchDecoded(Decoded decoded, std::function<void(std::string)>& reply, ConnectionId cid) {
        noteOwner("RemoteServer::dispatch");
        if (!decoded.env) {
            replyUndecodable(decoded.raw, decoded.error, reply, cid);
            return;
        }
        dispatchEnvelope(std::move(*decoded.env), reply, cid);
    }

    // One flat switch over the wire's `kind` discriminator. Splitting it would
    // scatter the authorization sequence each branch depends on across helpers,
    // with no reader benefit. On the server strand.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    void dispatchEnvelope(::morph::wire::Envelope env, std::function<void(std::string)>& reply, ConnectionId cid) {
        // One line per successfully-decoded request -- the point every kind
        // funnels through, so a client stuck mid-handshake (or one that never
        // sent anything) is distinguishable from one whose requests are
        // arriving normally. Deliberately omits the session principal (opt-in
        // territory: personal data in many deployments) and the payload body
        // (already covered, truncated, on the decode-failure path; logging every
        // successful body by default would be far higher volume and duplicate
        // what dispatch already records via the action log for execute).
        ::morph::log::logDebug(
            "[dispatchMessage] connection {}: kind={} callId={} typeId={} modelId={} "
            "modelType={} actionType={} bodyBytes={}",
            cid, env.kind, env.callId, env.typeId, env.modelId, env.modelType, env.actionType, env.body.size());
        // Once shutdown has begun, new work is rejected fast — before any other
        // validation runs — while `deregister` (and any other kind) still flows
        // through unchanged, so a client can still tear its models down cleanly
        // during the drain window.
        if ((env.kind == "register" || env.kind == "execute" || env.kind == "attach") && _shuttingDown) {
            reply(::morph::wire::encode(::morph::wire::makeErr("server shutting down", env.callId)));
            return;
        }
        try {
            if (env.kind == "register") {
                ::morph::observe::detail::emitMetric(::morph::observe::Metric::registerCount, 1.0);
                if (env.typeId.empty()) {
                    throw std::runtime_error("register requires a typeId");
                }
                LimitPolicy const& limits = _config.limits;
                // Cheap early rejection, so a server already at its cap does not
                // pay for authorize()/authenticate() and a model construction it
                // is about to discard.
                //
                // Skipped for a *shared* register, which may well create nothing:
                // if the key is already live it only takes another reference, and
                // `maxLiveModels` caps live models, not attachments to them.
                if (limits.maxLiveModels != 0 && (!env.shared || env.primary.empty()) &&
                    _instances.size() >= limits.maxLiveModels) {
                    reply(::morph::wire::encode(::morph::wire::makeErr("too many models", env.callId)));
                    return;
                }
                // Authenticate the caller and make the verified identity
                // authoritative, exactly as dispatchExecute does for execute: a
                // verifying authorizer's returned principal overwrites
                // env.session.principal; a non-authenticating authorizer
                // (including allow-all) clears it, so the register decision
                // below — and the owner recorded from it — never key on the
                // client's unverified claim.
                stampVerifiedPrincipal(env);
                // Bound *who may create* an instance. The default hook allows
                // all, so an unconfigured server registers any known type; a
                // deployer opts into gating registration by overriding
                // authorizeRegister. No instance is constructed on denial.
                if (!_authorizer->authorizeRegister(env.session, env.typeId)) {
                    reply(::morph::wire::encode(::morph::wire::makeErr("unauthorized", env.callId)));
                    return;
                }
                // A `shared` register naming a primary is a register-or-attach
                // against the directory; everything below is the private path.
                if (env.shared && !env.primary.empty()) {
                    acquireSharedInstance(env, reply, cid, ::morph::exec::detail::ModelId{0});
                    return;
                }
                // env.primary here is either empty (a private/anonymous instance
                // -- attachIdentity is then a no-op) or a caller-supplied
                // identity for a non-shared instance, which is still this
                // instance's own key to learn.
                auto holder = _registry.create(env.typeId, env.primary);
                attachLogIfConfigured(*holder, env);
                // find(), never operator[]: the scope may already be gone. A
                // client that registers and immediately drops its socket
                // closes its scope before this register runs, and operator[]
                // would resurrect it -- a scope nothing closes a second time, so
                // this model and every later one on the dead cid would be
                // unreclaimable, and with `maxLiveModels` set the server would
                // wedge at `err "too many models"`.
                if (cid != 0 && !_connectionScopes.contains(cid)) {
                    reply(::morph::wire::encode(::morph::wire::makeErr("connection closed", env.callId)));
                    return;
                }
                // Re-tested after the construction, which is host code: a model
                // constructor that registers another model through
                // `handleInline` runs that register inline, on this strand.
                if (limits.maxLiveModels != 0 && _instances.size() >= limits.maxLiveModels) {
                    reply(::morph::wire::encode(::morph::wire::makeErr("too many models", env.callId)));
                    return;
                }
                ::morph::exec::detail::ModelId const mid{nextOpaqueId()};
                if (cid != 0) {
                    _connectionScopes.at(cid)[mid] += 1;
                }
                // Private: no directory key, no sharing, no hydration tracking.
                // The owner is the principal stamped above, never the client's
                // raw claim; it is what lets `authorizeInstance` later deny a
                // different principal.
                _instances.insertPrivate(mid, std::move(holder), std::move(env.session.principal));
                reply(::morph::wire::encode(::morph::wire::makeOk(env.callId, {}, mid.v)));
            } else if (env.kind == "attach") {
                if (env.typeId.empty()) {
                    throw std::runtime_error("attach requires a typeId");
                }
                stampVerifiedPrincipal(env);
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
                stampVerifiedPrincipal(env);
                if (!_authorizer->authorizeRegister(env.session, env.typeId)) {
                    reply(::morph::wire::encode(::morph::wire::makeErr("unauthorized", env.callId)));
                    return;
                }
                applyAssign(env);
                reply(::morph::wire::encode(::morph::wire::makeOk(env.callId, {}, env.modelId)));
            } else if (env.kind == "instances") {
                if (env.typeId.empty()) {
                    throw std::runtime_error("instances requires a typeId");
                }
                stampVerifiedPrincipal(env);
                // Enumeration is a read channel over the directory: gate it with
                // `authorize` for the model type (empty action id) so a deployer
                // can refuse listing without refusing use. It discloses the live
                // key set to anyone admitted — see docs/spec/security.md.
                if (!_authorizer->authorize(env.session, env.typeId, {})) {
                    reply(::morph::wire::encode(::morph::wire::makeErr("unauthorized", env.callId)));
                    return;
                }
                handleInstances(env, reply);
            } else if (env.kind == "schemas") {
                if (env.typeId.empty()) {
                    throw std::runtime_error("schemas requires a typeId");
                }
                stampVerifiedPrincipal(env);
                // Gated exactly like `instances`, and for the same reason: a
                // description is a read channel over the model type, so
                // `authorize` with an empty action id lets a deployer refuse
                // *describing* a type without refusing *using* it. It is a
                // real disclosure — field names, bounds, rules and the
                // payload fingerprint of every action — so it must not be
                // reachable by a caller the server would not let execute.
                if (!_authorizer->authorize(env.session, env.typeId, {})) {
                    reply(::morph::wire::encode(::morph::wire::makeErr("unauthorized", env.callId)));
                    return;
                }
                reply(::morph::wire::encode(::morph::wire::makeOk(env.callId, _dispatcher.schemasJson(env.typeId))));
            } else if (env.kind == "deregister") {
                ::morph::observe::detail::emitMetric(::morph::observe::Metric::deregisterCount, 1.0);
                ::morph::exec::detail::ModelId const mid{env.modelId};
                // Per-instance authorization also gates deregister: consult the
                // hook with the recorded owner before destroying the instance.
                // The default hook allows all, so unconfigured behaviour is
                // unchanged; an ownership-enforcing authorizer can reject a
                // caller tearing down an instance it does not own.
                if (const auto* inst = _instances.find(mid);
                    inst != nullptr && !_authorizer->authorizeInstance(env.session, {}, {}, mid.v, inst->owner)) {
                    reply(::morph::wire::encode(::morph::wire::makeErr("unauthorized", env.callId)));
                    return;
                }
                // Release exactly the reference *this* connection (`cid`, the
                // scope the deregister request itself carries) holds, not
                // whichever connection happened to attach the instance last --
                // a shared instance may have several owning connections at once,
                // and crediting the release to the wrong one either strands a
                // reference nobody will ever decrement, or lets one connection's
                // deregister silently consume another's hold.
                releaseScoped(mid, cid);
                reply(::morph::wire::encode(::morph::wire::makeOk(env.callId)));
            } else if (env.kind == "execute") {
                dispatchExecute(std::move(env), reply);
            } else if (env.kind == "hello") {
                const std::uint32_t minV = _config.minProtocolVersion;
                const std::uint32_t maxV = _config.maxProtocolVersion;
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
            // Any throw from the branches above — including one out of
            // `dispatchExecute`'s `authorize`/`authenticate`/`authorizeInstance`/
            // `missingRequiredFields` steps, all reachable, non-`noexcept` code
            // — becomes this call's `err` reply. Nothing is held across it: a
            // later execute for the same model is simply the next task on this
            // strand.
            reply(::morph::wire::encode(::morph::wire::makeErr(exc.what(), env.callId)));
        }
    }

    // Admission: one ordered gate sequence — limits, authorize, authenticate,
    // lookup, per-instance authorize, payload completeness, in-flight
    // reservation — whose *order* is the security contract itself (see
    // docs/spec/security.md), so it is deliberately not broken up. On the
    // server strand; the admitted run is posted to the model's strand.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    void dispatchExecute(::morph::wire::Envelope env, std::function<void(std::string)> reply) {
        noteOwner("RemoteServer::admitExecute");
        auto reject = [&env, &reply](const char* message) {
            reply(::morph::wire::encode(::morph::wire::makeErr(message, env.callId)));
        };
        LimitPolicy const& limits = _config.limits;
        // Exact, not advisory: the count is this strand's own state, so nothing
        // can be admitted between this check and the increment below.
        if (limits.maxInFlightExecutes != 0 && _inFlight >= limits.maxInFlightExecutes) {
            reject("server busy");
            return;
        }
        if (!_authorizer->authorize(env.session, env.modelType, env.actionType)) {
            reject("unauthorized");
            return;
        }
        // Make the identity authoritative. A verifying authorizer returns the
        // principal it extracted from a valid token; we stamp it so model code
        // reading session::current()->principal can trust it. If authenticate()
        // returns nullopt the authorizer cannot vouch for the caller, so we CLEAR
        // the client-asserted principal rather than passing it through unverified.
        // This closes two holes: (1) the TOCTOU window where a token that passed
        // authorize() expires before authenticate() (worst case is an empty
        // principal, never the attacker's claim), and (2) an authorize-only or
        // allow-all authorizer that never authenticates (the model never sees an
        // untrusted principal as authoritative). See docs/spec/security.md.
        stampVerifiedPrincipal(env);
        ::morph::exec::detail::ModelId const mid{env.modelId};
        // One lookup for the holder, its recorded owner and its hydration state
        // together: they are fields of one record.
        const auto* inst = _instances.find(mid);
        if (inst == nullptr) {
            // Answered in this task's turn: the server strand runs no handler,
            // so a lookup of a model that is gone never waits on a model that
            // is busy.
            reject("model not found");
            return;
        }
        std::shared_ptr<::morph::model::detail::IModelHolder> holder = inst->holder;
        std::shared_ptr<detail::HydrationState> hydration = inst->hydration;
        // Per-instance (row-level) authorization. `authorize` above only saw the
        // model *type*; this consults the optional ownership hook with the target
        // instance id and its recorded owner. The default hook allows all.
        // env.session carries the verified principal (stamped just above), so an
        // ownership authorizer compares the recorded owner against it.
        if (!_authorizer->authorizeInstance(env.session, env.modelType, env.actionType, mid.v, inst->owner)) {
            reject("unauthorized");
            return;
        }
        // Action-evolution policy gate (opt-in; see `PayloadCompleteness`).
        // After authorization — the diagnostic names the action's own field,
        // which is exactly what the `"schemas"` kind discloses and is therefore
        // owed the same gate — and before the in-flight slot is reserved, so a
        // payload that will not be dispatched never consumes one.
        if (_config.payloadCompleteness == PayloadCompleteness::RequireDeclaredFields) {
            const std::string missing = missingRequiredFields(env.modelType, env.actionType, env.body);
            if (!missing.empty()) {
                const std::string message = "payload missing required field(s): " + missing;
                reject(message.c_str());
                return;
            }
        }
        // Reserve the in-flight slot. From here the call is answered exactly
        // once, by `complete`, whichever of the model strand's finish and the
        // `executeTimeout` gets there first; `complete` posts the matching
        // `executeFinished` back to this strand.
        _inFlight += 1;
        ::morph::observe::detail::emitMetric(::morph::observe::Metric::executeInFlight,
                                             static_cast<double>(_inFlight));
        auto self = shared_from_this();
        auto finished = std::make_shared<std::atomic_flag>();
        // Gives the slot back if this frame leaves by exception before the run
        // is handed to the model's strand: dispatchEnvelope's catch replies on
        // that path, so this only claims `finished` (making an armed timeout a
        // no-op) and decrements, here, on the strand.
        struct Reservation {
            RemoteServer& server;
            std::shared_ptr<std::atomic_flag> finished;
            bool handedOff = false;
            Reservation(RemoteServer& owner, std::shared_ptr<std::atomic_flag> flag)
                : server{owner}, finished{std::move(flag)} {}
            Reservation(const Reservation&) = delete;
            Reservation& operator=(const Reservation&) = delete;
            Reservation(Reservation&&) = delete;
            Reservation& operator=(Reservation&&) = delete;
            ~Reservation() {
                if (!handedOff && !finished->test_and_set()) {
                    server.executeFinished();
                }
            }
        } reservation{*this, finished};

        std::uint64_t const callId = env.callId;
        auto replySlot = std::make_shared<std::function<void(std::string)>>(std::move(reply));
        // The decrement is posted before the reply goes out, so anything posted
        // to the server strand by someone who has seen the reply runs after it.
        auto complete = [self, finished, replySlot](std::string msg) {
            if (!finished->test_and_set()) {
                self->_strand.postTask([self] { self->executeFinished(); });
                (*replySlot)(std::move(msg));
            }
        };

        ::morph::async::detail::TimeoutScheduler::Handle timeoutHandle{};
        // Requested by the timeout alongside its reply, so a Task handler still
        // suspended when the caller is answered unwinds and leaves the action
        // gate rather than holding the model until its await completes.
        std::shared_ptr<::core::async::StopSource> stopSource;
        if (_executeTimeouts) {
            stopSource = std::make_shared<::core::async::StopSource>();
            // Safe to fire after its own `cancel()`, which the model strand's
            // finish issues on both its success and its failure arm:
            // `TimeoutScheduler::cancel` stops a callback that has not started
            // but returns without waiting for one that already has. A timeout
            // callback already mid-flight when the dispatch finishes therefore
            // still calls `complete`, and `complete`'s reply-exactly-once flag
            // drops it rather than double-answering the call.
            timeoutHandle =
                _executeTimeouts->schedule(limits.executeTimeout, [complete, callId, stopSource]() mutable {
                    complete(::morph::wire::encode(::morph::wire::makeErr("timeout", callId)));
                    stopSource->request_stop();
                });
        }

        RemoteRun run;
        run.self = self;
        run.env = std::move(env);
        run.holder = std::move(holder);
        run.hydration = std::move(hydration);
        run.complete = complete;
        run.timeoutHandle = timeoutHandle;
        run.stopSource = std::move(stopSource);
        run.mid = mid;
        // Posted in this strand's order, which is the order `handle()` was
        // called in; the model's strand runs its tasks in the order they were
        // posted. That is the whole of per-model execute ordering.
        //
        // Through the instance's action gate: an action starts only once the one
        // before it has finished, which a Task handler does when its Task
        // completes rather than when the strand task that started it returns.
        if (_dispatcher.dispatchesAsync(run.env.modelType, run.env.actionType)) {
            // A Task run is shared: its completion callback outlives the strand
            // task.
            auto shared = std::make_shared<RemoteRun>(std::move(run));
            _strands->post(mid,
                           [shared] { shared->holder->actionGate().enter([shared] { startTaskRemote(shared); }); });
        } else {
            // An ordinary run travels by value in the strand task, so an
            // execute costs the post's one allocation, as LocalBackend's does.
            // See `ActionGate::tryEnter`.
            _strands->post(mid, [run = std::move(run)]() mutable {
                auto& gate = run.holder->actionGate();
                if (gate.tryEnter()) {
                    startRemote(run);
                    return;
                }
                gate.enter([waiting = std::make_shared<RemoteRun>(std::move(run))] { startRemote(*waiting); });
            });
        }
        // The model strand's task now owns `complete`, so the in-flight slot is
        // its responsibility rather than this frame's.
        reservation.handedOff = true;
    }

    /// @brief Returns the next opaque model id. On the server strand.
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
            id = _idGen.permute(++_nextId);
        } while (id == 0);
        return ::morph::exec::detail::ModelId{id};
    }

    /// Everything one dispatched execute carries from `dispatchExecute` to its
    /// reply. An ordinary handler's run is held by its model-strand task, or by
    /// the action gate's queue while it waits there; a Task handler's is
    /// shared, because its completion callback holds it too. Holding `self` is
    /// what keeps the server, and with it `_dispatcher`, alive until the reply
    /// is delivered.
    struct RemoteRun {
        std::shared_ptr<RemoteServer> self;
        ::morph::wire::Envelope env;
        std::shared_ptr<::morph::model::detail::IModelHolder> holder;
        std::shared_ptr<detail::HydrationState> hydration;
        std::function<void(std::string)> complete;
        ::morph::async::detail::TimeoutScheduler::Handle timeoutHandle{};
        /// Null without an `executeTimeout`.
        std::shared_ptr<::core::async::StopSource> stopSource;
        ::morph::exec::detail::ModelId mid{};
        std::chrono::steady_clock::time_point start;
        ::morph::observe::SpanId spanId{};
    };

    /// Stamps an execute's start once it holds its instance's action gate.
    ///
    /// Metrics and endSpan are recorded before `complete(...)` runs (in
    /// finishRemote) so a caller observing completion — via handle()'s reply
    /// or the timeout path racing it — can never see the reply before this
    /// dispatch's own instrumentation is recorded. This mirrors the
    /// reply-exactly-once contract `complete` already provides: whichever
    /// path wins the race, the metrics for *this* dispatch are always
    /// emitted, exactly once, regardless of which path's reply the caller
    /// actually receives.
    ///
    /// A handler whose decoded action fails ActionValidator<Action>::ready(...)
    /// throws morph::model::ValidationError before Model::execute runs. No
    /// special-casing is needed: it becomes an ordinary `err` reply carrying its
    /// message and callId, exactly like any other dispatch failure. See
    /// docs/spec/core/registry.md.
    static void admitRemote(RemoteRun& run) {
        run.start = std::chrono::steady_clock::now();
        run.spanId =
            ::morph::observe::detail::beginSpan(run.env.session.requestId, run.env.modelType, run.env.actionType);
    }

    /// Runs an ordinary handler's execute once it holds its instance's action
    /// gate, on the strand, and replies.
    static void startRemote(RemoteRun& run) {
        admitRemote(run);
        std::string result;
        std::exception_ptr error;
        try {
            ::morph::session::detail::ScopedContext const scoped{run.env.session};
            result = run.self->_dispatcher.dispatch(run.env.modelType, run.env.actionType, *run.holder, run.env.body);
        } catch (...) {
            error = std::current_exception();
        }
        finishRemote(run, std::move(result), error);
    }

    /// Starts a Task handler's execute once it holds its instance's action
    /// gate, on the strand. It replies when its Task completes.
    static void startTaskRemote(const std::shared_ptr<RemoteRun>& run) {
        admitRemote(*run);
        std::shared_ptr<::morph::exec::detail::TaskResumer> executor;
        try {
            ::morph::session::detail::ScopedContext const scoped{run->env.session};
            auto const& strands = run->self->_strands;
            executor = std::make_shared<::morph::exec::detail::TaskResumer>(strands, run->mid, run->env.session);
            strands->enroll(run->mid, executor);
            auto token = run->stopSource ? run->stopSource->get_token() : ::core::async::StopToken{};
            run->self->_dispatcher.dispatchAsync(
                run->env.modelType, run->env.actionType, *run->holder, run->env.body, executor, std::move(token),
                [run, resumer = executor.get()](std::string result, std::exception_ptr error) {
                    // As on LocalBackend: a handler can end on another
                    // executor, and leaving the gate belongs on the strand.
                    // The strands are held here, not reached through the run:
                    // the finish may release the last reference to this server.
                    auto const held = run->self->_strands;
                    held->withdraw(run->mid, resumer);
                    held->runOnStrand(run->mid, [run, result = std::move(result), error = std::move(error)] {
                        finishRemote(*run, result, error);
                    });
                });
        } catch (...) {
            run->self->_strands->withdraw(run->mid, executor.get());
            finishRemote(*run, std::string{}, std::current_exception());
        }
    }

    /// Records a finished execute, replies, and leaves the action gate so the
    /// next execute on the instance can start. On the strand.
    static void finishRemote(RemoteRun& run, std::string result, const std::exception_ptr& error) {
        auto& self = *run.self;
        if (self._executeTimeouts) {
            self._executeTimeouts->cancel(run.timeoutHandle);
        }
        bool const succeeded = error == nullptr;
        // Settle hydration before `endSpan`, before the metrics and before
        // `complete` -- each hands control to host code that is free to attach
        // to this instance's key, and an attacher reaching the directory while
        // the outcome is known but unrecorded is handed an instance whose first
        // action has already failed. See shared_instances.md's Failure modes.
        if (run.hydration) {
            run.hydration->settle(succeeded);
        }
        ::morph::observe::detail::endSpan(run.spanId, succeeded);
        auto const elapsedMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - run.start).count();
        std::array<std::pair<std::string_view, std::string_view>, 2> const tags{
            {{"modelType", run.env.modelType}, {"actionType", run.env.actionType}}};
        ::morph::observe::detail::emitMetric(::morph::observe::Metric::executeLatencyMs, elapsedMs, tags);
        if (succeeded) {
            run.complete(::morph::wire::encode(::morph::wire::makeOk(run.env.callId, std::move(result))));
        } else {
            ::morph::observe::detail::emitMetric(::morph::observe::Metric::executeErrors, 1.0, tags);
            std::string message = "unknown exception";
            try {
                std::rethrow_exception(error);
            } catch (const std::exception& exc) {
                message = exc.what();
            } catch (...) {  // NOLINT(bugprone-empty-catch): the message stays "unknown exception"
                // Keeps "unknown exception": the reply still goes out, so the
                // caller's completion does not wait for its deadline.
            }
            run.complete(::morph::wire::encode(::morph::wire::makeErr(message, run.env.callId)));
        }
        run.holder->actionGate().leave();
    }

    ::morph::exec::IExecutor& _pool;
    // The server strand. Declared before every member its tasks touch, and
    // closed first, in the destructor's body.
    ::morph::exec::OwnerStrand _strand;
    // One strand per model instance, shared with the Task handlers' resumers.
    // Never closed before its last owner goes: a suspended handler's driver
    // frame holds its `RemoteRun`, and with it this server.
    std::shared_ptr<::morph::exec::detail::ModelStrands> _strands;
    ::morph::model::detail::ActionDispatcher& _dispatcher;
    ::morph::model::detail::ModelRegistryFactory& _registry;
    std::shared_ptr<::morph::session::IAuthorizer> _authorizer;
    // Fixed at construction; read on the server strand and on model strands
    // without a lock because nothing writes it afterwards.
    ServerConfig const _config;
    // Arms `LimitPolicy::executeTimeout`; present only when one is configured.
    // Fixed at construction, so a model strand's finish can cancel through it
    // without a lock.
    std::unique_ptr<::morph::async::detail::TimeoutScheduler> const _executeTimeouts;

    // ── Server-strand state: touched only in the server strand's tasks ──────
    // Every live instance, private and shared alike, plus the shared-instance
    // directory over them — holder, owner principal, attach count, directory key
    // and hydration state as one record per instance, and the same type
    // LocalBackend owns. `HydrationState` is the one part with synchronisation
    // of its own: it is settled from the model strand's finish, reached through
    // a `shared_ptr` captured at admission rather than by looking the instance
    // up again.
    detail::InstanceDirectory _instances;
    // Connection-scope bookkeeping (opt-in; see openConnection/closeConnection
    // and the scoped handle(msg, reply, cid) overload). The value is a *count*
    // per instance, not a set: one connection may attach the same shared
    // instance from two handlers, and closing the connection must release both
    // references or the instance leaks. A private instance always has a count
    // of exactly 1.
    std::unordered_map<ConnectionId, std::unordered_map<::morph::exec::detail::ModelId, std::size_t,
                                                        ::morph::exec::detail::ModelIdHash>>
        _connectionScopes;
    std::uint64_t _nextId{0};
    detail::OpaqueIdGenerator _idGen;
    // Admitted executes whose reply has not been sent: incremented at
    // admission, decremented by `executeFinished`, which each reply posts here.
    // What `maxInFlightExecutes` gates, `health()` reports, the
    // `executeInFlight` metric carries and `drainedWithin()` waits on.
    std::size_t _inFlight{0};
    // Set by beginShutdown() and never cleared.
    bool _shuttingDown{false};
    // `health()`'s `ready`; cleared by beginShutdown() and never set again.
    bool _ready{true};
    /// One `drainedWithin()` call still waiting for the in-flight count to reach zero.
    struct DrainWaiter {
        std::uint64_t id;
        DrainPromise promise;
        ::morph::async::detail::TimeoutScheduler::Handle timer;
    };
    std::vector<DrainWaiter> _drainWaiters;
    std::uint64_t _nextDrainWaiter{0};
    // Arms drain deadlines; created by the first waiter that needs one.
    std::unique_ptr<::morph::async::detail::TimeoutScheduler> _drainTimer;

    // Drawn off the strand by openConnection(), which answers at once.
    std::atomic<uint64_t> _nextConnectionId{0};
};

/// @brief `IBackend` adapter that routes all calls through a `RemoteServer` as
///        wire `Envelope` messages.
///
/// Intended for testing and in-process simulation of remote execution.
/// Control envelopes (`bindModel`, `registerModel`, `deregisterModel`, ...) are
/// processed through `RemoteServer::handleInline`, which answers before it
/// returns, so a bind settles inside the call. `execute()` is asynchronous: it
/// sends the message through `RemoteServer::handle` and resolves the returned
/// `Completion` when the reply arrives.
///
/// Its pending list and session belong to the caller's owner (`setOwner`) and
/// are touched only there, without a lock; the server's reply callback touches
/// only the completion it settles.
class SimulatedRemoteBackend : public detail::IBackend {
public:
    /// @brief Constructs the backend targeting @p server, unscoped.
    ///
    /// Every `register`/`deregister`/`attach`/`assign`/`instances` call this
    /// backend makes uses `ConnectionId{0}` — the server's "unscoped" sentinel
    /// (`backend.md`, "Connection scopes"). Use the `ConnectionId` constructor
    /// below to give this backend its own scope instead, so tests can
    /// exercise connection-scoped state (rate limiting, connection-drop
    /// recovery, shared-instance attach/detach across connections)
    /// deterministically without a real socket.
    /// @param server The `RemoteServer` instance to forward calls to. Borrowed,
    ///               not owned: it must outlive this backend — a backend holding
    ///               a `RemoteServer&` that has been destroyed is a
    ///               use-after-free on every call (see
    ///               `docs/spec/concurrency_and_lifetimes.md`, "Destruction
    ///               ordering").
    explicit SimulatedRemoteBackend(RemoteServer& server MORPH_LIFETIMEBOUND) : _server{server} {}

    /// @brief Constructs the backend targeting @p server, scoped to @p cid.
    ///
    /// @p cid must have been obtained from `server.openConnection()` — the
    /// same connection-scope mechanism `QtWebSocketServer`/`morph::net::SocketServer`
    /// use for a real transport, made available for in-process simulation. Every
    /// `register`/`attach`/`assign`/`instances` call this backend makes is
    /// attributed to @p cid's scope (via the three-argument `RemoteServer::handle`/
    /// `handleInline` overloads), so a `deregisterModel`/destructor-driven
    /// `closeConnection` reclaims exactly what this backend registered — the
    /// same reclamation guarantee a dropped socket gives a real client.
    ///
    /// The backend does **not** call `closeConnection` itself on destruction:
    /// unlike a real transport, there is no single well-defined "this
    /// connection is gone" moment to hook here (the caller may want to keep
    /// the scope open past this object's lifetime, e.g. to construct a
    /// second `SimulatedRemoteBackend` against the same scope). Call
    /// `closeConnection(cid)` explicitly, or destroy `server` itself, when
    /// the simulated connection should be reclaimed.
    /// @param server The `RemoteServer` instance to forward calls to. Borrowed,
    ///               on the same terms as the constructor above.
    /// @param cid    Connection scope, as returned by `server.openConnection()`.
    SimulatedRemoteBackend(RemoteServer& server MORPH_LIFETIMEBOUND, ConnectionId cid) : _server{server}, _cid{cid} {}

    /// @brief Registers the model type on the server and returns its assigned id.
    ///
    /// Processed through `RemoteServer::handleInline`, which answers before it
    /// returns. The @p factory argument is ignored — model construction is
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
        note("SimulatedRemoteBackend::registerModelWithContext");
        auto env = ::morph::wire::makeRegister(typeId, std::string{contextKey});
        env.session = _session;
        auto reply = ::morph::wire::decode(_server.handleInline(::morph::wire::encode(env), _cid));
        if (reply.kind == "ok") {
            return ::morph::exec::detail::ModelId{reply.modelId};
        }
        throw std::runtime_error("register failed: " + reply.message);
    }

    /// @brief Acquires an instance on the server for @p request and settles
    ///        the returned `Completion` before returning.
    ///
    /// Each shape of `BindRequest` is one envelope, processed through
    /// `RemoteServer::handleInline` on the calling thread: a private
    /// `register`, a shared `register` (register-or-attach), or an `attach`
    /// that re-points from `current` in one request, so a re-pointing client
    /// cannot lose its slot to `LimitPolicy::maxLiveModels` between releasing
    /// the old instance and acquiring the new one. An empty `primary` with a
    /// live `current` releases it first and binds a private instance.
    /// @param request Owning bind request; moved from.
    /// @param cbExec  Executor the continuation is delivered on. Borrowed: it
    ///                must outlive the returned `Completion`.
    /// @return A settled `Completion`: the bound id, or the server's refusal as
    ///         a `std::runtime_error`.
    ::morph::async::Completion<::morph::exec::detail::ModelId> bindModel(detail::BindRequest request,
                                                                         ::morph::exec::IExecutor& cbExec) override {
        note("SimulatedRemoteBackend::bindModel");
        auto [completion, promise] =
            ::morph::async::Completion<::morph::exec::detail::ModelId>::makeSettleable(&cbExec);
        try {
            promise.resolve(bindNow(request));
        } catch (...) {
            promise.reject(std::current_exception());
        }
        return std::move(completion);
    }

    /// @brief Records the owner every verb is called from.
    /// @param owner The caller's owner.
    void setOwner(const ::morph::exec::detail::OwnerAffinity& owner) override { _affinity.emplace(owner); }

    /// @brief Files a live server-side instance under @p primary.
    /// @param mid     Live instance to promote.
    /// @param typeId  Model type id.
    /// @param primary Canonical string encoding of the key to file it under.
    void assignPrimary(::morph::exec::detail::ModelId mid, const std::string& typeId,
                       std::string_view primary) override {
        note("SimulatedRemoteBackend::assignPrimary");
        if (primary.empty() || mid.v == 0U) {
            return;
        }
        auto env = ::morph::wire::makeAssign(typeId, std::string{primary}, mid.v);
        env.session = _session;
        (void)_server.handleInline(::morph::wire::encode(env), _cid);
    }

    /// @brief Asks the server for the live shared primary keys of @p typeId.
    /// @param typeId String type-id to enumerate.
    /// @return Canonical key strings of the live shared instances.
    /// @throws std::runtime_error if the server replies with an error.
    std::vector<std::string> listInstances(const std::string& typeId) override {
        note("SimulatedRemoteBackend::listInstances");
        auto reply = ::morph::wire::decode(
            _server.handleInline(::morph::wire::encode(::morph::wire::makeInstances(typeId)), _cid));
        if (reply.kind != "ok") {
            throw std::runtime_error("instances failed: " + reply.message);
        }
        std::vector<std::string> keys;
        // Not covered by this file's own test suite: `_server` here is always
        // the same in-process `RemoteServer` this backend was constructed
        // against, and `handleInstances` (this file, above) only ever builds
        // `reply.body` via `glz::write_json` of a `std::vector<std::string>`
        // populated straight from the instance directory's own keys -- themselves
        // `env.primary` values that already round-tripped through a
        // successful envelope decode, so they are valid UTF-8 by
        // construction. There is no path through `SimulatedRemoteBackend` +
        // `RemoteServer` that hands this call a syntactically malformed
        // body, so this branch cannot be forced without directly fabricating
        // a fake "ok" reply -- not a real scenario for this backend. The
        // identical decode-failure shape is genuinely reachable (and
        // covered separately) on a backend that talks to an actual external
        // peer -- see `QtWebSocketBackend::listInstances`
        // (qt_websocket_backend.hpp) and `socket_backend.hpp`'s own
        // equivalent, both of which face a real wire and a peer this
        // process does not control.
        if (auto errCode = glz::read_json(keys, reply.body)) {
            throw std::runtime_error("instances decode failed: " + glz::format_error(errCode, reply.body));
        }
        return keys;
    }

    /// @brief Deregisters the model on the server. Processed inline.
    ///
    /// Carries this backend's own `ConnectionId`, so a shared instance's
    /// attach count is decremented against *this* backend's scope entry —
    /// never a different connection's — exactly as a real transport's
    /// `deregister` does (`backend.md`, "Connection scopes").
    /// @param mid Id of the model to deregister.
    void deregisterModel(::morph::exec::detail::ModelId mid) override {
        note("SimulatedRemoteBackend::deregisterModel");
        auto env = ::morph::wire::makeDeregister(mid.v);
        env.session = _session;
        (void)_server.handleInline(::morph::wire::encode(env), _cid);
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

    /// @brief Fetches the server's `{actionType: schema}` document for @p typeId.
    ///
    /// Sends a `"schemas"` envelope over the same synchronous control path as
    /// `registerModel`/`negotiateProtocolVersion` (`handleInline`), carrying
    /// the current session so an authorizing server can refuse. Opt-in in the
    /// same sense `negotiateProtocolVersion()` is: nothing calls it for you.
    ///
    /// The document is what a client that is **not** linked against the
    /// model's C++ needs — each action's properties, `required` array, form
    /// metadata, and its `x-payloadFingerprint`/`x-payloadShape`. A client
    /// that *is* linked against it can compare that fingerprint against its
    /// own `morph::model::payloadFingerprint<A>()` to detect a build skew.
    ///
    /// @param typeId Model type id to describe.
    /// @return The raw `{actionType: schema}` JSON document.
    /// @throws std::runtime_error if the server replies `err` (e.g.
    ///         `"unauthorized"`, or `"unknown envelope kind: schemas"` from a
    ///         server predating this kind).
    [[nodiscard]] std::string fetchActionSchemas(std::string typeId) {
        auto env = ::morph::wire::makeSchemas(std::move(typeId));
        env.session = _session;
        auto reply = ::morph::wire::decode(_server.handleInline(::morph::wire::encode(env)));
        if (reply.kind != "ok") {
            throw std::runtime_error("schemas request failed: " +
                                     (reply.message.empty() ? std::string{"malformed reply"} : reply.message));
        }
        return reply.body;
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
        note("SimulatedRemoteBackend::execute");
        auto state = std::make_shared<::morph::async::detail::CompletionState<std::shared_ptr<void>>>();
        ::morph::async::Completion<std::shared_ptr<void>> comp{state, cbExec};
        trackPending(state);

        ::morph::wire::Envelope env;
        env.kind = "execute";
        env.modelId = mid.v;
        env.modelType = call.modelTypeId;
        env.actionType = call.actionTypeId;
        env.body = call.serializeBody();
        env.session = std::move(call.session);
        auto deser = std::move(call.deserializeResult);

        _server.handle(
            ::morph::wire::encode(env),
            [state, deser = std::move(deser)](const std::string& replyJson) mutable {
                try {
                    auto reply = ::morph::wire::decode(replyJson);
                    // Triage shared with net::SocketBackend and
                    // QtWebSocketBackend (detail/reply_router.hpp), so the
                    // three cannot drift apart. The error text stays
                    // backend-specific: this one substitutes "malformed reply"
                    // for an empty message, which the socket backends do not.
                    switch (detail::classifyExecuteReply(reply)) {
                        case detail::ExecuteReplyKind::Value:
                            state->setValue(deser(reply.body));
                            break;
                        case detail::ExecuteReplyKind::Timeout:
                            throw TimeoutError{};
                        case detail::ExecuteReplyKind::Error:
                        default:
                            // `default:` only because the project builds with
                            // -Wswitch-default; every enumerator of the closed
                            // ExecuteReplyKind is handled explicitly above.
                            throw std::runtime_error(reply.message.empty() ? "malformed reply" : reply.message);
                    }
                } catch (...) {
                    state->setException(std::current_exception());
                }
            },
            _cid);
        return comp;
    }

    /// @brief No-op — models live in `RemoteServer`, not locally.
    void notifyBackendChanged() override {}

    /// @brief Resolves every still-pending completion this backend produced with @p exc.
    /// @param exc Exception delivered to every pending completion's error sink.
    void cancelPending(const std::exception_ptr& exc) override {
        note("SimulatedRemoteBackend::cancelPending");
        auto snapshot = std::exchange(_pending, {});
        for (auto& weak : snapshot) {
            if (auto state = weak.lock()) {
                state->setException(exc);
            }
        }
    }

    /// @brief Installs the session stamped onto every control envelope this
    ///        backend subsequently builds (`register`, `registerShared`,
    ///        `attach`, `assign`, `deregister`). See `IBackend::setSession`.
    /// @param session Session to stamp; typically pushed by `Bridge::setDefaultSession()`.
    void setSession(::morph::session::Context session) override {
        note("SimulatedRemoteBackend::setSession");
        _session = std::move(session);
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

    /// @brief `bindModel`'s body: one control envelope per request shape.
    /// @param request The bind request.
    /// @return The bound id.
    /// @throws std::runtime_error if the server replies with an error.
    ::morph::exec::detail::ModelId bindNow(const detail::BindRequest& request) {
        if (request.primary.empty()) {
            if (request.current.v != 0U) {
                deregisterModel(request.current);
            }
            return registerModelWithContext(request.typeId, {}, request.contextKey);
        }
        auto env =
            request.current.v == 0U
                ? ::morph::wire::makeRegisterShared(request.typeId, request.primary, request.contextKey)
                : ::morph::wire::makeAttach(request.typeId, request.primary, request.current.v, request.contextKey);
        env.session = _session;
        auto reply = ::morph::wire::decode(_server.handleInline(::morph::wire::encode(env), _cid));
        if (reply.kind == "ok") {
            return ::morph::exec::detail::ModelId{reply.modelId};
        }
        throw std::runtime_error((request.current.v == 0U ? "register failed: " : "attach failed: ") + reply.message);
    }

    void trackPending(const std::shared_ptr<::morph::async::detail::CompletionState<std::shared_ptr<void>>>& state) {
        std::erase_if(_pending, [](const auto& weak) { return weak.expired(); });
        _pending.emplace_back(state);
    }

    RemoteServer& _server;
    // 0 = unscoped (the default constructor's behavior); non-zero when
    // constructed with a ConnectionId from server.openConnection(). Threaded
    // through every handle()/handleInline() call this backend makes.
    ConnectionId _cid{0};
    // The owner every verb is called from, once `setOwner` has named it. The
    // two fields below are touched only there.
    std::optional<::morph::exec::detail::OwnerAffinity> _affinity;
    std::vector<std::weak_ptr<::morph::async::detail::CompletionState<std::shared_ptr<void>>>> _pending;
    ::morph::session::Context _session;
};

}  // namespace morph::backend
