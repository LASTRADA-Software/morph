// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <string>

#include "offline_queue.hpp"

namespace morph::offline {

/// @brief Aggregated result returned by `SyncWorker::run()`.
struct SyncResult {
    /// @brief Number of items successfully replayed and removed from the queue.
    int successful = 0;

    /// @brief Number of items that failed and were left in the queue.
    int failed = 0;
};

/// @brief Replays queued actions from an `IOfflineQueue` on reconnect.
///
/// `SyncWorker` drains the queue and calls a caller-supplied replay function
/// for each item. The framework has no knowledge of what "replay" means —
/// that is entirely the caller's domain.
///
/// @par ReplayFunction contract
/// - Return `true`  → item successfully processed; it is removed from the queue.
/// - Return `false` → item failed; it is left in the queue for the next `run()`.
/// - Throw          → treated as failure; exception is swallowed, item stays.
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
    /// @return Counts of successful and failed replays.
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
                ++result.successful;
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
    IOfflineQueue& _queue;
    ReplayFunction _replay;
    std::mutex _runMtx;
    std::atomic<bool> _stopped{false};
};

}  // namespace morph::offline
