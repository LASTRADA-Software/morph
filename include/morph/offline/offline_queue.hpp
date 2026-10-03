// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <concepts>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "../attributes.hpp"
#include "../core/completion.hpp"
#include "../core/detail/owned_state.hpp"
#include "../core/executor.hpp"
#include "../core/observability.hpp"
#include "../core/profiler.hpp"

namespace morph::offline {

/// @brief A replay attempt count, as a type distinct from a queue item id.
///
/// `setAttempts(itemId, attempts)` takes two integers that mean entirely
/// different things, and before this type existed both were plain unsigned
/// integers, mutually convertible in either direction. Transposing them
/// compiled, and — because `setAttempts()` on an unknown id is a documented
/// no-op — it also *ran*: the real item's count never advanced, nothing threw,
/// and the defect surfaced much later as a retry budget that never exhausts.
///
/// `Attempts` removes the hazard rather than suppressing the warning about it.
/// It is implicitly constructible from a narrow integer, so `setAttempts(id, 3)`
/// reads exactly as it did before, and **deliberately not constructible from a
/// 64-bit integer**, which is what a `QueueItem::id` is — so the transposed
/// call does not compile. A genuinely 64-bit count is still expressible, with
/// the narrowing spelled out at the call site:
/// `setAttempts(id, static_cast<std::uint32_t>(count))`.
class Attempts {
public:
    /// @brief Constructs a zero attempt count.
    constexpr Attempts() noexcept = default;

    /// @brief Implicitly wraps a narrow integral attempt count.
    ///
    /// Constrained to integral types **strictly narrower than a queue item
    /// id**, which is the whole point of the class: `uint64_t` (and any other
    /// 64-bit integer) is rejected, so passing an item id where an attempt
    /// count belongs is a compile error rather than a silent no-op. `bool` is
    /// excluded because a boolean is never an attempt count.
    ///
    /// @tparam Count Integral type of the incoming count.
    /// @param count The attempt count to wrap.
    template <typename Count>
        requires(std::integral<Count> && !std::same_as<std::remove_cv_t<Count>, bool> &&
                 sizeof(Count) < sizeof(std::uint64_t))
    constexpr Attempts(Count count) noexcept : _value{static_cast<std::uint32_t>(count)} {}

    /// @brief Returns the wrapped count.
    /// @return The attempt count as a plain `uint32_t`.
    [[nodiscard]] constexpr std::uint32_t value() const noexcept { return _value; }

    /// @brief Compares two attempt counts.
    /// @param other The count to compare against.
    /// @return `true` when both wrap the same value.
    [[nodiscard]] constexpr bool operator==(const Attempts& other) const noexcept = default;

private:
    std::uint32_t _value{0};
};

static_assert(!std::is_constructible_v<Attempts, std::uint64_t>,
              "Attempts must not be constructible from a queue item id, or setAttempts()'s two "
              "parameters become mutually convertible again and a transposed call compiles.");
static_assert(std::is_convertible_v<std::uint32_t, Attempts>,
              "Attempts must stay implicitly constructible from a narrow count, so existing "
              "setAttempts(id, n) call sites keep reading the way they did.");

/// @brief An item stored in the offline queue.
///
/// The payload is an opaque string — the caller controls the serialisation
/// format (JSON, binary-hex, plain text, etc.).
struct QueueItem {
    /// @brief Stable identifier assigned at enqueue time.
    ///
    /// Local to *this* queue instance; it is **not** a cross-subsystem
    /// idempotency key (it is queue-local and the journal never sees it; both
    /// shipped durable queues re-present the *stored* id after a restart). Use
    /// `idempotencyKey` to dedup a replay against already-applied ops.
    uint64_t id{};

    /// @brief Opaque serialised representation of the queued action.
    std::string payload;

    /// @brief Optional caller-supplied idempotency key that is **stable across
    ///        subsystems and process restarts** for one logical operation.
    ///
    /// Empty by default. When set, it is the shared dedup token that lets a
    /// replay consumer recognise an op the journal (or a previous replay) has
    /// already applied and skip it, instead of double-applying. The offline
    /// queue and the journal replay have no shared identity otherwise — the
    /// queue's `id` is queue-local and the journal's `seq` is journal-local —
    /// so a host that replays through both paths must wire this key (see
    /// `docs/spec/offline/offline.md`, "`idempotencyKey`: deduping against the journal").
    /// The value is opaque to the queue; a good choice is a stable content hash
    /// or a client-minted operation id (e.g. a UUID) reused if the same op is
    /// re-enqueued.
    ///
    /// The queue never *interprets* the key, and the interface does not
    /// **require** an implementation to enforce uniqueness on it — a replay
    /// consumer must therefore always dedup on the key itself, because a
    /// conforming queue may hand it the same key twice. An implementation is
    /// nonetheless **permitted** to dedup a non-empty key at enqueue time as a
    /// strengthening of that floor; see `IOfflineQueue::enqueue(std::string,
    /// std::string)` for exactly what a dedup hit does.
    std::string idempotencyKey;

    /// @brief Durable retry count for this item, authoritative when the queue
    ///        persists it.
    ///
    /// Defaults to `0`. A queue that overrides `IOfflineQueue::setAttempts()`
    /// to store this value makes the retry budget survive a process restart:
    /// `SyncWorker` seeds its own attempt counter from the larger of this
    /// field and its in-memory count, and writes the updated count back via
    /// `setAttempts()` after every failed replay. A queue that leaves
    /// `setAttempts()` as the default no-op never advances this field, so
    /// `SyncWorker`'s in-memory counter stays authoritative — today's
    /// process-local retry behavior, unchanged.
    uint32_t attempts{0};
};

/// @brief Thrown by `enqueue()` when the queue is at its configured `maxDepth()`.
///
/// `IOfflineQueue` enforces a reject-newest overflow policy: once the queue
/// holds `maxDepth()` items, a further `enqueue()` throws instead of silently
/// evicting an older item or invoking an app-defined callback. Evicting the
/// oldest item would destroy data the caller believes durable and break
/// replay ordering with no error raised anywhere; a host that genuinely wants
/// different eviction semantics already has the seam for it — subclass
/// `IOfflineQueue` directly rather than layering a second policy mechanism on
/// top of this one.
struct OfflineQueueFullError : std::runtime_error {
    /// @param maxDepthValue    The configured capacity that was reached.
    /// @param currentSizeValue Number of pending items at the time of rejection
    ///                         (equal to maxDepth for a well-behaved implementation).
    OfflineQueueFullError(std::size_t maxDepthValue, std::size_t currentSizeValue)
        : std::runtime_error("IOfflineQueue: enqueue rejected, queue is at capacity (" +
                             std::to_string(currentSizeValue) + "/" + std::to_string(maxDepthValue) + ")"),
          maxDepth{maxDepthValue},
          currentSize{currentSizeValue} {}

    /// @brief The configured capacity that was reached.
    std::size_t maxDepth;
    /// @brief Number of pending items at the time of rejection.
    std::size_t currentSize;
};

// ── Interface ─────────────────────────────────────────────────────────────────

/// @brief Interface for durable storage of actions that could not be delivered.
///
/// Items accumulate while the system is offline and are replayed by `SyncWorker`
/// on reconnect. The interface is intentionally minimal so that implementations
/// can range from in-memory (`InMemoryOfflineQueue`) to SQLite or file-backed stores.
///
/// @par One owner
/// Every queue morph ships belongs to the executor it is given at
/// construction. Its synchronous verbs answer only there; its writes
/// (`markDone`, `setAttempts`) run there, posted from anywhere else; a caller
/// that is not on the owner asks through the completion overloads
/// (`enqueue(replyExec, …)`, `drain(replyExec)`, `size(replyExec)`). A
/// `SyncWorker` draining the queue runs on the queue's owner. See
/// `docs/spec/offline/offline.md`, "One owner".
// NOLINTBEGIN(cppcoreguidelines-special-member-functions)
struct IOfflineQueue {
    virtual ~IOfflineQueue() = default;

    /// @brief Appends @p payload to the queue with no idempotency key.
    ///
    /// @param payload Serialised action to persist.
    /// @return A stable id that can be passed to `markDone()`.
    [[nodiscard]] virtual uint64_t enqueue(std::string payload) = 0;

    /// @brief Appends @p payload carrying the cross-subsystem @p idempotencyKey.
    ///
    /// The key is stored on the resulting `QueueItem::idempotencyKey` so a
    /// replay consumer can dedup this op against ones the journal (or a prior
    /// replay) already applied (see `QueueItem::idempotencyKey` and
    /// `docs/spec/offline/offline.md`).
    ///
    /// @par Uniqueness is a floor, not a prohibition
    /// The queue never interprets the key, and this interface does not
    /// *require* uniqueness enforcement: a conforming implementation may store
    /// the same non-empty key twice, so a replay consumer must dedup on the key
    /// regardless. Enqueue-time dedup is an allowed **strengthening**, not a
    /// contract violation. Of the implementations morph ships,
    /// `InMemoryOfflineQueue` never dedups, while `FileOfflineQueue` (linear
    /// scan) and `SqliteOfflineQueue` (partial unique index) both do.
    ///
    /// @par What a dedup hit does, in every implementation that dedups
    /// - It applies only to a **non-empty** key already carried by a *pending*
    ///   item. An empty key is never a dedup token — two empty-key enqueues
    ///   always produce two distinct items, in every implementation.
    /// - The call **succeeds** and returns the **existing** item's id rather
    ///   than a fresh one, so the return value is not a reliable signal that
    ///   anything was stored.
    /// - It is **first-write-wins with silent payload loss**: @p payload is
    ///   discarded, the pending item keeps the payload it already had, and no
    ///   error is raised and nothing is reported to the caller. A caller that
    ///   re-enqueues a *corrected* payload under an unchanged key therefore
    ///   loses the correction — mint a new key when the payload changes.
    /// - `markDone()` releases the key: once the pending item is gone, the same
    ///   key enqueues normally again.
    /// - It survives a restart in a durable queue — re-enqueuing a key that a
    ///   pending persisted item still carries is a hit after a reopen.
    ///
    /// These guarantees are pinned for every shipped implementation by
    /// `tests/offline_queue_conformance.hpp`, which is told which policy each
    /// implementation has and asserts it.
    ///
    /// The default implementation delegates to `enqueue(std::move(payload))`
    /// and then stamps the key via `setIdempotencyKey`, so existing
    /// `IOfflineQueue` implementations keep working without overriding it.
    ///
    /// @par Non-atomicity of this default (implementors, read this)
    /// This default makes **two separate virtual calls**
    /// (`enqueue(payload)`, then `setIdempotencyKey(itemId, key)`), and nothing
    /// ties them together. In an implementation reachable from more than one
    /// thread, a `markDone(itemId)` landing between them could erase the item
    /// before the key is stamped (silently dropping the key rather than
    /// throwing), and a `drain()` in that same window can observe the item
    /// with an *empty* idempotency key. A subclass that stores keys and cares
    /// about atomicity **must override the two-argument `enqueue`** and stamp
    /// the key in the same step that inserts the item, exactly as every
    /// implementation morph ships already does (`FileOfflineQueue`,
    /// `InMemoryOfflineQueue`, `SqliteOfflineQueue` all override it inline) —
    /// this base default exists only so a subclass that has not been updated
    /// yet keeps compiling and working for the non-concurrent case, not as an
    /// atomicity guarantee.
    /// @param payload        Serialised action to persist.
    /// @param idempotencyKey Stable dedup token for the logical op; may be empty.
    /// @return A stable id that can be passed to `markDone()`.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverloaded-virtual"
    [[nodiscard]] virtual uint64_t enqueue(std::string payload, std::string idempotencyKey) {
        const uint64_t itemId = enqueue(std::move(payload));
        setIdempotencyKey(itemId, std::move(idempotencyKey));
        return itemId;
    }
#pragma GCC diagnostic pop

    /// @brief Returns all pending items in enqueue order without removing them.
    ///
    /// Items remain in the queue until `markDone()` is called. It is safe to
    /// call `drain()` multiple times — items survive a crash between `drain()`
    /// and the corresponding `markDone()` call.
    ///
    /// @par Enqueue order is the implementation's to keep; the id does not imply it
    /// "Enqueue order" is a property this method **requires** and that
    /// `QueueItem::id` does not supply. Every implementation morph ships mints
    /// ids that happen to increase with insertion — an in-memory counter, a
    /// file offset, SQLite's `INTEGER PRIMARY KEY AUTOINCREMENT` — so each of
    /// them satisfies this method by presenting rows ordered by id. That is a
    /// property of those three stores, not of the type: a `uint64_t` neither
    /// promises monotonicity nor rules out an id minted from a GUID, a content
    /// hash, or a sharded sequence, and a store that reuses the id of a removed
    /// row does not order correctly either. An implementation over such a store
    /// must carry its own insertion sequence and order on *that*; ordering by
    /// the id would compile, survive a casual smoke test, and replay a user's
    /// actions out of order. `tests/offline_queue_conformance.hpp` asserts the
    /// ordering, so an implementation that gets this wrong fails there.
    /// @return Snapshot of all pending items.
    [[nodiscard]] virtual std::vector<QueueItem> drain() const = 0;

    /// @brief Removes the item identified by @p itemId.
    ///
    /// No-op if @p itemId is not found.
    /// @param itemId Id returned by the corresponding `enqueue()` call.
    virtual void markDone(uint64_t itemId) = 0;

    /// @brief Returns the number of pending items without removing them.
    ///
    /// Default implementation calls `drain().size()` — correct but O(n) and
    /// allocates a full snapshot vector to answer a size query. Override for
    /// an O(1) or index-backed answer.
    /// @return Current pending item count.
    [[nodiscard]] virtual std::size_t size() const { return drain().size(); }

    /// @brief Returns the configured maximum depth, or `std::nullopt` if unbounded.
    ///
    /// Default: `std::nullopt` (unbounded) — preserves current behavior for any
    /// `IOfflineQueue` subclass that predates this method.
    /// @return The capacity `enqueue()` enforces, or `std::nullopt` if none.
    [[nodiscard]] virtual std::optional<std::size_t> maxDepth() const { return std::nullopt; }

    /// @brief Persists an updated attempt count for an item. Default: no-op.
    ///
    /// A durable queue overrides this to store the count so the retry budget
    /// survives a restart — `SyncWorker` calls it after every failed replay,
    /// and reads the persisted value back through `QueueItem::attempts` on
    /// the next `drain()` (this run, or after a restart). `InMemoryOfflineQueue`
    /// overrides it to update the in-deque item; a queue that leaves it as
    /// the default no-op keeps the pre-existing process-local retry behavior
    /// (`SyncWorker`'s own in-memory counter is then always authoritative,
    /// since `QueueItem::attempts` never advances).
    /// @param itemId   Id of the item whose attempt count changed.
    /// @param attempts New cumulative attempt count to persist. Typed, not a
    ///        bare integer, so a transposed call does not compile — see
    ///        `Attempts` for what that silently did before.
    virtual void setAttempts([[maybe_unused]] uint64_t itemId, [[maybe_unused]] Attempts attempts) {}

    /// @brief Appends @p payload carrying @p idempotencyKey on the queue's
    ///        owner; the new item's id is delivered on @p replyExec.
    ///
    /// The form of `enqueue()` for a caller that is not on the queue's owner.
    /// A full queue rejects the completion with `OfflineQueueFullError`.
    /// @param replyExec      Where the answer is delivered, and where the
    ///        caller attaches to it. Borrowed: it must outlive the completion.
    /// @param payload        Serialised action to persist.
    /// @param idempotencyKey Stable dedup token for the logical op; may be empty.
    /// @return The id `enqueue(payload, idempotencyKey)` would have returned,
    ///         or the error it would have thrown.
    [[nodiscard]] ::morph::async::Completion<uint64_t> enqueue(::morph::exec::IExecutor& replyExec MORPH_LIFETIMEBOUND,
                                                               std::string payload, std::string idempotencyKey = {}) {
        return askEnqueue(replyExec, std::move(payload), std::move(idempotencyKey));
    }

    /// @brief Snapshots the pending items on the queue's owner; delivered on
    ///        @p replyExec.
    /// @param replyExec Where the answer is delivered. Borrowed: it must
    ///        outlive the completion.
    /// @return What `drain()` would have returned.
    [[nodiscard]] ::morph::async::Completion<std::vector<QueueItem>> drain(
        ::morph::exec::IExecutor& replyExec MORPH_LIFETIMEBOUND) const {
        return askDrain(replyExec);
    }

    /// @brief Counts the pending items on the queue's owner; delivered on
    ///        @p replyExec.
    /// @param replyExec Where the answer is delivered. Borrowed: it must
    ///        outlive the completion.
    /// @return What `size()` would have returned.
    [[nodiscard]] ::morph::async::Completion<std::size_t> size(
        ::morph::exec::IExecutor& replyExec MORPH_LIFETIMEBOUND) const {
        return askSize(replyExec);
    }

protected:
    /// @brief What `enqueue(replyExec, payload, key)` asks. The default
    ///        enqueues where it is called, for an implementation with no
    ///        owner; one with an owner enqueues there.
    /// @param replyExec      Where the answer is delivered.
    /// @param payload        Serialised action to persist.
    /// @param idempotencyKey Stable dedup token; may be empty.
    /// @return The new item's id, or the enqueue's error.
    [[nodiscard]] virtual ::morph::async::Completion<uint64_t> askEnqueue(::morph::exec::IExecutor& replyExec,
                                                                          std::string payload,
                                                                          std::string idempotencyKey) {
        return answerHere<uint64_t>(replyExec, [&] { return enqueue(std::move(payload), std::move(idempotencyKey)); });
    }

    /// @brief What `drain(replyExec)` asks. The default drains where it is
    ///        called; one with an owner drains there.
    /// @param replyExec Where the answer is delivered.
    /// @return The pending items.
    [[nodiscard]] virtual ::morph::async::Completion<std::vector<QueueItem>> askDrain(
        ::morph::exec::IExecutor& replyExec) const {
        return answerHere<std::vector<QueueItem>>(replyExec, [&] { return drain(); });
    }

    /// @brief What `size(replyExec)` asks. The default counts where it is
    ///        called; one with an owner counts there.
    /// @param replyExec Where the answer is delivered.
    /// @return The pending item count.
    [[nodiscard]] virtual ::morph::async::Completion<std::size_t> askSize(::morph::exec::IExecutor& replyExec) const {
        return answerHere<std::size_t>(replyExec, [&] { return size(); });
    }

    /// @brief Stamps an idempotency key onto an already-enqueued item.
    ///
    /// Called by the default `enqueue(payload, key)` overload so implementations
    /// that only override the single-argument `enqueue` still support keys. The
    /// default is a no-op (an implementation with no per-item key storage simply
    /// drops it); `InMemoryOfflineQueue` overrides it to record the key.
    /// @param itemId         Id of the item to stamp.
    /// @param idempotencyKey Key to store; ignored by the default no-op.
    virtual void setIdempotencyKey([[maybe_unused]] uint64_t itemId, [[maybe_unused]] std::string idempotencyKey) {}

private:
    /// Answers @p body's result now, delivered on @p replyExec.
    template <typename T, typename Body>
    static ::morph::async::Completion<T> answerHere(::morph::exec::IExecutor& replyExec, Body&& body) {
        auto settleable = ::morph::async::Completion<T>::makeSettleable(&replyExec);
        try {
            settleable.second.resolve(std::forward<Body>(body)());
        } catch (...) {
            settleable.second.reject(std::current_exception());
        }
        return std::move(settleable.first);
    }
};
// NOLINTEND(cppcoreguidelines-special-member-functions)

// The point of `Attempts`, stated as something the compiler checks rather than
// as a comment. The second assertion is what keeps the first from being
// vacuous: if `Attempts` were an alias for `uint32_t` the transposed call would
// compile and the first assertion would fail, and if it were made explicit the
// ordinary call would stop compiling and the second would fail. Both directions
// have to hold.
static_assert(!std::is_invocable_v<decltype(&IOfflineQueue::setAttempts), IOfflineQueue&, uint32_t, uint64_t>,
              "setAttempts's parameters must not be transposable: writing an item id into an attempt count is "
              "silent, because setAttempts on an unknown id is a documented no-op.");
static_assert(std::is_invocable_v<decltype(&IOfflineQueue::setAttempts), IOfflineQueue&, uint64_t, uint32_t>,
              "setAttempts must still accept a plain count in the right order.");

// ── In-memory implementation ──────────────────────────────────────────────────

/// @brief In-memory implementation of `IOfflineQueue`, owned by one executor.
///
/// Suitable for testing and for applications that do not require persistence
/// across process restarts. Items are stored in a `std::deque` that belongs to
/// the executor given at construction (see `IOfflineQueue`, "One owner").
class InMemoryOfflineQueue : public IOfflineQueue {
public:
    using IOfflineQueue::drain;
    using IOfflineQueue::enqueue;  // keep the two-arg and completion overloads visible
    using IOfflineQueue::size;

    /// @brief Constructs an in-memory queue belonging to @p owner, optionally bounded.
    /// @param owner The executor every access runs on; must run one task at a
    ///        time. Borrowed: it must outlive this queue and run what it posts.
    /// @param maxDepth Maximum number of pending items `enqueue()` will admit
    ///        before throwing `OfflineQueueFullError`; `std::nullopt` (the
    ///        default) means unbounded.
    explicit InMemoryOfflineQueue(::morph::exec::IExecutor& owner MORPH_LIFETIMEBOUND,
                                  std::optional<std::size_t> maxDepth = std::nullopt)
        : _maxDepth{maxDepth}, _owned{owner, std::make_shared<State>()} {}

    /// @brief Appends @p payload and returns a monotonically increasing id. On the owner.
    /// @param payload Serialised action to store.
    /// @return Unique id for this item.
    [[nodiscard]] uint64_t enqueue(std::string payload) override { return enqueue(std::move(payload), {}); }

    /// @brief Appends @p payload carrying @p idempotencyKey and returns a
    ///        monotonically increasing id. On the owner.
    /// @param payload        Serialised action to store.
    /// @param idempotencyKey Stable dedup token; stored verbatim on the item.
    /// @return Unique id for this item.
    /// @throws OfflineQueueFullError if the queue is already at `maxDepth()`.
    [[nodiscard]] uint64_t enqueue(std::string payload, std::string idempotencyKey) override {
        return insert(_owned.read("InMemoryOfflineQueue::enqueue"), _maxDepth, std::move(payload),
                      std::move(idempotencyKey));
    }

    /// @brief Returns a snapshot of all pending items. On the owner.
    /// @return Copy of all items in insertion order.
    [[nodiscard]] std::vector<QueueItem> drain() const override {
        auto const& items = _owned.read("InMemoryOfflineQueue::drain").items;
        return std::vector<QueueItem>{items.begin(), items.end()};
    }

    /// @brief Returns the number of pending items. On the owner.
    /// @return Current pending item count.
    [[nodiscard]] std::size_t size() const override { return _owned.read("InMemoryOfflineQueue::size").items.size(); }

    /// @brief Returns the configured maximum depth, or `std::nullopt` if
    ///        unbounded. Callable anywhere: fixed at construction.
    /// @return The capacity `enqueue()` enforces, or `std::nullopt` if none.
    [[nodiscard]] std::optional<std::size_t> maxDepth() const override { return _maxDepth; }

    /// @brief Removes the item with @p itemId from the queue, on the owner.
    ///
    /// No-op if @p itemId is not found.
    /// @param itemId Id of the item to remove.
    void markDone(uint64_t itemId) override {
        _owned.apply("InMemoryOfflineQueue::markDone", [itemId](State& state) {
            auto iter =
                std::ranges::find_if(state.items, [itemId](const QueueItem& item) { return item.id == itemId; });
            if (iter != state.items.end()) {
                state.items.erase(iter);
            }
        });
    }

    /// @brief Updates the persisted attempt count on the in-deque item with
    ///        @p itemId, on the owner. No-op if @p itemId is not found.
    ///
    /// `InMemoryOfflineQueue` does not itself survive a process restart, but
    /// overriding this hook lets a *fresh* `SyncWorker` constructed over the
    /// same instance observe a durable-style cumulative attempt count — used
    /// to simulate cross-restart dead-lettering in tests.
    /// @param itemId   Id of the item to update.
    /// @param attempts New attempt count to store.
    void setAttempts(uint64_t itemId, Attempts attempts) override {
        _owned.apply("InMemoryOfflineQueue::setAttempts", [itemId, attempts](State& state) {
            auto iter =
                std::ranges::find_if(state.items, [itemId](const QueueItem& item) { return item.id == itemId; });
            if (iter != state.items.end()) {
                iter->attempts = attempts.value();
            }
        });
    }

protected:
    /// @brief Enqueues on the owner, answered on @p replyExec.
    /// @param replyExec      Where the answer is delivered.
    /// @param payload        Serialised action to store.
    /// @param idempotencyKey Stable dedup token; may be empty.
    /// @return The new item's id, or `OfflineQueueFullError`.
    [[nodiscard]] ::morph::async::Completion<uint64_t> askEnqueue(::morph::exec::IExecutor& replyExec,
                                                                  std::string payload,
                                                                  std::string idempotencyKey) override {
        return _owned.ask<uint64_t>("InMemoryOfflineQueue::enqueue", replyExec,
                                    [maxDepth = _maxDepth, payload = std::move(payload),
                                     idempotencyKey = std::move(idempotencyKey)](State& state) mutable {
                                        return insert(state, maxDepth, std::move(payload), std::move(idempotencyKey));
                                    });
    }

    /// @brief Snapshots on the owner, answered on @p replyExec.
    /// @param replyExec Where the answer is delivered.
    /// @return The pending items.
    [[nodiscard]] ::morph::async::Completion<std::vector<QueueItem>> askDrain(
        ::morph::exec::IExecutor& replyExec) const override {
        return _owned.ask<std::vector<QueueItem>>("InMemoryOfflineQueue::drain", replyExec, [](State& state) {
            return std::vector<QueueItem>{state.items.begin(), state.items.end()};
        });
    }

    /// @brief Counts on the owner, answered on @p replyExec.
    /// @param replyExec Where the answer is delivered.
    /// @return The pending item count.
    [[nodiscard]] ::morph::async::Completion<std::size_t> askSize(::morph::exec::IExecutor& replyExec) const override {
        return _owned.ask<std::size_t>("InMemoryOfflineQueue::size", replyExec,
                                       [](State& state) { return state.items.size(); });
    }

private:
    /// Everything the queue holds; touched only on the owner.
    struct State {
        std::deque<QueueItem> items;
        uint64_t nextId{0};
    };

    /// Inserts one item into @p state, refusing past @p maxDepth.
    static uint64_t insert(State& state, std::optional<std::size_t> maxDepth, std::string payload,
                           std::string idempotencyKey) {
        MORPH_ZONE("InMemoryOfflineQueue::enqueue");
        if (maxDepth && state.items.size() >= *maxDepth) {
            ::morph::observe::detail::emitMetric(::morph::observe::Metric::queueOverflow,
                                                 static_cast<double>(state.items.size()));
            throw OfflineQueueFullError{*maxDepth, state.items.size()};
        }
        uint64_t const itemId = ++state.nextId;
        state.items.push_back(
            QueueItem{.id = itemId, .payload = std::move(payload), .idempotencyKey = std::move(idempotencyKey)});
        return itemId;
    }

    std::optional<std::size_t> _maxDepth;
    ::morph::exec::detail::OwnedState<State> _owned;
};

}  // namespace morph::offline
