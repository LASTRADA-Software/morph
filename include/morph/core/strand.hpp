// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <atomic>
#include <core/async/ExecutorContext.hpp>
#include <core/async/IExecutor.hpp>
#include <core/async/KeyedStrands.hpp>
#include <core/async/ParkedWork.hpp>
#include <core/async/Strand.hpp>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "../attributes.hpp"
#include "../session/session.hpp"
#include "executor.hpp"
#include "logger.hpp"

/// @file
/// @brief morph's strands: one per model instance, from core-cpp's
///        `core::async::KeyedStrands`, over a morph executor.
///
/// Specified in `docs/spec/core/executor.md`, "Strands".

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

/// @brief A `core::async::IExecutor` over a morph executor: how a core-cpp
///        strand's pump reaches a thread pool, a main-thread pump or Qt.
///
/// A strand hands its base one bare coroutine handle per turn, which this
/// posts as a lambda holding nothing else: trivially copyable, so it fits a
/// `std::function`'s small buffer and a turn costs no allocation. The lambda
/// does not refer to this object, so it may be destroyed while a pump it
/// posted is still queued: the pump finds its strand closed and ends.
class CoreExecutorOver final : public ::core::async::IExecutor {
public:
    /// @param executor Where every resumption is posted. Borrowed: it must
    ///        outlive every strand over this object and run what it queued.
    explicit CoreExecutorOver(::morph::exec::IExecutor& executor MORPH_LIFETIMEBOUND) : _executor{&executor} {}

    using ::core::async::IExecutor::submit;

    /// @brief Posts @p handle's resumption.
    /// @param handle The coroutine to resume; borrowed.
    void submit(std::coroutine_handle<> handle) override {
        _executor->post([handle] { handle.resume(); });
    }

    /// @brief Posts @p work's resumption. An executor that drops the task
    ///        unrun releases its claim, which frees a chain nobody owns.
    /// @param work The coroutine to resume, and its claim.
    void submit(::core::async::ParkedWork work) override {
        _executor->post([work] {
            work.abandon.disarm();
            work.resume.resume();
        });
    }

private:
    ::morph::exec::IExecutor* _executor;
};

/// @brief A callable posted to a strand, with its throw logged rather than
///        propagated.
///
/// A core-cpp strand lets a task's throw propagate to whoever resumed its
/// pump, and under MSVC's `cl` ends the process instead. morph's strands have
/// always logged a throwing task and gone on with the next; this keeps that
/// policy in morph, inside the one allocation the post costs.
/// @tparam F The callable's type.
template <typename F>
struct LoggedTask {
    F fn;

    void operator()() {
        try {
            fn();
        } catch (const std::exception& exc) {
            ::morph::log::logError("[strand] task threw: " + std::string{exc.what()});
        } catch (...) {
            ::morph::log::logError("[strand] task threw unknown exception");
        }
    }
};

class TaskResumer;

/// @brief In which order `ModelStrands::teardown` stops the Task handlers and
///        seals the strands.
enum class TeardownOrder : std::uint8_t {
    /// Stop, then seal: a stopped handler unwinds on its strand, which still
    /// admits its resumption. For a build with threads, where the strands keep
    /// running on the pool while the owner waits.
    StopThenSeal,
    /// Seal, then stop: a stopped handler's resumption is refused and runs
    /// inline, in the stop. For the single-threaded build, where nothing runs
    /// the strands while the owner tears them down.
    SealThenStop,
};

/// @brief The teardown order for this build: `StopThenSeal` where threads
///        exist, `SealThenStop` where they do not.
inline constexpr TeardownOrder buildTeardownOrder =
    CORE_CPP_ASYNC_HAS_THREADS ? TeardownOrder::StopThenSeal : TeardownOrder::SealThenStop;

/// @brief One strand per model instance over a morph executor: work for one
///        `ModelId` runs serially and in order, and work for different ids runs
///        concurrently where the executor has the threads.
///
/// `core::async::KeyedStrands<ModelId>`, with what morph adds to it:
/// - the base adapter (`CoreExecutorOver`), owned here;
/// - posted callables that log a throw (`LoggedTask`);
/// - the action's session, and the Task handler's resumer as the current
///   executor, around every task of a model instance whose Task handler has
///   started and not finished (`enroll`). That is the keyed around-task hook's
///   job: a handler that comes back to its strand through any awaitable finds
///   both installed.
///
/// Held by `std::shared_ptr`: a `TaskResumer` shares it, so a suspended
/// handler can still ask whether it is closed after its backend is gone.
class ModelStrands final {
public:
    /// @param base    Where every strand's pump runs. Borrowed: it must outlive
    ///                this object and keep running tasks until `drain` has
    ///                returned.
    /// @param options How each strand shares @p base; its `aroundTask` must be
    ///                unset, since these strands install their own.
    explicit ModelStrands(::morph::exec::IExecutor& base MORPH_LIFETIMEBOUND,
                          ::core::async::StrandOptions options = {})
        : _base{base}, _strands{_base, options, ::core::async::KeyedAroundTask<ModelId>::of(_hook)} {}

    ModelStrands(const ModelStrands&) = delete;
    ModelStrands& operator=(const ModelStrands&) = delete;
    ModelStrands(ModelStrands&&) = delete;
    ModelStrands& operator=(ModelStrands&&) = delete;
    ~ModelStrands() = default;

    /// @brief Queues @p task on @p key's strand, after everything queued there
    ///        before it. Dropped once the strands are closed.
    /// @param key  The model instance.
    /// @param task The callable; held by value in one allocation, and its throw
    ///        is logged.
    template <typename F>
    void post(ModelId key, F&& task) {
        _strands.post(key, LoggedTask<std::decay_t<F>>{std::forward<F>(task)});
    }

    /// @brief Runs @p task here if the calling thread is on @p key's strand,
    ///        posts it there otherwise, and runs it here once the strands are
    ///        closed.
    ///
    /// How a Task handler's end reaches its strand when the handler finished
    /// off it. Once the strands are closed their backend has drained them, so
    /// no strand task is left to race @p task.
    /// @param key  The model instance.
    /// @param task The callable.
    template <typename F>
    void runOnStrand(ModelId key, F task) {
        if (runningHere(key)) {
            task();
            return;
        }
        LoggedTask<F> logged{std::move(task)};
        if (!_strands.tryPost(key, logged)) {
            logged();
        }
    }

    /// @brief Queues @p handle on @p key's strand, unless the strands are closed.
    /// @param key    The model instance.
    /// @param handle The coroutine to resume there; borrowed.
    /// @return Whether it was queued.
    [[nodiscard]] bool trySubmit(ModelId key, std::coroutine_handle<> handle) {
        return _strands.trySubmit(key, handle);
    }

    /// @brief Queues @p work on @p key's strand, holding its claim until it
    ///        runs, unless the strands are closed.
    /// @param key  The model instance.
    /// @param work The coroutine and its claim; moved from only where this
    ///        returns true.
    /// @return Whether it was queued.
    [[nodiscard]] bool trySubmit(ModelId key, ::core::async::ParkedWork& work) {
        return _strands.trySubmit(key, work);
    }

    /// @brief Whether the calling thread is inside a task of @p key's strand.
    /// @param key The model instance.
    /// @return True inside that strand's task, at any depth.
    [[nodiscard]] bool runningHere(ModelId key) const noexcept { return _strands.runningHere(key); }

    /// @brief Whether the calling thread is inside a task of any of these strands.
    /// @return True inside any of their tasks.
    [[nodiscard]] bool runningAnyHere() const noexcept { return _strands.runningAnyHere(); }

    /// @brief Whether nothing is queued or running on any strand.
    /// @return True when idle. Racy by nature where other threads post.
    [[nodiscard]] bool idle() const { return _strands.idle(); }

    /// @brief Blocks until nothing is queued or running on any strand,
    ///        including work posted while it waits, where the build has
    ///        threads.
    ///
    /// Not from one of these strands' own tasks, which would wait for itself: a
    /// debug build asserts that. The single-threaded WebAssembly build has no
    /// other thread to finish the work and allows no blocking wait, so there
    /// this returns at once and `close` drops what is queued; a host that wants
    /// it run pumps its executor until `idle()` first.
    void drain() {
#if CORE_CPP_ASYNC_HAS_THREADS
        _strands.waitIdle();
#endif
    }

    /// @brief Closes every strand: queued work is dropped, a task running on
    ///        another thread is waited for, and later posts are dropped too.
    ///        Idempotent.
    void close() { _strands.close(); }

    /// @brief Refuses the try-forms and keeps running what is queued:
    ///        `trySubmit` and `runOnStrand`'s post are refused, so their callers
    ///        run the work inline; a plain `post` is still queued until
    ///        `close`. Idempotent.
    void seal() { _strands.seal(); }

    /// @brief Takes the strands down without losing work: stops the Task
    ///        handlers, seals, drains and closes, with the stop and the seal in
    ///        @p order.
    ///
    /// Whatever arrives once the strands are sealed -- a resumption, a
    /// handler's end -- is refused and runs inline, so nothing reaches a strand
    /// that the close would drop: not between the drain and the close, and not
    /// on the single-threaded build, where the drain waits for nothing.
    /// @param stopHandlers Requests stop on every Task handler still running.
    /// @param order        Which of stopping and sealing comes first.
    template <typename Stop>
    void teardown(Stop&& stopHandlers, TeardownOrder order = buildTeardownOrder) {
        if (order == TeardownOrder::SealThenStop) {
            seal();
            std::forward<Stop>(stopHandlers)();
        } else {
            std::forward<Stop>(stopHandlers)();
            seal();
        }
        drain();
        close();
    }

    /// @brief Installs @p resumer's session and executor around every task of
    ///        @p key's strand, until `withdraw(key, resumer)`.
    ///
    /// Called on the strand, when a Task handler starts. The action gate lets
    /// one action at a time run on a model instance, so a key has at most one
    /// suspended handler to resume. Held weakly: the handler's driver owns the
    /// resumer.
    /// @param key     The model instance.
    /// @param resumer The handler's resumer.
    void enroll(ModelId key, const std::shared_ptr<TaskResumer>& resumer) {
        std::unique_lock const lock{_enrolledMtx};
        _enrolled.insert_or_assign(key, resumer);
        _enrolledCount.store(_enrolled.size(), std::memory_order_release);
    }

    /// @brief Ends `enroll(key, resumer)`. Called when the handler has
    ///        finished; a key enrolled for another resumer since is left alone.
    /// @param key     The model instance.
    /// @param resumer The resumer that was enrolled for it.
    void withdraw(ModelId key, const TaskResumer* resumer) {
        std::unique_lock const lock{_enrolledMtx};
        if (auto const found = _enrolled.find(key);
            found != _enrolled.end() && found->second.lock().get() == resumer) {
            _enrolled.erase(found);
        }
        _enrolledCount.store(_enrolled.size(), std::memory_order_release);
    }

private:
    /// The keyed around-task hook: runs a task inside its model instance's
    /// enrolled resumer, if it has one. Touches nothing of this object after
    /// the task has run, which may have released the last reference to it.
    struct AroundTask {
        ModelStrands* self;
        void operator()(const ModelId& key, ::core::async::RunTask run) const;
    };

    CoreExecutorOver _base;
    /// Shared by the hook, which only reads: tasks of different keys do not
    /// serialise on it while handlers are enrolled.
    std::shared_mutex _enrolledMtx;
    /// How many keys are enrolled: the hook's one atomic load when none is.
    std::atomic<std::size_t> _enrolledCount{0};
    std::unordered_map<ModelId, std::weak_ptr<TaskResumer>, ModelIdHash> _enrolled;
    AroundTask _hook{this};
    /// Last, so it is destroyed first: its strands reference the base and the
    /// hook.
    ::core::async::KeyedStrands<ModelId, ModelIdHash> _strands;
};

/// @brief Resumes one Task handler's coroutines on its model instance's strand.
///
/// The current executor wherever the handler runs, so every awaitable that
/// resumes on the current executor -- morph's `Completion` and `delay`,
/// core-cpp's `AsyncQueue::pop` -- brings the handler back here, and `submit`
/// queues it on the strand. Each resumption runs with the action's session
/// installed: through the strand's around-task hook (`ModelStrands::enroll`),
/// or here.
///
/// Once the strands are closed, a resumption runs inline, on the thread that
/// submitted it, with the same context installed. The call it belongs to has
/// already failed then, and the model instance is reachable only through the
/// handler's own frame, so there is nothing left for the strand to serialise it
/// against.
///
/// Always held by `std::shared_ptr`.
class TaskResumer final : public ::core::async::IExecutor, public std::enable_shared_from_this<TaskResumer> {
public:
    /// @param strands The strands the model's actions run on.
    /// @param key     The model instance whose strand resumptions are queued on.
    /// @param session The action's session context, installed for each resumption.
    TaskResumer(std::shared_ptr<ModelStrands> strands, ModelId key, ::morph::session::Context session)
        : _strands{std::move(strands)}, _key{key}, _session{std::move(session)} {}

    using ::core::async::IExecutor::submit;

    /// @brief Queues @p handle's resumption on the model's strand, or resumes
    ///        it here once the strands are closed.
    /// @param handle The coroutine to resume; borrowed, as every handler frame
    ///        is owned by the driver that started it.
    void submit(std::coroutine_handle<> handle) override {
        if (!_strands->trySubmit(_key, handle)) {
            resumeHere(handle);
        }
    }

    /// @brief Queues @p work's resumption on the model's strand, with its
    ///        claim, or resumes it here once the strands are closed.
    ///
    /// The claim matters for a chain nobody owns: a `core::async::DetachedTask`
    /// started inside the handler that parks on an awaitable resuming on the
    /// current executor reaches here with its claim armed. Dropping the claim
    /// would free that frame while its handle is still queued.
    /// @param work The coroutine to resume, and its claim on the chain root.
    void submit(::core::async::ParkedWork work) override {
        if (_strands->trySubmit(_key, work)) {
            return;
        }
        // Resumed, so the chain goes back to its owner: disarmed first, as
        // core-cpp's own executors do before they resume a parked entry.
        work.abandon.disarm();
        resumeHere(work.resume);
    }

    /// @brief Resumes @p handle on the calling thread, inside this resumer.
    /// @param handle The coroutine to resume.
    void resumeHere(std::coroutine_handle<> handle) {
        within([handle] { handle.resume(); });
    }

    /// @brief Calls @p body with the action's session installed and this
    ///        resumer as the current executor.
    ///
    /// Touches no member after @p body returns: the body may release the last
    /// owner but the one this call holds.
    /// @param body The work, called once.
    template <typename Body>
    void within(Body&& body) {
        std::shared_ptr<void> const keep = shared_from_this();
        ::morph::session::detail::ScopedContext const scoped{_session};
        ::core::async::ExecutorScope const scope{*this, &keep, nullptr};
        std::forward<Body>(body)();
    }

private:
    std::shared_ptr<ModelStrands> _strands;
    ModelId _key;
    ::morph::session::Context _session;
};

inline void ModelStrands::AroundTask::operator()(const ModelId& key, ::core::async::RunTask run) const {
    if (self->_enrolledCount.load(std::memory_order_acquire) == 0) {
        run();
        return;
    }
    std::shared_ptr<TaskResumer> resumer;
    {
        std::shared_lock const lock{self->_enrolledMtx};
        if (auto const found = self->_enrolled.find(key); found != self->_enrolled.end()) {
            resumer = found->second.lock();
        }
    }
    if (!resumer) {
        run();
        return;
    }
    resumer->within(run);
}

}  // namespace morph::exec::detail
