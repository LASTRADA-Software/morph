// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <core/net/EventLoop.hpp>
#include <functional>
#include <memory>

#include "../attributes.hpp"
#include "../core/executor.hpp"

/// @file
/// @brief `morph::tui::LoopExecutor`: a morph executor whose tasks run in turns of a core-cpp event loop.
///
/// Specified in `docs/spec/tui/frontend.md`, "The loop executor".

namespace morph::tui {

/// @brief Runs every posted task in a later turn of one `core::net::EventLoop`, with this executor current.
///
/// The TUI frontend's reactive `Runtime` is owned by one, and every `BridgeHandler` an application builds receives
/// it as its callback executor, so model replies, timers, sockets and input all run on the thread that turns the
/// loop. `post()` may be called from any thread. A task still queued when the executor is destroyed is dropped,
/// and a task that throws is logged and does not stop the loop.
///
/// Destroy it on the thread that turns the loop, between turns or from inside one of its own tasks, never from
/// another thread: a task already running there would go on using an owner (the reactive `Runtime`) that is gone.
/// Nothing guards against that, because a lock held across a task would deadlock a task that destroys its own
/// executor.
class LoopExecutor final : public exec::IExecutor {
public:
    /// @param loop The loop to post to. Borrowed: it must outlive this executor.
    explicit LoopExecutor(::core::net::EventLoop& loop MORPH_LIFETIMEBOUND);

    /// @brief Destroys the executor; tasks it posted that have not run yet are dropped when the loop reaches them.
    ~LoopExecutor() override;

    LoopExecutor(LoopExecutor const&) = delete;
    LoopExecutor& operator=(LoopExecutor const&) = delete;
    LoopExecutor(LoopExecutor&&) = delete;
    LoopExecutor& operator=(LoopExecutor&&) = delete;

    /// @brief Queues @p task for a later turn of the loop.
    /// @param task What to run; inside it `exec::runningOn(*this)` holds.
    void post(std::function<void()> task) override;

private:
    ::core::net::EventLoop* _loop;
    /// Read by each queued task: once this executor is gone the task is dropped. A box rather than the executor,
    /// because a task outlives the executor that posted it.
    std::shared_ptr<LoopExecutor*> _alive;
};

}  // namespace morph::tui
