// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

#include "logger.hpp"
#include "offline_queue.hpp"

namespace morph::offline {

/// @brief Aggregated result returned by `SyncWorker::run()`.
struct SyncResult {
    /// @brief Number of items successfully replayed and removed from the queue.
    int successful = 0;

    /// @brief Number of items that failed and were left in the queue for retry.
    int failed = 0;

    /// @brief Number of items that exhausted their retry budget and were dropped
    ///        from the queue (logged via `morph::log::logError`).
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
/// - Each item is retried up to **5 attempts** across successive `run()` calls.
/// - Items that fail their 5th attempt are dropped from the queue and logged at
///   `morph::log::LogLevel::error` (so the host application sees them through
///   whatever sink it installed). The payload appears in the log line.
/// - Items that succeed reset their attempt counter implicitly (they're removed).
/// - There is no public knob — if your application needs different retry math,
///   wrap or replace `SyncWorker`. The framework's promise is "obvious, safe
///   defaults that the GUI never has to think about."
///
/// @par ReplayFunction contract
/// - Return `true`  → item successfully processed; it is removed from the queue.
/// - Return `false` → item failed; attempt counter incremented; left in queue if
///                    under the cap, otherwise dropped + logged.
/// - Throw          → treated as failure (same path as returning `false`).
///
/// @par Thread safety
/// `run()` is safe to call from any thread. Concurrent calls are serialised
/// by an internal mutex — the second caller blocks until the first `run()` completes.
class SyncWorker {
public:
    /// @brief Callable that attempts to replay a single queued item.
    using ReplayFunction = std::function<bool(const std::string& payload)>;

    /// @brief Constructs a worker that drains @p queue using @p replay.
    /// @param queue  Queue to drain on each `run()` call.
    /// @param replay Function called for each pending item.
    SyncWorker(IOfflineQueue& queue, ReplayFunction replay) : _queue{queue}, _replay{std::move(replay)} {}

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
        std::scoped_lock runLock{_runMtx};
        bool wasStoppedBeforeRun = _stopped.exchange(false);
        SyncResult result;
        if (wasStoppedBeforeRun) {
            return result;
        }

        for (auto& item : _queue.drain()) {
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
            auto attempts = ++_attempts[item.id];
            if (attempts >= kMaxAttempts) {
                ::morph::log::logError(
                    "[sync_worker] dropping payload after " + std::to_string(kMaxAttempts) +
                    " failed attempts: " + item.payload);
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
    /// @brief Cap on per-item retry attempts. Intentionally hard-coded — see class docs.
    static constexpr int kMaxAttempts = 5;

    IOfflineQueue& _queue;
    ReplayFunction _replay;
    std::mutex _runMtx;
    std::atomic<bool> _stopped{false};
    std::unordered_map<uint64_t, int> _attempts;
};

}  // namespace morph::offline
