// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "action_log.hpp"
#include "model.hpp"
#include "registry.hpp"

namespace morph::journal {

/// @brief Reconstructs model state by replaying @p entries, in order, against a
///        freshly created model instance.
///
/// Builds on the same `ModelRegistryFactory` / `ActionDispatcher` machinery
/// `RemoteServer` already uses to dispatch incoming requests — no separate
/// replay engine is needed. Because a model's entire state is exactly "initial
/// state plus the ordered actions replayed against it", this both reconstructs
/// state from a durable log and powers `SessionLog::undoLast()` below.
///
/// @param modelTypeId String type-id of the model to reconstruct (`ModelTraits<M>::typeId()`).
/// @param entries     Ordered entries to replay, typically from `IActionLog::entries()`.
/// @param registry    Model factory registry; defaults to the process-level singleton.
/// @param dispatcher  Action dispatcher; defaults to the process-level singleton.
/// @return A freshly created holder with @p entries replayed against it.
/// @throws std::runtime_error if @p modelTypeId or any entry's action type is unregistered.
inline std::unique_ptr<::morph::model::detail::IModelHolder> replay(
    std::string_view modelTypeId, const std::vector<LogEntry>& entries,
    ::morph::model::detail::ModelRegistryFactory& registry = ::morph::model::detail::defaultRegistry(),
    ::morph::model::detail::ActionDispatcher& dispatcher = ::morph::model::detail::defaultDispatcher()) {
    auto holder = registry.create(modelTypeId);
    for (const auto& entry : entries) {
        dispatcher.dispatch(entry.modelType, entry.actionType, *holder, entry.payload);
    }
    return holder;
}

/// @brief Full-fidelity, in-memory log of one model instance's executed actions.
///
/// Attach directly via `IModelHolder::attachActionLog` — every successfully
/// executed loggable action is appended here in order, regardless of any
/// `ActionLogPolicy::coalesce` setting. This full history is the raw material
/// for `undoLast()`. `checkpoint()` is the one place coalescing actually
/// happens: entries accumulated since the last checkpoint are reduced by
/// `(modelType, entityKey, actionType)` — keeping only the latest occurrence
/// where the action's policy says `coalesce == true`, keeping every occurrence
/// otherwise — and only that reduced set reaches the durable sink.
///
/// @par Thread safety
/// All public methods are thread-safe (guarded by an internal mutex).
class SessionLog : public IActionLog {
public:
    /// @brief Appends @p entry, assigning a monotonically increasing `seq`.
    ///
    /// Always full fidelity: this is what `undoLast()` walks back through, so
    /// nothing is coalesced or dropped here.
    void append(LogEntry entry) override {
        std::scoped_lock lock{_mtx};
        entry.seq = ++_nextSeq;
        _all.push_back(std::move(entry));
    }

    /// @brief No-op — `checkpoint()` is this class's real commit point.
    void flush() override {}

    /// @brief Returns the full history (or one entity's slice of it) in append order.
    [[nodiscard]] std::vector<LogEntry> entries(std::string_view entityKey = {}) const override {
        std::scoped_lock lock{_mtx};
        if (entityKey.empty()) {
            return _all;
        }
        std::vector<LogEntry> out;
        for (const auto& entry : _all) {
            if (entry.entityKey == entityKey) {
                out.push_back(entry);
            }
        }
        return out;
    }

    /// @brief Drops the most recently appended entry and replays everything that
    ///        remains against a fresh model instance, reconstructing the state
    ///        the model was in immediately before that entry executed.
    ///
    /// No inverse/undo operations are needed on the action types themselves —
    /// this reuses `replay()` over a shorter prefix of the same log. A no-op
    /// (returns a freshly created, un-replayed holder) if the log is empty.
    ///
    /// @param modelTypeId String type-id of the model to reconstruct.
    /// @param registry    Model factory registry; defaults to the process-level singleton.
    /// @param dispatcher  Action dispatcher; defaults to the process-level singleton.
    std::unique_ptr<::morph::model::detail::IModelHolder> undoLast(
        std::string_view modelTypeId,
        ::morph::model::detail::ModelRegistryFactory& registry = ::morph::model::detail::defaultRegistry(),
        ::morph::model::detail::ActionDispatcher& dispatcher = ::morph::model::detail::defaultDispatcher()) {
        std::vector<LogEntry> remaining;
        {
            std::scoped_lock lock{_mtx};
            if (!_all.empty()) {
                _all.pop_back();
                _committedUpTo = std::min(_committedUpTo, _all.size());
            }
            remaining = _all;
        }
        return replay(modelTypeId, remaining, registry, dispatcher);
    }

    /// @brief Coalesces entries appended since the last checkpoint and forwards
    ///        the reduced set, in order, to @p durableSink; then flushes it.
    ///
    /// Advances the checkpoint regardless of whether @p durableSink->flush()
    /// throws, matching `IOfflineQueue`-style at-least-once semantics: a
    /// checkpoint is a forward-only commit point, not a transaction to retry.
    /// A no-op if nothing has been appended since the last checkpoint.
    ///
    /// @param durableSink Receives only the coalesced entries — never the raw stream.
    /// @param dispatcher  Supplies each action's `coalesce` policy; defaults to
    ///                    the process-level singleton (the same one actions were
    ///                    registered against via `BRIDGE_REGISTER_ACTION`).
    void checkpoint(IActionLog& durableSink,
                    ::morph::model::detail::ActionDispatcher& dispatcher = ::morph::model::detail::defaultDispatcher()) {
        std::vector<LogEntry> pending;
        {
            std::scoped_lock lock{_mtx};
            if (_committedUpTo >= _all.size()) {
                return;
            }
            pending.assign(_all.begin() + static_cast<std::ptrdiff_t>(_committedUpTo), _all.end());
            _committedUpTo = _all.size();
        }
        for (auto& entry : coalesced(pending, dispatcher)) {
            durableSink.append(std::move(entry));
        }
        durableSink.flush();
    }

private:
    /// @brief Reduces @p pending by `(modelType, entityKey, actionType)`: the
    ///        latest occurrence overwrites earlier ones (in place, preserving
    ///        first-seen position) wherever @p dispatcher says the action
    ///        coalesces; every other entry is kept as-is.
    static std::vector<LogEntry> coalesced(const std::vector<LogEntry>& pending,
                                           ::morph::model::detail::ActionDispatcher& dispatcher) {
        std::vector<LogEntry> out;
        std::unordered_map<std::string, std::size_t> lastIndexForKey;
        for (const auto& entry : pending) {
            if (!dispatcher.coalesce(entry.modelType, entry.actionType)) {
                out.push_back(entry);
                continue;
            }
            std::string key = entry.modelType + '\0' + entry.entityKey + '\0' + entry.actionType;
            auto iter = lastIndexForKey.find(key);
            if (iter == lastIndexForKey.end()) {
                lastIndexForKey.emplace(std::move(key), out.size());
                out.push_back(entry);
            } else {
                out[iter->second] = entry;
            }
        }
        return out;
    }

    mutable std::mutex _mtx;
    std::vector<LogEntry> _all;
    std::size_t _committedUpTo{0};
    uint64_t _nextSeq{0};
};

}  // namespace morph::journal
