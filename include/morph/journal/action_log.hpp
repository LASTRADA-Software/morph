// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstdint>
#include <glaze/glaze.hpp>
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

/// @brief One recorded execution of an action against a model instance.
///
/// Produced automatically by `morph::model::detail::IModelHolder::recordIfAttached`
/// — application and model code never construct or append these directly.
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

    /// @brief JSON-encoded result (`ActionTraits<A>::resultToJson`), captured after
    ///        successful execution.
    std::string result;

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
    std::string idempotencyKey{};

    /// @brief Line-format version this entry was written at.
    ///
    /// Defaults to `kLogFormatVersion`, so every freshly-constructed entry
    /// already carries the current version with no separate stamping step. A
    /// legacy line (written before this field existed) has no `v` key; under
    /// `fromJson`'s lenient decode that is just an absent key, so it decodes
    /// with this same default — i.e. legacy data reads as `v == 1`, which is
    /// correct: v1 is today's shape, `kLogFormatVersion` merely names it.
    std::uint32_t v = kLogFormatVersion;
};

/// @brief Thrown by `toJson`/`fromJson` when `LogEntry` (de)serialisation fails.
struct SerializationError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

namespace detail {

/// @brief Converts a Glaze error into a `SerializationError`, or does nothing
///        if @p errCode reports success.
///
/// Shared by `toJson` and `fromJson` on purpose, not just for DRY: this is one
/// non-template function, so both call through the exact same compiled branch.
/// `fromJson`'s failure path is easy to exercise for real (malformed JSON is
/// everyday input); `toJson`'s is not — Glaze's buffer-writer has no reachable
/// failure mode for a flat struct of strings/integers like `LogEntry` (its
/// only write-relevant error codes are for recursion-depth limits `LogEntry`
/// can't hit, and `dump_int_error`, which nothing in Glaze's own source ever
/// actually raises). Routing both through here means `toJson`'s error branch
/// is the same branch `fromJson`'s test already exercises, rather than a
/// second, structurally-unreachable copy of the same three lines.
/// @param errCode Result of a `glz::write_json`/`glz::read_json` call.
/// @param context Buffer or input passed to `glz::format_error` for the message.
inline void throwOnGlazeError(const glz::error_ctx& errCode, std::string_view context) {
    if (errCode) {
        throw SerializationError{glz::format_error(errCode, context)};
    }
}

}  // namespace detail

/// @brief Encodes @p entry as JSON.
///
/// `LogEntry` is a plain aggregate, so Glaze reflects it without a `glz::meta`
/// specialisation — the same automatic reflection `BRIDGE_REGISTER_ACTION`
/// relies on for user action structs. Used by sinks that need an opaque
/// string representation (`FileActionLog`).
/// @throws SerializationError on encode failure (see `detail::throwOnGlazeError`
///         for why this is not realistically reachable for `LogEntry`).
inline std::string toJson(const LogEntry& entry) {
    std::string out;
    detail::throwOnGlazeError(glz::write_json(entry, out), out);
    return out;
}

/// @brief Decodes @p json into a `LogEntry`.
///
/// Reads leniently: `glz::read<glz::opts{.error_on_unknown_keys = false}>`,
/// the same stance `morph::wire::decode` takes (`include/morph/core/wire.hpp`) —
/// an unknown/extra JSON key (e.g. an additive field written by a newer morph
/// build) is ignored rather than rejected. Same duplicate-key caveat as
/// `wire::decode`: last-wins, not a security boundary (glaze offers no reject
/// option). Syntactically malformed JSON still throws. After a successful
/// decode, also enforces the line-format version rule: `v <= kLogFormatVersion`
/// decodes normally, `v` greater than this build's `kLogFormatVersion` throws
/// — a build refuses to guess at a line format newer than any it has seen.
/// @throws SerializationError if @p json is not valid JSON, does not decode
///         into a `LogEntry`, or decodes with `v` greater than
///         `kLogFormatVersion`.
inline LogEntry fromJson(std::string_view json) {
    LogEntry entry{};
    // `null_terminated = false`: `json` is a caller-supplied view (a line read from a
    // journal file, with no guaranteed trailing '\0') — see the identical rationale on
    // `morph::wire::decode` (`wire.hpp`), whose fuzz harness found the resulting
    // heap-buffer-overflow in glaze's `skip_ws`.
    static constexpr glz::opts kLenient{.null_terminated = false, .error_on_unknown_keys = false};
    detail::throwOnGlazeError(glz::read<kLenient>(entry, json), json);
    if (entry.v > kLogFormatVersion) {
        throw SerializationError{
            "journal::fromJson: line format v" + std::to_string(entry.v) +
            " is newer than this build supports (kLogFormatVersion = " + std::to_string(kLogFormatVersion) + ")"};
    }
    return entry;
}

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
