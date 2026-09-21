// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "../attributes.hpp"
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
    /// @param base Underlying executor that actually runs the tasks. Borrowed,
    ///             not owned: it must outlive this `StrandExecutor` *and* keep
    ///             running tasks until the destructor's wait has completed —
    ///             destroying it first deadlocks (see
    ///             `docs/spec/concurrency_and_lifetimes.md`, "Destruction
    ///             ordering").
    explicit StrandExecutor(IExecutor& base MORPH_LIFETIMEBOUND) : _base{&base} {}

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
        bool schedule = false;
        {
            // Hold _mapMtx across the whole slot-lookup + push + re-arm
            // decision, and take strand->mtx *while still holding _mapMtx*. The
            // drain-and-erase step in scheduleNext makes its "keep-running vs.
            // erase" decision under the same {_mapMtx, strand->mtx} pair, so the
            // two are mutually exclusive.
            //
            // Doing the lookup and the re-arm under separate locks once opened a
            // window: a post() could capture a strand, release _mapMtx, then
            // re-arm it under strand->mtx *after* a concurrent drain had already
            // erased it from the map — orphaning a live strand and letting a
            // later post(key) create a second strand for the same key that ran
            // concurrently. Because the map lookup and the re-arm now share
            // _mapMtx with the erase, the strand we push into is always the map's
            // current entry for this key: a strand that becomes `running` here is
            // guaranteed to still be the map entry, and it cannot be erased
            // out from under us (the erase needs _mapMtx too, and never fires
            // while pending is non-empty).
            //
            // Lock order is _mapMtx → strand->mtx, matching scheduleNext —
            // which acquires the two sequentially rather than with one
            // scoped_lock over both, precisely to keep this order (see its own
            // comment). A freshly created strand's mtx
            // is uncontended; an existing strand's mtx can only be held
            // elsewhere under the same _mapMtx-first order, so no deadlock.
            std::scoped_lock const mapLock{_mapMtx};
            auto& slot = _strands[key];
            if (!slot) {
                slot = std::make_shared<Strand>();
                slot->base = _base;
            }
            strand = slot;
            std::scoped_lock const strandLock{strand->mtx};
            strand->pending.push(std::move(task));
            if (!strand->running) {
                strand->running = true;
                schedule = true;
                // Account for the strand lambda we are about to dispatch *while
                // still holding _mapMtx*, before releasing it. If we deferred the
                // ++_inFlight into scheduleNext (which re-takes _mapMtx), a window
                // opened between releasing _mapMtx here and re-acquiring it there
                // in which ~StrandExecutor could observe _inFlight == 0, destroy
                // _strands, and let scheduleNext touch freed state. Incrementing
                // under this lock makes "decided to schedule" and "counted as
                // in-flight" atomic, so the destructor never sees a zero it should
                // not.
                ++_inFlight;
            }
        }
        if (schedule) {
            scheduleNext(strand, key);
        }
    }

private:
    /// @brief FIFO of tasks queued on one strand, with the head task held inline.
    ///
    /// Behaviourally a `std::queue<std::function<void()>>` restricted to the
    /// three operations the strand uses, and used under exactly the same
    /// discipline: every call happens with the owning `Strand::mtx` held, so
    /// this type does no locking of its own.
    ///
    /// It exists because of what the *container* cost, not what the strand
    /// did with it. The drain-and-erase step in `scheduleNext` destroys the
    /// whole `Strand` as soon as the queue empties, so a workload that
    /// dispatches one action at a time against a model builds a fresh queue on
    /// every call and puts exactly one task in it. libstdc++'s `std::deque`
    /// allocates its node map *and* a first 512-byte buffer in its default
    /// constructor, so that came to 576 bytes of the 760 the strand cost per
    /// local dispatch (morph#660). Holding the head task in the strand makes
    /// that case allocation-free; the overflow deque is constructed only when
    /// a second task is genuinely queued behind a running one, after which the
    /// cost is the deque's as before.
    ///
    /// This changes no lifetime or locking rule: the erase still happens when
    /// `empty()` becomes true, still under the `{_mapMtx, strand->mtx}` pair.
    class PendingQueue {
    public:
        /// @brief Reports whether the queue holds no task.
        /// @return `true` when nothing is queued.
        [[nodiscard]] bool empty() const noexcept { return !_hasHead; }

        /// @brief Appends @p task to the back of the queue.
        /// @param task Callable to queue. An *empty* `std::function` is queued
        ///             and later dispatched like any other: occupancy is
        ///             tracked by a separate flag rather than by testing the
        ///             callable, so this type never silently drops one.
        void push(std::function<void()>&& task) {
            if (!_hasHead) {
                _head = std::move(task);
                _hasHead = true;
                return;
            }
            if (!_overflow) {
                _overflow = std::make_unique<std::deque<std::function<void()>>>();
            }
            _overflow->push_back(std::move(task));
        }

        /// @brief Removes the task at the front of the queue and returns it.
        /// @return The front task.
        /// @pre `!empty()`.
        std::function<void()> pop() {
            std::function<void()> task = std::move(_head);
            if (_overflow && !_overflow->empty()) {
                _head = std::move(_overflow->front());
                _overflow->pop_front();
            } else {
                // A moved-from std::function is valid but unspecified; clear it
                // explicitly so the slot holds no captured state while idle.
                _head = nullptr;
                _hasHead = false;
            }
            return task;
        }

    private:
        std::function<void()> _head;
        std::unique_ptr<std::deque<std::function<void()>>> _overflow;
        bool _hasHead = false;
    };

    struct Strand {
        IExecutor* base = nullptr;
        std::mutex mtx;
        PendingQueue pending;
        bool running = false;
    };

    /// @brief Dispatches one strand lambda onto the base executor.
    ///
    /// **Precondition:** the caller must have already incremented `_inFlight`
    /// (under `_mapMtx`) to account for this dispatch. `post()` does so in the
    /// same critical section that decides to schedule, and the re-entrant call
    /// below does so under the `_mapMtx` it already holds. Keeping the increment
    /// with the *decision* (rather than here) closes the window where
    /// `~StrandExecutor` could observe `_inFlight == 0` between the decision and
    /// this dispatch and destroy `_strands` out from under us.
    void scheduleNext(const std::shared_ptr<Strand>& strand, ModelId key) {
        strand->base->post([this, strand, key] {
            std::function<void()> task;
            {
                std::scoped_lock const lock{strand->mtx};
                task = strand->pending.pop();
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
                // Same lock order as post(): _mapMtx first, then strand->mtx.
                // Acquiring them sequentially (rather than via a single
                // scoped_lock over both, whose std::lock back-off can grab them
                // in address order) keeps a single, consistent ordering across
                // every site that holds both, so there is no lock-ordering
                // deadlock. This is the point where "drain-and-erase" is decided
                // atomically w.r.t. a concurrent post(): a post() re-arming this
                // strand and this block erasing it cannot interleave, because
                // both hold _mapMtx across the whole decision.
                std::scoped_lock const mapLock{_mapMtx};
                std::scoped_lock const strandLock{strand->mtx};
                more = !strand->pending.empty();
                if (!more) {
                    strand->running = false;
                    auto iter = _strands.find(key);
                    if (iter != _strands.end() && iter->second == strand) {
                        _strands.erase(iter);
                    }
                } else {
                    // Account for the re-armed dispatch *before* releasing
                    // _mapMtx (scheduleNext's precondition). Because this run's
                    // own decrement below has not happened yet, _inFlight is
                    // briefly 2 here and never dips to 0 across the handoff, so
                    // ~StrandExecutor cannot slip in and destroy _strands between
                    // the two runs.
                    ++_inFlight;
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
