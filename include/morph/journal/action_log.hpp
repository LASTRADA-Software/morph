// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace morph::journal {

/// @brief Current line-format version stamped on every newly-written
///        `LogEntry` (`LogEntry::v`'s default).
///
/// Bumped only on a **breaking** change to `LogEntry`'s on-disk/on-wire shape;
/// an additive key (tolerated by `fromJson`'s lenient decode) does not bump
/// it. A reader refuses to decode a line whose `v` exceeds this constant —
/// see `fromJson`.
inline constexpr std::uint32_t kLogFormatVersion = 1;

/// @brief Discriminates a recorded `LogEntry` as a successful execution or one
///        rejected by the validator / thrown by `Model::execute`.
///
/// A failed entry has an empty `result` (there was none) and a non-empty
/// `error`; a succeeded entry is the reverse. Serialises as the string
/// `"Succeeded"`/`"Failed"` (see the `glz::meta` specialisation below) so the
/// audit trail stays human-readable without cross-referencing the enum.
enum class Outcome : std::uint8_t { Succeeded, Failed };

/// @brief One recorded execution of an action against a model instance.
///
/// Normally produced automatically by
/// `morph::model::detail::IModelHolder::recordIfAttached`. Application code may
/// also construct and append one directly — that is what an outbox row is, and
/// what `causalParentId` is set by; see `morph::journal::OutboxRelay`.
struct LogEntry {
    /// @brief Monotonic order assigned by the sink on `append()`. Callers pass `0`.
    uint64_t seq = 0;

    /// @brief String type-id of the model the action ran against (`ModelTraits<M>::typeId()`).
    std::string modelType;

    /// @brief Stable identity of the model instance (e.g. an account id), stamped
    ///        from the value passed to `attachActionLog()`. Empty if none was set.
    std::string entityKey;

    /// @brief String type-id of the executed action (`ActionTraits<A>::typeId()`).
    std::string actionType;

    /// @brief JSON-encoded request (`ActionTraits<A>::toJson`).
    std::string payload;

    /// @brief Structural fingerprint of the action payload's shape at the
    ///        moment `payload` was encoded (`morph::model::payloadFingerprint`).
    ///
    /// `v` above versions the *line format*; this versions the *payload*. Without
    /// it, replaying an entry written by an older build against a renamed field
    /// is indistinguishable from replaying an entry that genuinely omitted the
    /// field: the lenient decode drops the unknown key, default-constructs the
    /// new one, and reconstruction reports a state that was never recorded. With
    /// it, `replay()` compares this against the fingerprint the *current* build
    /// computes for the same action and refuses rather than guessing — see
    /// `journal::replay()` and `docs/spec/journal/journal.md`, "Payload schema
    /// fingerprint".
    ///
    /// Stamped automatically by the two sites that execute an action
    /// (`ActionDispatcher::registerAction`'s runner and `Bridge::executeVia`'s
    /// local op). **Empty is a meaningful value**: it marks an *unstamped*
    /// entry — one written before this field existed, or appended directly by
    /// application code — whose payload shape is unknown and therefore
    /// unverifiable. See `UnstampedPayloadPolicy` for what replay does with one.
    std::string schema;

    /// @brief JSON-encoded result (`ActionTraits<A>::resultToJson`), captured after
    ///        successful execution. Empty when `outcome == Outcome::Failed`.
    std::string result;

    /// @brief Whether this execution succeeded or was rejected/threw. Defaults to
    ///        `Succeeded` so a pre-existing on-disk entry (written before this
    ///        field existed) decodes unchanged under the lenient reader — an
    ///        absent key is indistinguishable from an explicit `Succeeded`.
    Outcome outcome = Outcome::Succeeded;

    /// @brief `std::exception::what()` from the exception that rejected the
    ///        action. Empty unless `outcome == Outcome::Failed`.
    std::string error;

    /// @brief Auth principal from `morph::session::current()`, if any. Empty if unset.
    std::string principal;

    /// @brief Wall-clock time of execution, milliseconds since the Unix epoch.
    int64_t timestampMs = 0;

    /// @brief Optional dedup token for outbox-relayed entries. Empty by default;
    ///        ordinary auto-appended entries (from `ActionDispatcher`'s runner or
    ///        `Bridge::executeVia`'s local op) never set it. Mirrors
    ///        `morph::offline::QueueItem::idempotencyKey`'s exact contract: opaque,
    ///        stored verbatim, stable across restarts for one logical outbox row.
    ///        See `journal::OutboxRelay` (`outbox.hpp`) for how it's used.
    std::string idempotencyKey;

    /// @brief Line-format version this entry was written at.
    ///
    /// Defaults to `kLogFormatVersion`, so every freshly-constructed entry
    /// already carries the current version with no separate stamping step. A
    /// legacy line (written before this field existed) has no `v` key; under
    /// `fromJson`'s lenient decode that is just an absent key, so it decodes
    /// with this same default — i.e. legacy data reads as `v == 1`, which is
    /// correct: v1 is today's shape, `kLogFormatVersion` merely names it.
    std::uint32_t v = kLogFormatVersion;

    /// @brief Identity of the "trigger" entry that caused this entry to be
    ///        recorded, or empty (the sentinel) if this entry was not caused
    ///        by another one.
    ///
    /// Set by application code that journals a cascaded mutation — e.g. an
    /// automation rule that reacts to one recorded action by executing a
    /// further one — to the triggering entry's own stable identity, so
    /// `replay()` and an activity view can both recover "what caused this."
    /// Empty by default: an ordinary, non-cascaded entry never sets it.
    ///
    /// @warning **Must not be a `LogEntry::seq` value.** `seq` is sink-local
    /// and re-stamped by every sink's `append()` (and again by
    /// `SessionLog::checkpoint()` when forwarding) — it is not a stable,
    /// cross-sink or cross-restart identifier (see the Invariants section of
    /// `docs/spec/journal/journal.md`). A `causalParentId` wired to a raw
    /// `seq` would stop matching anything the moment the trigger entry is
    /// forwarded to another sink or the process restarts. Application code
    /// must instead mint its own opaque/UUID-style identity for the trigger
    /// entry at the point it is created, independent of whatever `seq` any
    /// sink later assigns it, and reuse that same identity as every cascaded
    /// entry's `causalParentId`.
    std::string causalParentId;
};

/// @brief Thrown by `toJson`/`fromJson` when `LogEntry` (de)serialisation fails.
///
/// Declared here rather than beside the codec in `action_log_json.hpp`: a
/// caller catching it needs only `<stdexcept>`, and making that catch drag in
/// glaze would put the surcharge back on exactly the consumers this split
/// exists to spare (morph#573, step 4).
struct SerializationError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// @brief Interface for durable storage of executed-action entries.
///
/// Entries are never removed by the framework — this is a permanent, append-only
/// record, unlike `morph::offline::IOfflineQueue` (whose `markDone()` deletes
/// items once retried successfully). Implementations range from in-memory
/// (`InMemoryActionLog`) to file, SQL, or network-backed stores supplied by the
/// host application.
///
/// @par Idempotency-key dedup (optional)
/// An implementation MAY treat a non-empty `LogEntry::idempotencyKey` as a dedup
/// key on `append()`: if an entry with the same key was already recorded, treat
/// the call as a no-op. This is not required by the interface, but
/// `InMemoryActionLog` and `FileActionLog` both do it, which is what makes them
/// safe choices for `journal::OutboxRelay::sink` (see `outbox.hpp`) — a
/// re-relayed row after a crash between `append()` and marking it relayed lands
/// here twice but is stored once. An entry with an empty `idempotencyKey` is
/// never deduped.
// NOLINTBEGIN(cppcoreguidelines-special-member-functions)
struct IActionLog {
    virtual ~IActionLog() = default;

    /// @brief Appends @p entry. Implementations assign `entry.seq`.
    ///
    /// An implementation that can fail to record the entry must throw. Returning
    /// normally is the sink's promise that the entry is recorded (or, for a
    /// buffering sink, that it will be by the next successful `flush()`); see
    /// `flush()` for why silence is not an option here.
    ///
    /// @param entry Entry to append.
    /// @throws std::exception (implementation-defined) if the entry could not be recorded.
    virtual void append(LogEntry entry) = 0;

    /// @brief Pushes any buffered entries to the durable backend. No-op for sinks
    ///        with nothing to buffer (e.g. `InMemoryActionLog`).
    ///
    /// **Must throw if the data did not reach the backend.** The return type is
    /// `void`, so throwing is the only channel an implementation has, and
    /// callers rely on it: `OutboxRelay::relay()` calls `markRelayed()` directly
    /// after this, and a silently-failed flush would mark rows relayed in the
    /// model's own store while nothing was durably written — dropping them from
    /// the outbox *and* from the log, with no error anywhere. An implementation
    /// that cannot fail (nothing to buffer) simply never throws.
    ///
    /// @throws std::exception (implementation-defined) if buffered entries could
    ///         not be made durable.
    virtual void flush() = 0;

    /// @brief Returns recorded entries in append order.
    /// @param entityKey If non-empty, restricts the result to that entity's entries.
    /// @return Matching entries, in append order.
    [[nodiscard]] virtual std::vector<LogEntry> entries(std::string_view entityKey = {}) const = 0;
};
// NOLINTEND(cppcoreguidelines-special-member-functions)

/// @brief Thread-safe in-memory implementation of `IActionLog`.
///
/// Suitable for testing and for applications that do not need cross-process
/// durability. Mirrors `morph::offline::InMemoryOfflineQueue`'s shape. Dedups
/// `append()` on a non-empty `LogEntry::idempotencyKey` — see `IActionLog`'s
/// class docs.
class InMemoryActionLog : public IActionLog {
public:
    /// @brief Appends @p entry, assigning a monotonically increasing `seq`. Thread-safe.
    /// @param entry Entry to append; `seq` is overwritten regardless of the input value.
    void append(LogEntry entry) override {
        std::scoped_lock const lock{_mtx};
        if (!entry.idempotencyKey.empty() && !_seenIdempotencyKeys.insert(entry.idempotencyKey).second) {
            return;  // already recorded once; a re-relayed duplicate is a safe no-op
        }
        entry.seq = ++_nextSeq;
        _entries.push_back(std::move(entry));
    }

    /// @brief No-op — there is no external backend to flush to.
    void flush() override {}

    /// @brief Returns a snapshot of matching entries in append order. Thread-safe.
    /// @param entityKey If non-empty, restricts the result to that entity's entries.
    /// @return Matching entries, in append order.
    [[nodiscard]] std::vector<LogEntry> entries(std::string_view entityKey = {}) const override {
        std::scoped_lock const lock{_mtx};
        if (entityKey.empty()) {
            return _entries;
        }
        std::vector<LogEntry> out;
        for (const auto& entry : _entries) {
            if (entry.entityKey == entityKey) {
                out.push_back(entry);
            }
        }
        return out;
    }

private:
    mutable std::mutex _mtx;
    std::vector<LogEntry> _entries;
    uint64_t _nextSeq{0};
    std::unordered_set<std::string> _seenIdempotencyKeys;
};

namespace detail {

/// @brief Process-wide slot holding the default action log, plus the mutex
///        guarding it. A function-local static, not a namespace-scope global,
///        so it's safe regardless of translation-unit init order.
inline std::pair<std::mutex, std::shared_ptr<IActionLog>>& defaultActionLogState() {
    static std::pair<std::mutex, std::shared_ptr<IActionLog>> state;
    return state;
}

}  // namespace detail

/// @brief Installs @p log as the process-wide default action log.
///
/// Every model instance created via `morph::model::detail::ModelFactory::create<Model>()`
/// — which is every model registered the ordinary way, whether the active
/// backend ends up being local or remote — automatically gets @p log attached
/// (with an empty `entityKey`) from that point on. Call this once at startup;
/// no per-model or per-handler wiring is needed for the common case.
///
/// Application code that needs a specific instance identity (e.g. per-account
/// auditing) can still call `IModelHolder::attachActionLog` explicitly on that
/// instance afterward — an explicit call always overrides whatever the
/// default attached. Thread-safe.
///
/// @param log Sink to attach automatically, or `nullptr` to stop auto-attaching
///            (existing instances keep whatever they already have).
inline void setActionLog(std::shared_ptr<IActionLog> log) {
    auto& [mtx, slot] = detail::defaultActionLogState();
    std::scoped_lock const lock{mtx};
    slot = std::move(log);
}

/// @brief Returns the currently installed default action log, or `nullptr`
///        if none has been set. Thread-safe.
[[nodiscard]] inline std::shared_ptr<IActionLog> defaultActionLog() {
    auto& [mtx, slot] = detail::defaultActionLogState();
    std::scoped_lock const lock{mtx};
    return slot;
}

/// @brief RAII helper that installs a default action log for its lifetime and
///        restores the previous one on destruction.
///
/// Mirrors `morph::log::ScopedLoggerOverride`. Intended for tests (so one test
/// case's sink never leaks into the next) and for applications that need to
/// temporarily redirect auto-attached logging within a scope.
///
/// @code
/// {
///     morph::journal::ScopedActionLog guard{std::make_shared<morph::journal::InMemoryActionLog>()};
///     // ... models created in this scope auto-attach guard's log ...
/// }  // previous default restored here
/// @endcode
class ScopedActionLog {
public:
    /// @brief Installs @p log as the default, saving whatever was there before.
    /// @param log New default for the lifetime of this object.
    explicit ScopedActionLog(std::shared_ptr<IActionLog> log) : _previous{defaultActionLog()} {
        setActionLog(std::move(log));
    }

    /// @brief Restores the saved default.
    ~ScopedActionLog() { setActionLog(std::move(_previous)); }

    ScopedActionLog(const ScopedActionLog&) = delete;
    ScopedActionLog& operator=(const ScopedActionLog&) = delete;
    ScopedActionLog(ScopedActionLog&&) = delete;
    ScopedActionLog& operator=(ScopedActionLog&&) = delete;

private:
    std::shared_ptr<IActionLog> _previous;
};

}  // namespace morph::journal
