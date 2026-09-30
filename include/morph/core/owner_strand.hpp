// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <core/async/ExecutorContext.hpp>
#include <core/async/Strand.hpp>
#include <functional>
#include <type_traits>
#include <utility>

#include "../attributes.hpp"
#include "executor.hpp"
#include "strand.hpp"

/// @file
/// @brief `OwnerStrand`: one core-cpp strand over a morph executor, which is
///        itself a morph executor — the owner a component's state belongs to.
///
/// Specified in `docs/spec/core/executor.md`, "Owner strands".

namespace morph::exec {

/// @brief One strand over a morph executor, usable wherever a morph executor
///        is: the owner a component's state belongs to.
///
/// Tasks run one at a time, in the order they were posted, on the threads of
/// the base executor. A component keeps its state in tasks on one of these
/// and needs no lock for it: `RemoteServer` keeps its registry here, and
/// `ReconnectCoordinator` its reconnect sequence.
///
/// Every task runs inside an `ExecutorScope` naming this object's
/// `coreExecutor()`, so inside a task `runningOn(*this)` is true — as is
/// `runningOn(base)`, whose scope the strand's batch runs within — and a
/// coroutine that suspends in a task and resumes on the current executor comes
/// back here. Outside every task both are false.
///
/// Being a `morph::exec::IExecutor` is what lets a component hand its owner out
/// as a `Completion`'s callback executor, or to a collaborator that must run
/// on the same owner (`SyncWorker`, on the coordinator's strand).
class OwnerStrand final : public ::morph::exec::IExecutor {
public:
    /// @param base Where the strand's tasks run. Borrowed: it must outlive this
    ///        object and keep running tasks until it is closed.
    explicit OwnerStrand(::morph::exec::IExecutor& base MORPH_LIFETIMEBOUND)
        : _strand{base.coreExecutor(),
                  ::core::async::StrandOptions{.aroundTask = ::core::async::AroundTask::of(_hook)}} {}

    OwnerStrand(const OwnerStrand&) = delete;
    OwnerStrand& operator=(const OwnerStrand&) = delete;
    OwnerStrand(OwnerStrand&&) = delete;
    OwnerStrand& operator=(OwnerStrand&&) = delete;

    /// @brief Closes the strand: what is still queued is dropped, and a task
    ///        running on another thread is waited for.
    ~OwnerStrand() override { _strand.close(); }

    /// @brief Queues @p task after everything posted before it. A throw out of
    ///        it is logged. Dropped once the strand is closed.
    /// @param task Callable to run on the strand.
    void post(std::function<void()> task) override {
        _strand.post(detail::LoggedTask<std::function<void()>>{std::move(task)});
    }

    /// @brief Queues @p task, held by value in the strand's one allocation for
    ///        it, after everything posted before it. A throw out of it is
    ///        logged. Dropped once the strand is closed.
    ///
    /// What a component posts to its own strand: no `std::function` in
    /// between, so a move-only capture is fine and a post costs one allocation.
    /// @tparam F The callable's type.
    /// @param task Callable to run on the strand.
    template <typename F>
    void postTask(F&& task) {
        _strand.post(detail::LoggedTask<std::decay_t<F>>{std::forward<F>(task)});
    }

    /// @brief Whether the calling thread is inside one of this strand's tasks.
    /// @return True inside a task, at any depth.
    [[nodiscard]] bool runningHere() const noexcept { return _strand.runningHere(); }

    /// @brief Whether nothing is queued or running.
    /// @return True when idle. Racy by nature where other threads post.
    [[nodiscard]] bool idle() const { return _strand.idle(); }

    /// @brief Closes the strand, as the destructor does. Idempotent.
    void close() { _strand.close(); }

    /// @brief Refuses the strand's try-forms and keeps running what is queued;
    ///        `post` is still admitted until `close()`. Idempotent.
    void seal() { _strand.seal(); }

private:
    /// States this strand's identity around each task it runs.
    struct Hook {
        OwnerStrand* self;
        void operator()(::core::async::RunTask run) const {
            ::core::async::ExecutorScope const scope{self->coreExecutor()};
            run();
        }
    };

    Hook _hook{this};
    /// After the hook, so it is destroyed first: it calls the hook.
    ::core::async::Strand _strand;
};

}  // namespace morph::exec
