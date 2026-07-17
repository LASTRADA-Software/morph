// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>

#include "executor.hpp"
#include "logger.hpp"

namespace morph::exec::detail {

/// @brief Opaque identifier for a model instance inside a backend.
///
/// The value 0 is reserved and means "not bound". All non-zero values are
/// assigned by the backend and are stable for the lifetime of the model.
struct ModelId {
    /// @brief Raw numeric id. Zero means unbound.
    uint64_t v{0};

    /// @brief Three-way comparison — enables `==`, `!=`, `<`, `<=`, `>`, `>=`.
    auto operator<=>(const ModelId&) const = default;
};

/// @brief Hash functor so `ModelId` can be used as an `unordered_map` key.
struct ModelIdHash {
    /// @brief Returns the hash of @p mid.
    std::size_t operator()(ModelId mid) const noexcept { return std::hash<uint64_t>{}(mid.v); }
};

/// @brief Per-key serialising executor built on top of an arbitrary `IExecutor`.
///
/// Tasks posted with the same `ModelId` key are always executed in FIFO order
/// with no overlap, even when the underlying executor is a thread pool. Tasks
/// with different keys may run concurrently.
///
/// This removes the need for per-model mutexes: the model's `execute()` method
/// is always called from exactly one task at a time.
// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions)
class StrandExecutor {
public:
    /// @brief Constructs the strand executor wrapping @p base.
    /// @param base Underlying executor that actually runs the tasks.
    explicit StrandExecutor(IExecutor& base) : _base{&base} {}

    /// @brief Blocks until all in-flight tasks have completed, then destroys the executor.
    ///
    /// Waits for all in-flight lambdas to complete before destroying the map.
    /// Without this, a pool thread running scheduleNext can access _strands
    /// after it has been destroyed (TSan: data race on destructor vs erase).
    ~StrandExecutor() {
        std::unique_lock lock{_mapMtx};
        _cv.wait(lock, [this] { return _inFlight == 0; });
    }

    /// @brief Posts @p task to the strand associated with @p key.
    ///
    /// The task is guaranteed to run after all previously posted tasks for the
    /// same key have completed. Tasks for different keys may interleave freely.
    /// Thread-safe.
    /// @param key  Model identifier that selects the strand.
    /// @param task Callable to execute.
    void post(ModelId key, std::function<void()> task) {
        std::shared_ptr<Strand> strand;
        {
            std::scoped_lock const lock{_mapMtx};
            auto& slot = _strands[key];
            if (!slot) {
                slot = std::make_shared<Strand>();
                slot->base = _base;
            }
            strand = slot;
        }
        bool schedule = false;
        {
            std::scoped_lock const lock{strand->mtx};
            strand->pending.push(std::move(task));
            if (!strand->running) {
                strand->running = true;
                schedule = true;
            }
        }
        if (schedule) {
            scheduleNext(strand, key);
        }
    }

private:
    struct Strand {
        IExecutor* base = nullptr;
        std::mutex mtx;
        std::queue<std::function<void()>> pending;
        bool running = false;
    };

    void scheduleNext(const std::shared_ptr<Strand>& strand, ModelId key) {
        {
            std::scoped_lock const lock{_mapMtx};
            ++_inFlight;
        }
        strand->base->post([this, strand, key] {
            std::function<void()> task;
            {
                std::scoped_lock const lock{strand->mtx};
                task = std::move(strand->pending.front());
                strand->pending.pop();
            }
            try {
                task();
            } catch (const std::exception& exc) {
                // The strand is where Model::execute() actually runs; a throw
                // here must not stall the strand or vanish — log and continue so
                // the next queued task for this model still runs.
                ::morph::log::logError("[strand] task threw: " + std::string{exc.what()});
            } catch (...) {
                ::morph::log::logError("[strand] task threw unknown exception");
            }
            // Decide "keep running vs. drain-and-erase" atomically across the
            // map slot and the strand's pending queue. Doing it in two steps
            // (flip running under strand->mtx, then erase under _mapMtx) opened
            // a window where another post() could re-arm this strand after we
            // unlocked strand->mtx but before we erased the map entry. The
            // subsequent erase then orphaned a live strand: a later post(key)
            // would create a *new* strand for the same key, and the two
            // strands could run model tasks concurrently → data race.
            bool more = false;
            {
                std::scoped_lock const lock{_mapMtx, strand->mtx};
                more = !strand->pending.empty();
                if (!more) {
                    strand->running = false;
                    auto iter = _strands.find(key);
                    if (iter != _strands.end() && iter->second == strand) {
                        _strands.erase(iter);
                    }
                }
            }
            if (more) {
                scheduleNext(strand, key);
            }
            // Decrement after all map access is done; wake destructor if it is waiting.
            {
                std::scoped_lock const lock{_mapMtx};
                if (--_inFlight == 0) {
                    _cv.notify_all();
                }
            }
        });
    }

    IExecutor* _base;
    std::mutex _mapMtx;
    std::condition_variable _cv;
    int _inFlight{0};
    std::unordered_map<ModelId, std::shared_ptr<Strand>, ModelIdHash> _strands;
};

}  // namespace morph::exec::detail
