// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <functional>
#include <morph/reactive/scheduler.hpp>
#include <morph/reactive/testing/manual_scheduler.hpp>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using morph::reactive::TimerHandle;
using morph::reactive::testing::ManualScheduler;
using namespace std::chrono_literals;

}  // namespace

// ── ManualScheduler and TimerHandle ────────────────────────────────────────

// Mutation: ManualScheduler hands out handles without the isScheduled probe (the fired handle stays active); or
// keeps a fired one-shot scheduled.
TEST_CASE("reactive::ManualScheduler: after fires once at its deadline", "[reactive][scheduler]") {
    ManualScheduler scheduler;
    int fired = 0;
    TimerHandle const handle = scheduler.after(100ms, [&] { ++fired; });
    scheduler.advance(99ms);
    CHECK(fired == 0);
    CHECK(scheduler.pendingTimers() == 1);
    CHECK(handle.active());
    scheduler.advance(1ms);
    CHECK(fired == 1);
    CHECK_FALSE(handle.active());
    scheduler.advance(1000ms);
    CHECK(fired == 1);
    CHECK(scheduler.pendingTimers() == 0);
}

// Mutation: TimerHandle::cancel does not call the cancel function.
TEST_CASE("reactive::ManualScheduler: every repeats until cancelled", "[reactive][scheduler]") {
    ManualScheduler scheduler;
    int fired = 0;
    TimerHandle handle = scheduler.every(10ms, [&] { ++fired; });
    scheduler.advance(35ms);
    CHECK(fired == 3);
    CHECK(scheduler.pendingTimers() == 1);
    handle.cancel();
    CHECK_FALSE(handle.active());
    CHECK(scheduler.pendingTimers() == 0);
    scheduler.advance(100ms);
    CHECK(fired == 3);
}

// Mutation: fireUntil counts a periodic timer's next deadline from the advance's target, not from the deadline it
// fired for.
TEST_CASE("reactive::ManualScheduler: every fires once per elapsed period, as a loop that never blocks",
          "[reactive][scheduler]") {
    ManualScheduler scheduler;
    int fired = 0;
    TimerHandle const handle = scheduler.every(1s, [&] { ++fired; });
    scheduler.advance(10s);
    CHECK(fired == 10);
    scheduler.advance(999ms);
    CHECK(fired == 10);
    scheduler.advance(1ms);
    CHECK(fired == 11);
}

// Mutation: ~TimerHandle does not cancel.
TEST_CASE("reactive::TimerHandle: destruction cancels; moving transfers", "[reactive][scheduler]") {
    ManualScheduler scheduler;
    int fired = 0;
    {
        TimerHandle const transient = scheduler.every(10ms, [&] { ++fired; });
    }
    CHECK(scheduler.pendingTimers() == 0);
    scheduler.advance(50ms);
    CHECK(fired == 0);

    TimerHandle outer;
    CHECK_FALSE(outer.active());
    {
        TimerHandle inner = scheduler.every(10ms, [&] { ++fired; });
        outer = std::move(inner);
        CHECK_FALSE(inner.active());  // NOLINT(bugprone-use-after-move): a moved-from handle owns no timer
    }
    CHECK(outer.active());
    scheduler.advance(10ms);
    CHECK(fired == 1);
}

// Mutation: TimerHandle's move assignment does not cancel the timer it held.
TEST_CASE("reactive::TimerHandle: move-assigning over a live handle cancels its timer", "[reactive][scheduler]") {
    ManualScheduler scheduler;
    int first = 0;
    int second = 0;
    TimerHandle handle = scheduler.every(10ms, [&] { ++first; });
    handle = scheduler.every(10ms, [&] { ++second; });
    CHECK(scheduler.pendingTimers() == 1);
    scheduler.advance(10ms);
    CHECK(first == 0);
    CHECK(second == 1);
}

// Mutation: drop the try/catch in TimerHandle::cancel (std::terminate).
TEST_CASE("reactive::TimerHandle: a cancel function that throws does not escape", "[reactive][scheduler]") {
    int calls = 0;
    TimerHandle handle{[&] {
        ++calls;
        throw std::runtime_error{"broken cancel"};
    }};
    handle.cancel();  // noexcept: an escaping exception would terminate
    CHECK(calls == 1);
    CHECK_FALSE(handle.active());
}

// Mutation: fireUntil takes a periodic timer out while its callback runs and puts it back afterwards, so the
// callback's cancel finds nothing.
TEST_CASE("reactive::ManualScheduler: a callback may cancel itself", "[reactive][scheduler]") {
    ManualScheduler scheduler;
    int fired = 0;
    TimerHandle handle;
    handle = scheduler.every(10ms, [&] {
        ++fired;
        handle.cancel();
    });
    scheduler.advance(10ms);
    scheduler.advance(10ms);
    CHECK(fired == 1);
    CHECK(scheduler.pendingTimers() == 0);
}

// Mutation: fireUntil collects every timer due in the advance first and fires the collected list.
TEST_CASE("reactive::ManualScheduler: a timer cancelled by an earlier callback of the same advance never runs",
          "[reactive][scheduler]") {
    ManualScheduler scheduler;
    int fired = 0;
    TimerHandle victim;
    SECTION("a one-shot") {
        victim = scheduler.after(20ms, [&] { ++fired; });
    }
    SECTION("a periodic timer") {
        victim = scheduler.every(20ms, [&] { ++fired; });
    }
    TimerHandle const canceller = scheduler.after(10ms, [&] { victim.cancel(); });
    scheduler.advance(30ms);  // both are due: the canceller's earlier deadline runs it first
    CHECK(fired == 0);
    CHECK(scheduler.pendingTimers() == 0);
}

// Mutation: the isScheduled probe reports a timer whose scheduler is gone as scheduled; capturing the state strongly
// instead of weakly is observed by ASan.
TEST_CASE("reactive::ManualScheduler: a handle outliving its scheduler is safe", "[reactive][scheduler]") {
    TimerHandle handle;
    {
        ManualScheduler scheduler;
        handle = scheduler.every(10ms, [] {});
    }
    CHECK_FALSE(handle.active());  // the scheduler is gone
    handle.cancel();               // must not touch the destroyed scheduler (ASan observes)
    CHECK_FALSE(handle.active());
}

// Mutation: drop the period check in every().
TEST_CASE("reactive::ManualScheduler: every refuses a non-positive period", "[reactive][scheduler]") {
    ManualScheduler scheduler;
    CHECK_THROWS_AS(static_cast<void>(scheduler.every(0ms, [] {})), std::invalid_argument);
    CHECK_THROWS_AS(static_cast<void>(scheduler.every(-1ms, [] {})), std::invalid_argument);
    CHECK(scheduler.pendingTimers() == 0);
}

// Mutation: drop the delta check in advance().
TEST_CASE("reactive::ManualScheduler: advance refuses a negative delta", "[reactive][scheduler]") {
    ManualScheduler scheduler;
    CHECK_THROWS_AS(scheduler.advance(-1ms), std::invalid_argument);
}

// Mutation: drop the `advancing` check in advance().
TEST_CASE("reactive::ManualScheduler: advance from a timer callback is refused", "[reactive][scheduler]") {
    ManualScheduler scheduler;
    TimerHandle const reentrant = scheduler.after(10ms, [&] { scheduler.advance(1ms); });
    CHECK_THROWS_AS(scheduler.advance(10ms), std::logic_error);
    int fired = 0;
    TimerHandle const later = scheduler.after(5ms, [&] { ++fired; });
    scheduler.advance(5ms);  // the refusal left the scheduler usable
    CHECK(fired == 1);
}

// Mutation: break deadline ties by descending id in fireUntil.
TEST_CASE("reactive::ManualScheduler: due timers fire in deadline order, ties in scheduling order",
          "[reactive][scheduler]") {
    ManualScheduler scheduler;
    std::vector<std::string> fired;
    TimerHandle const late = scheduler.after(25ms, [&] { fired.emplace_back("late@25"); });
    TimerHandle const tick = scheduler.every(10ms, [&] { fired.emplace_back("tick"); });
    TimerHandle const tieFirst = scheduler.after(20ms, [&] { fired.emplace_back("tie-a@20"); });
    TimerHandle const tieSecond = scheduler.after(20ms, [&] { fired.emplace_back("tie-b@20"); });
    SECTION("in one advance") { scheduler.advance(30ms); }
    SECTION("in steps") {
        scheduler.advance(10ms);
        scheduler.advance(10ms);
        scheduler.advance(10ms);
    }
    // `tick` was scheduled before both 20 ms timers and keeps that place, so it fires first at 20 ms.
    CHECK(fired == std::vector<std::string>{"tick", "tick", "tie-a@20", "tie-b@20", "late@25", "tick"});
}

// Mutation: fireUntil does not move the clock to the deadline before firing.
TEST_CASE("reactive::ManualScheduler: a timer scheduled by a callback counts from that callback's deadline",
          "[reactive][scheduler]") {
    ManualScheduler scheduler;
    std::vector<int> fired;
    TimerHandle chained;
    TimerHandle const first = scheduler.after(10ms, [&] {
        fired.push_back(1);
        chained = scheduler.after(15ms, [&] { fired.push_back(2); });
    });
    SECTION("and fires in the same advance when it falls due within it") {
        scheduler.advance(30ms);
        CHECK(fired == std::vector<int>{1, 2});
    }
    SECTION("and waits for a later advance otherwise") {
        scheduler.advance(24ms);
        CHECK(fired == std::vector<int>{1});
        CHECK(scheduler.pendingTimers() == 1);
        scheduler.advance(1ms);
        CHECK(fired == std::vector<int>{1, 2});
    }
    CHECK(scheduler.pendingTimers() == 0);
}

// Mutation: never defer a zero-delay timer made by a callback (the zero-delay section livelocks into its guard).
TEST_CASE("reactive::ManualScheduler: a one-shot re-armed by its own callback", "[reactive][scheduler]") {
    ManualScheduler scheduler;
    int fired = 0;
    TimerHandle handle;
    std::chrono::milliseconds delay{0};
    std::function<void()> rearm = [&] {
        if (++fired > 100) {
            throw std::runtime_error{"livelock: the re-armed timer kept firing inside one advance"};
        }
        handle = scheduler.after(delay, rearm);
    };
    SECTION("with a zero delay fires once per advance") {
        handle = scheduler.after(delay, rearm);
        scheduler.advance(0ms);
        CHECK(fired == 1);
        scheduler.advance(50ms);
        CHECK(fired == 2);
    }
    SECTION("with a positive delay fires once per delay") {
        delay = 10ms;
        handle = scheduler.after(delay, rearm);
        scheduler.advance(35ms);
        CHECK(fired == 3);
    }
    CHECK(scheduler.pendingTimers() == 1);
}

// Mutation: advance() leaves a deferred timer's deadline where it was created.
TEST_CASE("reactive::ManualScheduler: a deferred zero-delay timer fires at the next advance's time, not in the past",
          "[reactive][scheduler]") {
    using std::chrono::milliseconds;
    ManualScheduler scheduler;
    std::vector<milliseconds> stamps;
    TimerHandle deferred;
    TimerHandle chained;
    TimerHandle const first = scheduler.after(10ms, [&] {
        stamps.push_back(scheduler.now());
        deferred = scheduler.after(0ms, [&] {
            stamps.push_back(scheduler.now());
            chained = scheduler.after(15ms, [&] { stamps.push_back(scheduler.now()); });
        });
    });
    scheduler.advance(50ms);
    CHECK(stamps == std::vector<milliseconds>{10ms});
    CHECK(scheduler.now() == 50ms);
    scheduler.advance(0ms);  // the deferred timer fires at 50 ms; what it schedules counts from there
    CHECK(stamps == std::vector<milliseconds>{10ms, 50ms});
    CHECK(scheduler.now() == 50ms);
    scheduler.advance(14ms);
    CHECK(stamps == std::vector<milliseconds>{10ms, 50ms});
    scheduler.advance(1ms);
    CHECK(stamps == std::vector<milliseconds>{10ms, 50ms, 65ms});
    CHECK(scheduler.now() == 65ms);
}

// Mutation: after() keeps a negative delay instead of counting it as zero.
TEST_CASE("reactive::ManualScheduler: a non-positive delay is due at the next advance", "[reactive][scheduler]") {
    ManualScheduler scheduler;
    scheduler.advance(100ms);
    std::vector<int> fired;
    TimerHandle chained;
    TimerHandle const zero = scheduler.after(0ms, [&] { fired.push_back(1); });
    // A negative delay counts as zero, so it ties with `zero` and fires after it, in scheduling order.
    TimerHandle const negative = scheduler.after(-50ms, [&] {
        fired.push_back(2);
        chained = scheduler.after(15ms, [&] { fired.push_back(3); });
    });
    scheduler.advance(0ms);
    CHECK(fired == std::vector<int>{1, 2});
    scheduler.advance(14ms);
    CHECK(fired == std::vector<int>{1, 2});
    scheduler.advance(1ms);
    CHECK(fired == std::vector<int>{1, 2, 3});
}

// Mutation: advance() leaves `advancing` set when a callback throws.
TEST_CASE("reactive::ManualScheduler: a callback that throws leaves the scheduler consistent",
          "[reactive][scheduler]") {
    ManualScheduler scheduler;
    int ticks = 0;
    TimerHandle const boom = scheduler.after(10ms, [] { throw std::runtime_error{"boom"}; });
    TimerHandle const tick = scheduler.every(20ms, [&] { ++ticks; });
    CHECK_THROWS_AS(scheduler.advance(30ms), std::runtime_error);
    CHECK(ticks == 0);
    CHECK(scheduler.pendingTimers() == 1);
    // The clock stopped at the throwing timer's deadline, 10 ms, so this advance reaches 30 ms.
    scheduler.advance(20ms);
    CHECK(ticks == 1);
    scheduler.advance(10ms);
    CHECK(ticks == 2);
}

// ── Query's timed refresh ──────────────────────────────────────────────────
