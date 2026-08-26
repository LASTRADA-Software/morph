// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdlib>
#include <exception>
#include <string>

/// @file
/// The `MORPH_LADDER_DEADLINE_MS` scale factor, on its own and **without
/// Qt** — so a wait loop that cannot include `pump.hpp` can still be scaled
/// by the same knob every other ladder wait obeys.
///
/// @par Why this is not simply part of `pump.hpp`
/// `pump.hpp` is the ladder's only sanctioned wait surface, and every wait it
/// offers is a *Qt event-loop* wait: it includes `<QCoreApplication>` and
/// calls `processEvents`. That include is the whole obstacle. kanban's
/// concurrent-move stress test (`examples/kanban/tests/test_kanban_stress.cpp`)
/// is deliberately Qt-free — morph#128 catalogued 165 ThreadSanitizer warnings
/// that all bottomed out in Qt-internal frames reached through the
/// `QtExecutor` its earlier, `pumpUntil`-driven version pulled in, and a
/// prebuilt Qt cannot be seen through by TSan, making those warnings unusable
/// evidence either way. That file therefore owns a small `waitUntil` of its
/// own over `std::this_thread::sleep_for`, and until this header existed it
/// was the last unscaled wait poll in `examples/`: its budgets were fixed
/// wall-clock constants, so `MORPH_LADDER_DEADLINE_MS` moved every deadline
/// in the ladder except the ones in the slowest test.
///
/// Splitting the scale factor out is what lets both hold: `pump.hpp` includes
/// this header and is otherwise unchanged, so every existing caller keeps the
/// same names in the same namespace, and the Qt-free stress test includes
/// *only* this one and scales its own poll loop with it.

namespace morph::ladder::testkit {

namespace detail {

/// @brief Pure decision logic behind `deadlineScale()`, factored out so it is
///        directly unit-testable: `deadlineScale()` itself reads
///        `MORPH_LADDER_DEADLINE_MS` behind a `static const` guard that runs
///        exactly once per *process*, so no test in the shared
///        `ladder_common_tests` binary can ever be first to observe a
///        particular env value — some earlier test (or `testkit_main.cpp`'s
///        own Qt setup) has always already forced the "unset" path before any
///        test gets to run. Taking the raw env value as a parameter instead
///        of reading it internally sidesteps that entirely: a test calls this
///        with whatever string it likes, no process boundary required.
/// @param envValue `MORPH_LADDER_DEADLINE_MS`'s raw value (as `std::getenv`
///        would return it), or `nullptr` if unset.
/// @return The scale factor, interpreting @p envValue as "use this many ms as
///         the new 5000ms baseline"; `1.0` if unset or unparseable.
[[nodiscard]] inline double computeDeadlineScale(const char* envValue) noexcept {
    if (envValue == nullptr) {
        return 1.0;
    }
    try {
        return std::stod(envValue) / 5000.0;
    } catch (const std::exception&) {
        return 1.0;
    }
}

/// @brief `MORPH_LADDER_DEADLINE_MS`, read once per process — scales every
///        `pumpUntil` default deadline uniformly (slow CI runners, sanitizer
///        builds) without touching call sites. All the interesting logic
///        (unset vs. set, parseable vs. not) lives in `computeDeadlineScale`
///        above; this is a one-line, branch-free delegation.
/// @return The process-wide scale factor.
inline double deadlineScale() {
    static const double scale = computeDeadlineScale(std::getenv("MORPH_LADDER_DEADLINE_MS"));
    return scale;
}

/// @brief Applies `deadlineScale()` to a plain millisecond count.
///
/// The one arithmetic shape both wait loops need, written once rather than
/// twice: `pumpUntil`'s Qt slices and the Qt-free stress-test poll differ in
/// how they *wait*, not in how they scale.
/// @param milliseconds The unscaled budget, in milliseconds.
/// @return @p milliseconds multiplied by `deadlineScale()`, truncated.
[[nodiscard]] inline long long scaledDeadlineMs(long long milliseconds) {
    return static_cast<long long>(static_cast<double>(milliseconds) * deadlineScale());
}

}  // namespace detail

}  // namespace morph::ladder::testkit
