// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <atomic>
#include <core/async/ExecutorContext.hpp>
#include <core/async/IExecutor.hpp>
#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "../core/detail/owner_probe.hpp"

/// @file
/// @brief `morph::testing::OwnerProbeRecorder`: observes the owner checks a test's code makes, instead of
///        letting a debug build assert on them.

namespace morph::testing {

/// @brief Records every owner check a loop-owned body makes while this object is
/// alive (`morph::exec::detail::noteOwner`), and what the calling thread's
/// executor scope said at that moment.
///
/// What a posted-call test reads: a verb called from a thread that is not the
/// loop, whose body then shows up here with `onOwner == true`, was posted. A
/// body run inline on the caller's thread shows up with `onOwner == false`.
/// The scope is read here, by the test, not taken from the component.
class OwnerProbeRecorder {
public:
    /// @brief One owner check.
    struct Seen {
        /// @brief The site the check named.
        std::string site;
        /// @brief Whether the checking thread was inside a task of the owner the check named.
        bool onOwner{false};
        /// @brief Whether the check named the owner this recorder expects.
        bool expectedOwner{false};
    };

    /// @brief Installs this recorder as the process's owner probe.
    /// @param expected The owner every check is expected to name: the
    ///        `IoLoop`'s loop.
    explicit OwnerProbeRecorder(::core::async::IExecutor const& expected) : _expected{&expected} {
        current().store(this);
        ::morph::exec::detail::ownerProbe().store(&OwnerProbeRecorder::record);
    }

    /// @brief Uninstalls the probe, then waits for every thread still inside `record()`.
    ~OwnerProbeRecorder() {
        ::morph::exec::detail::ownerProbe().store(nullptr);
        current().store(nullptr);
        // A thread that read `current()` before it was cleared may still be
        // inside `record()`, about to lock this object's mutex. Its members
        // must outlive it.
        while (inRecord().load() != 0) {
            std::this_thread::yield();
        }
    }

    OwnerProbeRecorder(const OwnerProbeRecorder&) = delete;
    OwnerProbeRecorder& operator=(const OwnerProbeRecorder&) = delete;
    OwnerProbeRecorder(OwnerProbeRecorder&&) = delete;
    OwnerProbeRecorder& operator=(OwnerProbeRecorder&&) = delete;

    /// @brief The checks made at one site.
    /// @param site The site name.
    /// @return Every check made at @p site so far.
    [[nodiscard]] std::vector<Seen> at(std::string_view site) const {
        std::scoped_lock const lock{_mtx};
        std::vector<Seen> matching;
        for (auto const& seen : _seen) {
            if (seen.site == site) {
                matching.push_back(seen);
            }
        }
        return matching;
    }

    /// @brief How many checks were made at one site.
    /// @param site The site name.
    /// @return How many checks were made at @p site.
    [[nodiscard]] std::size_t count(std::string_view site) const { return at(site).size(); }

    /// @brief Whether every check at one site ran posted to the expected owner.
    /// @param site The site name.
    /// @return Whether at least one check was made at @p site, and every one
    ///         of them ran inside a task of the expected owner.
    [[nodiscard]] bool allPosted(std::string_view site) const {
        auto const seen = at(site);
        if (seen.empty()) {
            return false;
        }
        return std::ranges::all_of(seen, [](auto const& one) { return one.onOwner && one.expectedOwner; });
    }

private:
    static std::atomic<OwnerProbeRecorder*>& current() {
        static std::atomic<OwnerProbeRecorder*> recorder{nullptr};
        return recorder;
    }

    /// How many threads are inside `record()`. Counted before `current()` is
    /// read, so the destructor, which clears `current()` and then waits for
    /// this to reach zero, cannot miss a caller that has seen the pointer.
    static std::atomic<std::size_t>& inRecord() {
        static std::atomic<std::size_t> count{0};
        return count;
    }

    static void record(char const* site, ::core::async::IExecutor const& owner) noexcept {
        struct InRecord {
            InRecord() { inRecord().fetch_add(1); }
            ~InRecord() { inRecord().fetch_sub(1); }
            InRecord(const InRecord&) = delete;
            InRecord& operator=(const InRecord&) = delete;
            InRecord(InRecord&&) = delete;
            InRecord& operator=(InRecord&&) = delete;
        } const counted;
        OwnerProbeRecorder* const self = current().load();
        if (self == nullptr) {
            return;
        }
        bool const onOwner = ::core::async::ExecutorScope::anyInForce(
            [&owner](::core::async::ExecutorScope const& scope) { return &scope.executor() == &owner; });
        try {
            std::scoped_lock const lock{self->_mtx};
            self->_seen.push_back(Seen{.site = site, .onOwner = onOwner, .expectedOwner = &owner == self->_expected});
        } catch (...) {  // NOLINT(bugprone-empty-catch)
            // An allocation failure loses one record; the test then sees too
            // few, never a false pass.
        }
    }

    ::core::async::IExecutor const* _expected;
    mutable std::mutex _mtx;
    std::vector<Seen> _seen;
};

}  // namespace morph::testing
