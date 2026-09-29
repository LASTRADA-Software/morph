// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <atomic>
#include <cassert>
#include <core/async/IExecutor.hpp>

/// @file
/// @brief The check every loop-owned body makes on entry: "am I on my owner?"
///
/// A component whose state belongs to one executor (an `IoLoop`'s loop, for the
/// transports, the timeout scheduler and the network monitor) calls
/// `noteOwner(site, owner)` at the top of each body that touches that state. In
/// a debug build it asserts the calling thread is inside a task of `owner`.
///
/// A test replaces the assertion with an observation: it installs a probe, and
/// every `noteOwner` hands the probe the site and the owner instead of
/// asserting. The probe reads the executor scope itself, so a test proves the
/// body ran posted without trusting the component's own answer, and a test that
/// puts the wrong behaviour back sees a failed check rather than an abort.

namespace morph::exec::detail {

/// @brief What a test installs to observe owner checks: called with the site
///        name and the owner the body expects, on the thread running the body.
using OwnerProbe = void (*)(char const* site, ::core::async::IExecutor const& owner) noexcept;

/// @brief The installed probe, or null.
/// @return The process-wide probe slot.
[[nodiscard]] inline std::atomic<OwnerProbe>& ownerProbe() noexcept {
    static std::atomic<OwnerProbe> probe{nullptr};
    return probe;
}

/// @brief Records that @p site is running a body that must run on @p owner.
///
/// With a probe installed, hands it the site and the owner. Otherwise asserts,
/// in a debug build, that a task of @p owner is running on the calling thread.
/// @param site  A static string naming the body, e.g. `"TimeoutScheduler::schedule"`.
/// @param owner The executor the body's state belongs to.
/// @param onOwner Whether the component considers itself on its owner; what a
///        debug build asserts when no probe is installed.
inline void noteOwner(char const* site, ::core::async::IExecutor const& owner, bool onOwner) noexcept {
    if (OwnerProbe const probe = ownerProbe().load(std::memory_order_acquire); probe != nullptr) {
        probe(site, owner);
        return;
    }
    assert(onOwner && "a loop-owned body ran off its owner's thread");
    static_cast<void>(onOwner);
}

}  // namespace morph::exec::detail
