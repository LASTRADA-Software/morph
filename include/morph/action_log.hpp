// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <glaze/glaze.hpp>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morph::journal {

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
/// @throws SerializationError if @p json is not a valid `LogEntry`.
inline LogEntry fromJson(std::string_view json) {
    LogEntry entry{};
    detail::throwOnGlazeError(glz::read_json(entry, json), json);
    return entry;
}

/// @brief Interface for durable storage of executed-action entries.
///
/// Entries are never removed by the framework — this is a permanent, append-only
/// record, unlike `morph::offline::IOfflineQueue` (whose `markDone()` deletes
/// items once retried successfully). Implementations range from in-memory
/// (`InMemoryActionLog`) to file, SQL, or network-backed stores supplied by the
/// host application.
// NOLINTBEGIN(cppcoreguidelines-special-member-functions)
struct IActionLog {
    virtual ~IActionLog() = default;

    /// @brief Appends @p entry. Implementations assign `entry.seq`.
    /// @param entry Entry to append.
    virtual void append(LogEntry entry) = 0;

    /// @brief Pushes any buffered entries to the durable backend. No-op for sinks
    ///        with nothing to buffer (e.g. `InMemoryActionLog`).
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
/// durability. Mirrors `morph::offline::InMemoryOfflineQueue`'s shape.
class InMemoryActionLog : public IActionLog {
public:
    /// @brief Appends @p entry, assigning a monotonically increasing `seq`. Thread-safe.
    /// @param entry Entry to append; `seq` is overwritten regardless of the input value.
    void append(LogEntry entry) override {
        std::scoped_lock const lock{_mtx};
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
