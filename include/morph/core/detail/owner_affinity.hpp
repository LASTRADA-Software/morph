// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <core/async/ExecutorContext.hpp>
#include <thread>

#include "../../attributes.hpp"
#include "../executor.hpp"
#include "owner_probe.hpp"

/// @file
/// @brief "Am I on my owner?" for a component that belongs to one executor.
///
/// Specified in `docs/spec/core/executor.md`, "Owner affinity".

namespace morph::exec::detail {

/// @brief The owner a component belongs to, and the check each of its
///        owner-only bodies makes on entry.
///
/// A component is on its owner inside a task the owner runs (`runningOn`),
/// and — when it was constructed on a thread running no executor's task — on
/// that thread. The second half is what a GUI owner needs: its thread runs
/// code outside the owner's tasks too (a Qt slot, a QML handler, a test body,
/// `main()` before its event loop starts), and that code is the owner. A
/// component constructed inside an executor's task — on a pool or a strand —
/// records no thread and is checked by `runningOn` alone. Copying an affinity
/// hands a collaborator (the bridge's backend) the same answer.
class OwnerAffinity {
public:
    /// @param owner The executor the component belongs to. Borrowed: it must
    ///        outlive the component.
    explicit OwnerAffinity(IExecutor& owner MORPH_LIFETIMEBOUND) noexcept
        : _owner{&owner},
          _thread{::core::async::ExecutorScope::innermost() == nullptr ? std::this_thread::get_id()
                                                                       : std::thread::id{}} {}

    /// @brief The owner.
    /// @return The executor passed at construction.
    [[nodiscard]] IExecutor& owner() const noexcept { return *_owner; }

    /// @brief Whether the calling thread is on the owner.
    /// @return True inside a task of the owner, or on the thread that
    ///         constructed this affinity outside every executor's task.
    [[nodiscard]] bool here() const noexcept {
        return runningOn(*_owner) || (_thread != std::thread::id{} && std::this_thread::get_id() == _thread);
    }

    /// @brief Records that @p site is running a body that must run on the owner:
    ///        asserts in a debug build, or reports to an installed probe.
    /// @param site A static string naming the body, e.g. `"Bridge::executeVia"`.
    void note(char const* site) const noexcept { noteOwner(site, _owner->coreExecutor(), here()); }

private:
    IExecutor* _owner;
    std::thread::id _thread;
};

}  // namespace morph::exec::detail
