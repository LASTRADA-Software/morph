// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

#include "../attributes.hpp"
#include "../core/logger.hpp"
#include "../core/observability.hpp"
#include "offline_queue.hpp"

namespace morph::offline {

/// @brief Aggregated result returned by `SyncWorker::run()`.
struct SyncResult {
    /// @brief Number of items successfully replayed and removed from the queue.
    int successful = 0;

    /// @brief Number of items that failed and were left in the queue for retry.
    int failed = 0;

    /// @brief Number of items that exhausted their retry budget and were dropped
    ///        from the queue (handed to the `DeadLetterSink` if one is set,
    ///        otherwise logged via `morph::log::logError`).
    int deadLettered = 0;
};

/// @brief Replays queued actions from an `IOfflineQueue` on reconnect.
///
/// `SyncWorker` drains the queue and calls a caller-supplied replay function
/// for each item. The framework has no knowledge of what "replay" means —
/// that is entirely the caller's domain. Retry semantics are baked in with
/// hard-coded sensible defaults; there are intentionally no setters.
///
/// @par Retry & dead-letter (built-in defaults)
/// - Each item is retried up to **5 cumulative attempts**. The count is seeded
///   from the larger of the drained `QueueItem::attempts` (durable, if the
///   queue persists it) and this worker's own in-memory count, so a queue that
///   overrides `IOfflineQueue::setAttempts()` makes the budget survive a
///   process restart; a queue that does not (the default no-op) keeps the
///   count purely in-memory, exactly as before.
/// - After every failed attempt, the new count is written back through
///   `IOfflineQueue::setAttempts()` (a no-op unless the queue overrides it) so
///   a persisting queue's next `drain()` — this run or after a restart — sees
///   the updated value.
/// - Items that fail their 5th cumulative attempt are dropped from the queue.
///   If a `DeadLetterSink` is set, it is invoked with the exhausted item
///   instead of the default log line; if unset, the item is logged at
///   `morph::log::LogLevel::error` (the payload appears in the log line) —
///   today's behavior, unchanged.
/// - Items that succeed reset their attempt counter implicitly (they're removed).
/// - There is no public knob on the retry cap — if your application needs
///   different retry math, wrap or replace `SyncWorker`. The framework's
///   promise is "obvious, safe defaults that the GUI never has to think about."
///
/// @par ReplayFunction contract
/// - Return `true`  → item successfully processed; it is removed from the queue.
/// - Return `false` → item failed; attempt counter incremented; left in queue if
///                    under the cap, otherwise dropped (dead-lettered).
/// - Throw          → treated as failure (same path as returning `false`).
///
/// @par Thread safety
/// `run()` is safe to call from any thread. Concurrent calls are serialised
/// by an internal mutex — the second caller blocks until the first `run()` completes.
class SyncWorker {
public:
    /// @brief Callable that attempts to replay a single queued item.
    using ReplayFunction = std::function<bool(const std::string& payload)>;

    /// @brief Callable invoked when an item exhausts its retry budget, just
    ///        before it is removed from the queue.
    ///
    /// Receives the exhausted item (payload, idempotencyKey, and its final
    /// cumulative attempt count). If unset, `SyncWorker` keeps the default
    /// behavior: log at `morph::log::LogLevel::error` and drop. A throwing
    /// sink is caught and logged — the item is still removed.
    using DeadLetterSink = std::function<void(const QueueItem& poisoned)>;

    /// @brief Constructs a worker that drains @p queue using @p replay.
    /// @param queue          Queue to drain on each `run()` call. Borrowed, not
    ///                       owned: it must outlive this worker.
    /// @param replay         Function called for each pending item. Stored and
    ///                       invoked for this worker's whole lifetime, so
    ///                       anything the callable refers to must outlive it.
    /// @param deadLetterSink Optional hook invoked with the exhausted item
    ///                       instead of the default log-and-drop path when an
    ///                       item exhausts its retry budget. Default: unset.
    ///                       Retained on the same terms as @p replay.
    SyncWorker(IOfflineQueue& queue MORPH_LIFETIMEBOUND, ReplayFunction replay MORPH_LIFETIMEBOUND,
               DeadLetterSink deadLetterSink MORPH_LIFETIMEBOUND = nullptr)
        : _queue{queue}, _replay{std::move(replay)}, _deadLetterSink{std::move(deadLetterSink)} {}

    /// @brief Drains the queue, replaying each item via the replay function.
    ///
    /// Concurrent calls are serialised. The call blocks until all pending items
    /// have been processed or `stop()` is signalled.
    ///
    /// If `stop()` was called before `run()` acquired the lock, `run()` returns
    /// immediately with an empty result and resets the stop flag.
    ///
    /// @return Counts of successful / failed / dead-lettered replays.
    SyncResult run() {
        std::scoped_lock const runLock{_runMtx};
        bool const wasStoppedBeforeRun = _stopped.exchange(false);
        SyncResult result;
        if (wasStoppedBeforeRun) {
            return result;
        }

        auto items = _queue.drain();
        ::morph::observe::detail::emitMetric(::morph::observe::Metric::queueDepth, static_cast<double>(items.size()));
        for (auto& item : items) {
            if (_stopped.load()) {
                break;
            }
            bool succeeded = false;
            try {
                succeeded = _replay(item.payload);
            } catch (...) {
                succeeded = false;
            }
            if (succeeded) {
                _queue.markDone(item.id);
                _attempts.erase(item.id);
                ++result.successful;
                continue;
            }
            // The effective count is the larger of the persisted item.attempts
            // (0 unless the queue overrides setAttempts) and this worker's own
            // in-memory count, so a durable queue's cross-restart count is
            // never regressed by a fresh (empty) in-memory map.
            auto& counter = _attempts[item.id];
            counter = std::max(counter, item.attempts);
            ++counter;
            _queue.setAttempts(item.id, counter);
            if (counter >= kMaxAttempts) {
                if (_deadLetterSink) {
                    QueueItem poisoned = item;
                    poisoned.attempts = counter;
                    try {
                        _deadLetterSink(poisoned);
                    } catch (...) {
                        ::morph::log::logError("[sync_worker] dead-letter sink threw while handling payload: " +
                                               item.payload);
                    }
                } else {
                    ::morph::log::logError("[sync_worker] dropping payload after " + std::to_string(kMaxAttempts) +
                                           " failed attempts: " + item.payload);
                }
                _queue.markDone(item.id);
                _attempts.erase(item.id);
                ++result.deadLettered;
            } else {
                ++result.failed;
            }
        }
        return result;
    }

    /// @brief Signals an in-progress `run()` to stop after the current item.
    ///
    /// Thread-safe. The flag is automatically reset at the start of the next
    /// `run()` call, so stopping is one-shot.
    void stop() { _stopped.store(true); }

private:
    /// @brief Cap on per-item cumulative retry attempts. Intentionally
    ///        hard-coded — see class docs.
    static constexpr uint32_t kMaxAttempts = 5;

    IOfflineQueue& _queue;
    ReplayFunction _replay;
    DeadLetterSink _deadLetterSink;
    std::mutex _runMtx;
    std::atomic<bool> _stopped{false};
    std::unordered_map<uint64_t, uint32_t> _attempts;
};

}  // namespace morph::offline
