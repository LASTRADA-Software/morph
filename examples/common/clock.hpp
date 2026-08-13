// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/util/datetime.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>

/// @file
/// The ladder-wide injectable "now" (examples/TESTING.md's framework-gaps
/// item 6; examples/LADDER.md framework prerequisite 3). Registry-constructed
/// models are always default-constructed (docs/findings/003,
/// docs/findings/020), so there is no constructor-injection seam for a
/// clock — every rung's time-dependent model logic reads
/// `morph::ladder::now()` instead of `Timestamp::now()`/`DateTime::now()`
/// directly, and a test overrides the process-global provider for the span
/// it needs.

namespace morph::ladder {

namespace detail {

/// @brief Sentinel meaning "disabled, read the real wall clock". Not `-1` (or
///        any other small negative number): `-1` is a valid epoch-ms value
///        for an instant one millisecond before 1970-01-01, so a
///        `ScopedClockOverride` freezing time to a genuine pre-epoch instant
///        would collide with the sentinel and be silently ignored.
///        `INT64_MIN` is an instant roughly 292 million years before the
///        epoch — outside any instant a real `DateTime` in test code will
///        ever hold.
inline constexpr std::int64_t kOverrideDisabled = std::numeric_limits<std::int64_t>::min();

/// @brief Process-global override, in epoch milliseconds; `kOverrideDisabled`
///        means "disabled, read the real wall clock".
[[nodiscard]] inline std::atomic<std::int64_t>& overrideMillisSlot() noexcept {
    static std::atomic<std::int64_t> slot{kOverrideDisabled};
    return slot;
}

}  // namespace detail

/// @brief The ladder's injectable "now".
/// @return The real wall-clock instant, or the frozen instant a live
///         `ScopedClockOverride` installed.
[[nodiscard]] inline ::morph::time::Timestamp now() {
    const std::int64_t overrideMs = detail::overrideMillisSlot().load();
    if (overrideMs == detail::kOverrideDisabled) {
        return ::morph::time::Timestamp::now();
    }
    return ::morph::time::Timestamp{::morph::time::DateTime{
        std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{overrideMs}}}};
}

/// @brief Freezes `morph::ladder::now()` at a fixed instant for the guard's
///        lifetime; restores the previous override (nests correctly) on
///        destruction.
///
/// Cross-thread visible (a `std::atomic`, not `thread_local`): a model under
/// test runs on its own strand/pool thread, not the test thread that
/// constructs this guard.
class ScopedClockOverride {
  public:
    /// @param frozenAt The instant `now()` reads for the guard's lifetime.
    explicit ScopedClockOverride(::morph::time::DateTime frozenAt) noexcept
        : _previous{detail::overrideMillisSlot().exchange(frozenAt.value.time_since_epoch().count())} {}

    ~ScopedClockOverride() { detail::overrideMillisSlot().store(_previous); }

    ScopedClockOverride(const ScopedClockOverride&) = delete;
    ScopedClockOverride& operator=(const ScopedClockOverride&) = delete;
    ScopedClockOverride(ScopedClockOverride&&) = delete;
    ScopedClockOverride& operator=(ScopedClockOverride&&) = delete;

  private:
    std::int64_t _previous;
};

}  // namespace morph::ladder
