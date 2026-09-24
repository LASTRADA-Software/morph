// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <concepts>
#include <condition_variable>
#include <core/async/IExecutor.hpp>
#include <core/async/ParkedWork.hpp>
#include <core/async/StopToken.hpp>
#include <core/async/Task.hpp>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include "../../session/session.hpp"
#include "../strand.hpp"
#include "completion_awaiter.hpp"

/// @file
/// @brief What drives a model's Task handler: the strand executor its
///        resumptions go through, the driver coroutine that starts and finishes
///        it, the per-instance gate that keeps actions from overlapping, and the
///        trait that tells a Task handler from an ordinary one.
///
/// Specified in `docs/spec/core/coroutines.md`, "Model side".

namespace morph::exec {

namespace detail {

/// @brief A backend's strand, as the Task handlers it started see it: usable
///        until the backend closes it.
///
/// A suspended Task handler outlives the strand task that started it, and can
/// outlive the backend too: `Bridge::switchBackend` fails every pending call
/// and destroys the outgoing `LocalBackend` without waiting for a handler
/// suspended on work that has not completed. So a `StrandCoroExecutor` holds
/// this link rather than the strand. The backend shares the link, and its
/// destructor calls `close()` before the strand is destroyed; `close()` waits
/// out every `post` already under way, so nothing reaches the strand after
/// it.
///
/// The strand is not shared itself because `~StrandExecutor` waits for its
/// in-flight tasks. The last owner of a shared strand could be let go by one
/// of those tasks, and that task's thread would then wait for itself.
class StrandLink {
public:
    /// @param strand The strand; must outlive every `post` that returns true,
    ///        which `close()` before its destruction guarantees.
    explicit StrandLink(StrandExecutor& strand MORPH_LIFETIMEBOUND) : _strand{&strand} {}

    /// @brief Posts @p task to @p key's strand, unless the link is closed.
    /// @param key  The model instance whose strand runs @p task.
    /// @param task The task.
    /// @return True if posted; false once closed, leaving @p task unrun.
    bool post(ModelId key, std::function<void()> task) {
        StrandExecutor* strand = nullptr;
        {
            std::scoped_lock const lock{_mtx};
            if (_strand == nullptr) {
                return false;
            }
            strand = _strand;
            ++_posting;
        }
        // Outside the lock: over an inline base executor the task runs inside
        // this call, and a handler it resumes may post again.
        PostingScope const posting{*this};
        strand->post(key, std::move(task));
        return true;
    }

    /// @brief Whether the calling thread is running a task of @p key's strand.
    /// @param key The model instance whose strand to ask about.
    /// @return False once closed: there is no strand left to be on.
    [[nodiscard]] bool runningHere(ModelId key) {
        std::scoped_lock const lock{_mtx};
        return _strand != nullptr && _strand->runningHere(key);
    }

    /// @brief Posts @p task to @p key's strand, or runs it here if the calling
    ///        thread is already on that strand or the link is closed.
    ///
    /// How a handler's end reaches its strand when the handler finished off it:
    /// on a core-cpp or other foreign awaiter's executor. Once the link is
    /// closed its backend has drained the strand, so no strand task is left to
    /// race @p task. Of the actions still queued in the gate, those whose calls
    /// `cancelPending` failed are skipped; any other runs after @p task, as the
    /// gate orders it, never beside it.
    /// @param key  The model instance whose strand runs @p task.
    /// @param task The task.
    void runOnStrand(ModelId key, std::function<void()> task) {
        if (runningHere(key)) {
            task();
            return;
        }
        std::function<void()> const fallback = task;
        if (!post(key, std::move(task))) {
            fallback();
        }
    }

    /// @brief Posts @p task to @p key's strand, even from that strand, or runs
    ///        it here once the link is closed.
    ///
    /// For a task whose captures must be destroyed outside any task of the
    /// strand: the strand destroys a task only once it has counted it finished,
    /// and this keeps no copy of @p task behind. A closed link's backend has
    /// drained the strand, so no strand task is left to race @p task.
    /// @param key  The model instance whose strand runs @p task.
    /// @param task The task.
    void postOrRun(ModelId key, std::function<void()> task) {
        StrandExecutor* strand = nullptr;
        {
            std::scoped_lock const lock{_mtx};
            if (_strand != nullptr) {
                strand = _strand;
                ++_posting;
            }
        }
        if (strand == nullptr) {
            task();
            return;
        }
        PostingScope const posting{*this};
        strand->post(key, std::move(task));
    }

    /// @brief Refuses every later `post`, and waits for those under way.
    void close() {
        std::unique_lock lock{_mtx};
        _strand = nullptr;
        _idle.wait(lock, [this] { return _posting == 0; });
    }

private:
    /// Ends one `post`, whether the strand accepted the task or threw.
    struct PostingScope {
        explicit PostingScope(StrandLink& link) noexcept : _link{&link} {}
        PostingScope(const PostingScope&) = delete;
        PostingScope& operator=(const PostingScope&) = delete;
        PostingScope(PostingScope&&) = delete;
        PostingScope& operator=(PostingScope&&) = delete;
        ~PostingScope() {
            std::scoped_lock const lock{_link->_mtx};
            if (--_link->_posting == 0) {
                _link->_idle.notify_all();
            }
        }

    private:
        StrandLink* _link;
    };

    std::mutex _mtx;
    std::condition_variable _idle;
    StrandExecutor* _strand;
    std::size_t _posting = 0;
};

}  // namespace detail

/// @brief Resumes coroutines on one model's strand.
///
/// Every `submit` posts `h.resume()` onto the strand of the model it was made
/// for, so each resumption of a Task handler runs serialised with that model's
/// other work. The posted task installs this executor as the resumption context
/// -- so a handler that awaits another model's completion comes back here rather
/// than to wherever that completion was delivered -- and the action's session
/// context, as the strand task that started the handler did.
///
/// Once the backend that owns the strand is gone (see `detail::StrandLink`), a
/// resumption runs inline, on the thread that submitted it, with the same
/// context installed. The call it belongs to has already failed then, and the
/// model instance is reachable only through the handler's own frame, so there
/// is nothing left for the strand to serialise it against.
///
/// Shared by the driver and by every posted resumption; always held by
/// `std::shared_ptr`.
class StrandCoroExecutor final : public ::core::async::IExecutor,
                                 public std::enable_shared_from_this<StrandCoroExecutor> {
public:
    /// @param link    The link to the strand the model's actions run on.
    /// @param key     The model instance whose strand resumptions are posted to.
    /// @param session The action's session context, installed for each resumption.
    StrandCoroExecutor(std::shared_ptr<detail::StrandLink> link, detail::ModelId key,
                       ::morph::session::Context session)
        : _link{std::move(link)}, _key{key}, _session{std::move(session)} {}

    using ::core::async::IExecutor::submit;

    /// @brief Posts @p handle's resumption onto the model's strand, or resumes
    ///        it here once the strand is gone.
    /// @param handle The coroutine to resume; borrowed, as every handler frame is
    ///        owned by the driver that started it.
    void submit(std::coroutine_handle<> handle) override {
        auto self = shared_from_this();
        if (_link->post(_key, [self, handle] { self->resumeHere(handle); })) {
            return;
        }
        resumeHere(handle);
    }

    /// @brief Posts @p work's resumption onto the model's strand.
    /// @param work The coroutine to resume. Its abandon claim is not taken: the
    ///        driver owns every handler frame, so there is nothing here to free.
    void submit(::core::async::ParkedWork work) override { submit(work.resume); }

private:
    void resumeHere(std::coroutine_handle<> handle) {
        ::morph::session::detail::ScopedContext const scoped{_session};
        ::morph::async::detail::ScopedResumeContext const context{this};
        handle.resume();
    }

    std::shared_ptr<detail::StrandLink> _link;
    detail::ModelId _key;
    ::morph::session::Context _session;
};

}  // namespace morph::exec

namespace morph::model {

/// @brief The result an action handler's return type stands for: `R` for a
///        `core::async::Task<R>` handler, the type itself for any other.
/// @tparam T The handler's return type.
template <typename T>
struct HandlerResult {
    /// @brief The action's result type.
    using type = T;
    /// @brief Whether the handler is a coroutine returning `core::async::Task`.
    static constexpr bool isTask = false;
};

/// @brief `HandlerResult` for a Task handler.
/// @tparam R The value the Task produces.
template <typename R>
struct HandlerResult<::core::async::Task<R>> {
    /// @brief The action's result type: what the Task produces.
    using type = R;
    /// @brief Whether the handler is a coroutine returning `core::async::Task`.
    static constexpr bool isTask = true;
};

/// @brief The action result a handler returning @p T produces.
/// @tparam T The handler's return type.
template <typename T>
using HandlerResultT = HandlerResult<T>::type;

/// @brief Whether a handler returning @p T is a Task handler.
/// @tparam T The handler's return type.
template <typename T>
inline constexpr bool isTaskHandler = HandlerResult<T>::isTask;

namespace detail {

/// @brief Keeps a model instance's actions from overlapping, across a Task
///        handler's suspensions.
///
/// A strand serialises the tasks posted to it, and a suspended Task handler is
/// not one: while it waits the strand is free. Every action therefore enters
/// this gate on the strand before it runs and leaves it when it is finished --
/// for a Task handler, when the Task completes. An action that finds the gate
/// held is queued, in arrival order, and started by the `leave()` that frees
/// it. Touched only on the model's strand, so it takes no lock.
class ActionGate {
public:
    /// @brief Starts @p start now, or queues it behind the action holding the gate.
    ///
    /// The started action holds the gate until it calls `leave()`.
    ///
    /// A template rather than a `std::function` parameter: an action that
    /// starts at once is called as it is, and only one that has to wait is
    /// type-erased into the queue. libstdc++'s `std::function` heap-allocates
    /// any callable that is not trivially copyable, a lambda holding a
    /// `shared_ptr` included, and an action on a free gate need not pay that.
    /// @param start The action; must not throw. Copyable, as the queue's
    ///              `std::function` requires.
    template <std::invocable Start>
    void enter(Start&& start) {
        Occupancy const occupancy{*this};
        if (_held || (_waiting != nullptr && !_waiting->empty())) {
            if (_waiting == nullptr) {
                _waiting = std::make_unique<std::deque<std::function<void()>>>();
            }
            _waiting->emplace_back(std::forward<Start>(start));
            return;
        }
        _held = true;
        start();
    }

    /// @brief Takes the gate if it is free, for an action its caller then runs
    ///        itself; otherwise leaves it to `enter` to queue the action.
    ///
    /// For a caller whose action must stay owned where it is while it runs:
    /// a backend's strand task, whose captures the strand destroys only once it
    /// has counted the task finished. The last reference to a `RemoteServer`
    /// dropped inside the task instead would run `~StrandExecutor`, which waits
    /// for that very task.
    /// @return True if taken: the caller runs its action now, and the action
    ///         holds the gate until it calls `leave()`. False if the action has
    ///         to wait, through `enter`.
    [[nodiscard]] bool tryEnter() {
        Occupancy const occupancy{*this};
        if (_held || (_waiting != nullptr && !_waiting->empty())) {
            return false;
        }
        _held = true;
        return true;
    }

    /// @brief Releases the gate and starts the actions queued behind it, oldest
    ///        first, for as long as each one finishes without suspending.
    ///
    /// A loop rather than a recursion: a queue of ordinary handlers would
    /// otherwise nest one stack frame per queued action.
    void leave() {
        Occupancy const occupancy{*this};
        _held = false;
        if (_draining) {
            return;
        }
        _draining = true;
        while (!_held && _waiting != nullptr && !_waiting->empty()) {
            auto next = std::move(_waiting->front());
            _waiting->pop_front();
            _held = true;
            next();
        }
        _draining = false;
    }

    /// @brief How many times, process-wide, two threads have been inside
    ///        `enter` or `leave` of one gate at once.
    ///
    /// Always zero when the gate is used as specified: a gate is touched only
    /// on its model's strand. A diagnostic for tests, which assert that it
    /// does not change; the check that counts is one atomic compare-exchange
    /// per call.
    /// @return The count so far.
    [[nodiscard]] static std::size_t overlapsObserved() noexcept { return overlapCounter().load(); }

private:
    static std::atomic<std::size_t>& overlapCounter() noexcept {
        static std::atomic<std::size_t> counter{0};
        return counter;
    }

    /// Marks the calling thread as inside the gate for the scope's lifetime,
    /// and counts it if another thread already is. Reentry on the same thread,
    /// which `leave()` starting the next action does, is not an overlap.
    class Occupancy {
    public:
        explicit Occupancy(ActionGate& gate) noexcept : _gate{&gate} {
            auto expected = std::thread::id{};
            auto const self = std::this_thread::get_id();
            _owner = _gate->_occupant.compare_exchange_strong(expected, self);
            if (!_owner && expected != self) {
                overlapCounter().fetch_add(1);
            }
        }
        Occupancy(const Occupancy&) = delete;
        Occupancy& operator=(const Occupancy&) = delete;
        Occupancy(Occupancy&&) = delete;
        Occupancy& operator=(Occupancy&&) = delete;
        ~Occupancy() {
            if (_owner) {
                _gate->_occupant.store(std::thread::id{});
            }
        }

    private:
        ActionGate* _gate;
        bool _owner = false;
    };

    std::atomic<std::thread::id> _occupant;
    bool _held = false;
    bool _draining = false;
    /// Allocated the first time an action has to wait, which only a suspended
    /// Task handler causes: every model instance carries a gate, and one that
    /// never queues should cost a model holder nothing but a pointer.
    std::unique_ptr<std::deque<std::function<void()>>> _waiting;
};

/// @brief The coroutine that runs one Task handler to completion.
///
/// Created suspended so its promise's stop token can be set before the first
/// step; `startTaskHandler` resumes it. It frees itself at the end
/// (`final_suspend` never suspends). Its promise answers `stopToken()`, which is
/// how the token reaches the handler: `Task`'s awaiter copies the awaiting
/// promise's token into the handler's.
struct TaskHandlerDriver {
    // The compiler calls every member of a promise through the object, so none
    // is made static.
    // NOLINTBEGIN(readability-convert-member-functions-to-static)
    struct promise_type {
        ::core::async::StopToken token;

        TaskHandlerDriver get_return_object() noexcept {
            return TaskHandlerDriver{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        [[nodiscard]] std::suspend_always initial_suspend() const noexcept { return {}; }
        [[nodiscard]] std::suspend_never final_suspend() const noexcept { return {}; }
        void return_void() const noexcept {}
        // Every exception is caught in the driver's body, which settles the call
        // with it; one escaping here would mean the settle itself threw.
        [[noreturn]] void unhandled_exception() const noexcept { std::terminate(); }
        [[nodiscard]] ::core::async::StopToken stopToken() const noexcept { return token; }
    };
    // NOLINTEND(readability-convert-member-functions-to-static)

    std::coroutine_handle<promise_type> handle;
};

/// @brief The driver body: awaits @p task and hands its outcome to @p done.
/// @tparam R The handler's result type.
/// @param executor The model's strand executor. Held in the frame: every
///        awaiter the handler suspends on keeps only a raw pointer to it, as its
///        resumption context, so it must live until the handler has finished.
/// @param task     The handler's Task, not yet started.
/// @param done     Called once, on the strand, with the result or the exception.
/// @return The suspended driver.
template <typename R>
TaskHandlerDriver driveTaskHandler(std::shared_ptr<::morph::exec::StrandCoroExecutor> executor,
                                   ::core::async::Task<R> task,
                                   std::function<void(std::optional<R>, std::exception_ptr)> done) {
    static_cast<void>(executor);
    std::optional<R> result;
    std::exception_ptr error;
    try {
        // The await is its own statement: MSVC cannot tail-call a call that
        // shares a full-expression with a `co_await` (C4737).
        R value = co_await std::move(task);
        result.emplace(std::move(value));
    } catch (...) {
        error = std::current_exception();
    }
    done(std::move(result), error);
}

/// @brief Starts a Task handler on the model's strand, in the calling strand task.
///
/// The handler's first step runs here, with @p executor installed as the
/// resumption context and the stop token set; every later step is resumed
/// through @p executor, on the same strand.
/// @tparam R The handler's result type.
/// @param executor The model's strand executor.
/// @param task     The handler's Task, as the handler call returned it.
/// @param token    The stop token the handler observes.
/// @param done     Called once, on the strand, with the result or the exception.
template <typename R>
void startTaskHandler(const std::shared_ptr<::morph::exec::StrandCoroExecutor>& executor, ::core::async::Task<R> task,
                      ::core::async::StopToken token, std::function<void(std::optional<R>, std::exception_ptr)> done) {
    auto driver = driveTaskHandler<R>(executor, std::move(task), std::move(done));
    driver.handle.promise().token = std::move(token);
    ::morph::async::detail::ScopedResumeContext const context{executor.get()};
    driver.handle.resume();
}

}  // namespace detail

}  // namespace morph::model
