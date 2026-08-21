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
#include "../core/model.hpp"
#include "../core/registry.hpp"

namespace morph::journal {

namespace detail {

/// @brief Thread-local flag telling executing model code whether the current
///        dispatch is happening inside `replay()`.
///
/// Installed by `replay()` around its dispatch loop via `ScopedReplayFlag`,
/// mirroring how `morph::session::detail::tlsCurrent()`/`ScopedContext` thread
/// a per-call `Context` through dispatch -- same shape (a thread-local slot
/// plus an RAII guard that restores the previous value on scope exit), applied
/// to a `bool` instead of a `const Context*`. Model/rule code never touches
/// this directly; it reads the public accessor `isReplaying()`.
inline bool& tlsIsReplaying() {
    thread_local bool tls = false;
    return tls;
}

/// @brief RAII helper that sets the thread-local replay flag for its scope,
///        restoring the previous value on destruction.
///
/// Nests correctly: a `replay()` call that itself triggers another `replay()`
/// (not a pattern this framework uses today, but not precluded) leaves the
/// flag `true` for the whole nested extent and restores the outer call's value
/// on the inner guard's destruction -- the same nesting behavior
/// `session::detail::ScopedContext` already has for `Context`.
class ScopedReplayFlag {
  public:
    /// @brief Sets the thread-local replay flag to `true`, saving whatever
    ///        value was there before.
    ScopedReplayFlag() : _previous{tlsIsReplaying()} { tlsIsReplaying() = true; }
    /// @brief Restores the saved value.
    ~ScopedReplayFlag() { tlsIsReplaying() = _previous; }

    ScopedReplayFlag(const ScopedReplayFlag&) = delete;
    ScopedReplayFlag& operator=(const ScopedReplayFlag&) = delete;
    ScopedReplayFlag(ScopedReplayFlag&&) = delete;
    ScopedReplayFlag& operator=(ScopedReplayFlag&&) = delete;

  private:
    bool _previous;
};

}  // namespace detail

/// @brief Returns `true` if the calling thread is currently inside a
///        `replay()` dispatch, `false` otherwise.
///
/// This is the signal Phase 6's automation-rules engine (and any other model
/// code that reacts to its own actions) checks before evaluating a rule: a
/// rule that fires again while `replay()` re-applies its recorded trigger
/// entry would double-apply a cascade that is also being replayed from its own
/// recorded entry (see `docs/spec/journal/journal.md`'s cascade-journaling
/// section). Reading this outside of any `replay()` call (the ordinary,
/// live-dispatch case) always returns `false`.
/// @return `true` during `replay()`'s dispatch loop on this thread, `false` otherwise.
[[nodiscard]] inline bool isReplaying() noexcept { return detail::tlsIsReplaying(); }

/// @brief Reconstructs model state by replaying @p entries, in order, against a
///        freshly created model instance.
///
/// Builds on the same `ModelRegistryFactory` / `ActionDispatcher` machinery
/// `RemoteServer` already uses to dispatch incoming requests — no separate
/// replay engine is needed. Because a model's entire state is exactly "initial
/// state plus the ordered actions replayed against it", this both reconstructs
/// state from a durable log and powers `SessionLog::undoLast()` below.
///
/// Sets `isReplaying()` to `true` for the duration of the dispatch loop below
/// (via `detail::ScopedReplayFlag`), so any model/rule code executed as part of
/// a replayed dispatch can tell it is being replayed rather than live-dispatched
/// -- this is what lets Phase 6's rules engine suppress rule evaluation on
/// replay while `replay()` re-applies the cascade's own recorded entry
/// unchanged. The flag is restored to its prior value (`false`, for any
/// ordinary top-level caller) once this function returns, so it never leaks
/// into dispatches that happen after `replay()` completes.
///
/// @param modelTypeId String type-id of the model to reconstruct (`ModelTraits<M>::typeId()`).
/// @param entries     Ordered entries to replay, typically from `IActionLog::entries()`. Entries
///                    with `outcome == Outcome::Failed` are skipped (see below).
/// @param registry    Model factory registry; defaults to the process-level singleton.
/// @param dispatcher  Action dispatcher; defaults to the process-level singleton.
/// @return A freshly created holder with @p entries replayed against it.
/// @throws std::runtime_error if @p modelTypeId or any entry's action type is unregistered.
inline std::unique_ptr<::morph::model::detail::IModelHolder> replay(
    std::string_view modelTypeId, const std::vector<LogEntry>& entries,
    ::morph::model::detail::ModelRegistryFactory& registry = ::morph::model::detail::defaultRegistry(),
    ::morph::model::detail::ActionDispatcher& dispatcher = ::morph::model::detail::defaultDispatcher()) {
    auto holder = registry.create(modelTypeId);
    // `registry.create` (via `ModelFactory::create`) auto-attaches the process
    // default action log. Detach it before replaying: reconstruction re-runs the
    // recorded actions, and without this each replayed dispatch would re-record
    // into the live sink, corrupting the very audit trail we are reading from.
    holder->attachActionLog(nullptr, {});
    const detail::ScopedReplayFlag replayFlag;
    for (const auto& entry : entries) {
        // A Failed entry (see action_log.hpp's `Outcome`) never mutated model
        // state -- Model::execute threw or the validator rejected it before any
        // state change -- so there is nothing to reconstruct from it. Worse,
        // re-dispatching it would very likely throw the same exception again
        // (the same rejected precondition), aborting reconstruction outright.
        // Skipping it is exactly "replay only committed facts", which is what
        // this function already promised before Failed entries could appear in
        // the same log stream (issue #23).
        if (entry.outcome == Outcome::Failed) {
            continue;
        }
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
/// All public methods are thread-safe. A history mutex guards `_all` and the
/// watermark; a separate forwarding mutex serializes `checkpoint()` bodies end
/// to end so concurrent checkpoints forward to the durable sink in append order
/// without blocking ordinary `append()` calls during the sink's I/O.
class SessionLog : public IActionLog {
public:
    /// @brief Appends @p entry, assigning a monotonically increasing `seq`.
    ///
    /// Always full fidelity: this is what `undoLast()` walks back through, so
    /// nothing is coalesced or dropped here.
    /// @param entry Entry to append; `seq` is overwritten regardless of the input value.
    void append(LogEntry entry) override {
        std::scoped_lock const lock{_mtx};
        entry.seq = ++_nextSeq;
        _all.push_back(std::move(entry));
    }

    /// @brief No-op — `checkpoint()` is this class's real commit point.
    void flush() override {}

    /// @brief Returns the full history (or one entity's slice of it) in append order.
    /// @param entityKey If non-empty, restricts the result to that entity's entries.
    /// @return Matching entries, in append order.
    [[nodiscard]] std::vector<LogEntry> entries(std::string_view entityKey = {}) const override {
        std::scoped_lock const lock{_mtx};
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
    /// Undo only rewinds this in-memory history; it never moves the checkpoint
    /// watermark, which is keyed by entry `seq` (see `checkpoint()`). Undoing an
    /// entry a prior `checkpoint()` already forwarded does **not** un-forward it:
    /// the durable sink is append-only, so that entry stays durable while the
    /// reconstructed history diverges from it. A later `checkpoint()` never
    /// re-forwards a coalesced-away, already-committed entry. Applications that
    /// need to durably reverse a checkpointed action must record a compensating
    /// action, not rely on `undoLast()`.
    ///
    /// @param modelTypeId String type-id of the model to reconstruct.
    /// @param registry    Model factory registry; defaults to the process-level singleton.
    /// @param dispatcher  Action dispatcher; defaults to the process-level singleton.
    /// @return A freshly created holder with the pre-undo state replayed onto it.
    std::unique_ptr<::morph::model::detail::IModelHolder> undoLast(
        std::string_view modelTypeId,
        ::morph::model::detail::ModelRegistryFactory& registry = ::morph::model::detail::defaultRegistry(),
        ::morph::model::detail::ActionDispatcher& dispatcher = ::morph::model::detail::defaultDispatcher()) {
        std::vector<LogEntry> remaining;
        {
            std::scoped_lock const lock{_mtx};
            if (!_all.empty()) {
                // The checkpoint watermark is a *seq* threshold, not a raw index
                // into `_all`, so simply dropping the tail entry is all that is
                // needed: `_committedUpToSeq` already names exactly the set of
                // entries a prior `checkpoint()` forwarded, and popping a
                // never-committed tail entry (the only kind whose loss is
                // recoverable) cannot lower it. The watermark is therefore
                // monotonic — undo never rewinds it, and a later `checkpoint()`
                // never re-forwards a coalesced-away, already-committed entry.
                // Undoing an entry that a prior checkpoint already forwarded does
                // not un-forward it from the durable sink (see the class docs and
                // journal.md): the sink is append-only, so that entry stays
                // durable and the reconstructed history simply diverges from it.
                _all.pop_back();
            }
            remaining = _all;
        }
        return replay(modelTypeId, remaining, registry, dispatcher);
    }

    /// @brief Coalesces entries appended since the last checkpoint and forwards
    ///        the reduced set, in order, to @p durableSink; then flushes it.
    ///
    /// Advances the checkpoint watermark (the `seq` of the last committed entry)
    /// *before* forwarding, so the batch is consumed even if @p durableSink's
    /// `append()` or `flush()` throws: a checkpoint is a forward-only commit
    /// point (at-most-once), not a transaction to retry. A no-op if nothing has
    /// been appended since the last checkpoint.
    ///
    /// @par Concurrency and ordering
    /// The whole checkpoint body runs under a dedicated forwarding mutex, so
    /// concurrent checkpoints are fully serialized: the batch whose watermark is
    /// taken first is also forwarded to @p durableSink first, and no two
    /// checkpoints ever interleave their `append()` calls. This preserves the
    /// append-order identity the sink relies on. The forwarding mutex is *not*
    /// the mutex that guards `append()`, which is held only briefly to select the
    /// pending entries and advance the watermark — never across the sink's I/O —
    /// so ordinary `append()` calls keep making progress during a checkpoint.
    ///
    /// @param durableSink Receives only the coalesced entries — never the raw stream.
    /// @param dispatcher  Supplies each action's `coalesce` policy; defaults to
    ///                    the process-level singleton (the same one actions were
    ///                    registered against via `BRIDGE_REGISTER_ACTION`).
    void checkpoint(IActionLog& durableSink,
                    ::morph::model::detail::ActionDispatcher& dispatcher = ::morph::model::detail::defaultDispatcher()) {
        // `_checkpointMtx` serializes the *entire* checkpoint body across
        // concurrent callers, so {take the pending slice + advance the
        // watermark} and {forward that slice to the sink} happen atomically with
        // respect to any other checkpoint. Without it, two concurrent
        // checkpoints could each grab a disjoint pending slice under `_mtx`,
        // release `_mtx`, then race on the unlocked forward phase — letting
        // batches reach the durable sink out of append order and breaking the
        // append-order identity the sink relies on. It is deliberately *not*
        // `_mtx`: `_mtx` is held only briefly (slice + watermark), never across
        // the sink's `append()`/`flush()` I/O, so regular `append()` calls keep
        // making progress while a checkpoint is forwarding.
        std::scoped_lock const checkpointLock{_checkpointMtx};
        std::vector<LogEntry> pending;
        uint64_t highestSeq = _committedUpToSeq;
        {
            std::scoped_lock const lock{_mtx};
            // Forward every entry whose seq is strictly past the last committed
            // seq. `seq` is assigned once at append and never reused, so this is
            // stable under coalescing and under `undoLast()` removing tail
            // entries — unlike a raw index into `_all`, which those mutate.
            for (const auto& entry : _all) {
                if (entry.seq > _committedUpToSeq) {
                    pending.push_back(entry);
                    highestSeq = std::max(highestSeq, entry.seq);
                }
            }
            if (pending.empty()) {
                return;
            }
            _committedUpToSeq = highestSeq;
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
                // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) — index from same loop
                out[iter->second] = entry;
            }
        }
        return out;
    }

    mutable std::mutex _mtx;
    std::mutex _checkpointMtx;
    std::vector<LogEntry> _all;
    uint64_t _committedUpToSeq{0};
    uint64_t _nextSeq{0};
};

}  // namespace morph::journal
