// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <atomic>
#include <core/async/ExecutorContext.hpp>
#include <core/async/IExecutor.hpp>
#include <cstddef>
#include <morph/core/detail/owner_probe.hpp>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace morph::testing {

/// Records every owner check a loop-owned body makes while this object is
/// alive (`morph::exec::detail::noteOwner`), and what the calling thread's
/// executor scope said at that moment.
///
/// What a posted-call test reads: a verb called from a thread that is not the
/// loop, whose body then shows up here with `onOwner == true`, was posted. A
/// body run inline on the caller's thread shows up with `onOwner == false`.
/// The scope is read here, by the test, not taken from the component.
class OwnerProbeRecorder {
public:
    /// One owner check.
    struct Seen {
        std::string site;
        bool onOwner{false};
        bool expectedOwner{false};
    };

    /// @param expected The owner every check is expected to name: the
    ///        `IoLoop`'s loop.
    explicit OwnerProbeRecorder(::core::async::IExecutor const& expected) : _expected{&expected} {
        current().store(this);
        ::morph::exec::detail::ownerProbe().store(&OwnerProbeRecorder::record);
    }

    ~OwnerProbeRecorder() {
        ::morph::exec::detail::ownerProbe().store(nullptr);
        current().store(nullptr);
    }

    OwnerProbeRecorder(const OwnerProbeRecorder&) = delete;
    OwnerProbeRecorder& operator=(const OwnerProbeRecorder&) = delete;
    OwnerProbeRecorder(OwnerProbeRecorder&&) = delete;
    OwnerProbeRecorder& operator=(OwnerProbeRecorder&&) = delete;

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

    /// @return How many checks were made at @p site.
    [[nodiscard]] std::size_t count(std::string_view site) const { return at(site).size(); }

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

    static void record(char const* site, ::core::async::IExecutor const& owner) noexcept {
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
