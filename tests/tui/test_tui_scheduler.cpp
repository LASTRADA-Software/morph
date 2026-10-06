// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/core/io_loop.hpp>
#include <morph/reactive/scheduler.hpp>
#include <morph/tui/loop_executor.hpp>
#include <stdexcept>

#include "tui/scheduler.hpp"

using morph::reactive::TimerHandle;
using morph::tui::LoopExecutor;
using morph::tui::detail::LoopScheduler;
using namespace std::chrono_literals;

namespace {

/// A caller-driven loop, its executor, and a scheduler on both.
struct Loop {
    morph::exec::IoLoop io{morph::exec::IoLoopDriver::Caller};
    LoopExecutor executor{io.loop()};
    LoopScheduler scheduler{io.loop(), executor};

    /// Turns the loop on this thread for @p duration.
    void turnFor(std::chrono::milliseconds duration) {
        auto const deadline = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < deadline) {
            static_cast<void>(io.loop().runOnce(2ms));
        }
    }

    /// Turns the loop until @p done holds or two seconds pass.
    template <class Pred>
    bool turnUntil(Pred done) {
        auto const deadline = std::chrono::steady_clock::now() + 2s;
        while (!done()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            static_cast<void>(io.loop().runOnce(2ms));
        }
        return true;
    }
};

/// A loop whose time moves only when the case advances its clock, so which turn finds a timer due is exact.
struct ManualLoop {
    core::platform::ManualClock clock;
    core::net::testing::TestLoop loop{clock};
    LoopExecutor executor{loop};
    LoopScheduler scheduler{loop, executor};
};

}  // namespace

TEST_CASE("tui::LoopScheduler: after fires once, in a loop turn, with the owner current", "[tui][scheduler]") {
    Loop loop;
    int fired = 0;
    bool onOwner = false;
    TimerHandle const handle = loop.scheduler.after(5ms, [&] {
        ++fired;
        onOwner = morph::exec::runningOn(loop.executor);
    });
    REQUIRE(loop.turnUntil([&] { return fired == 1; }));
    loop.turnFor(20ms);
    CHECK(fired == 1);
    CHECK(onOwner);
    CHECK(loop.scheduler.pendingTimers() == 0);
}

TEST_CASE("tui::LoopScheduler: every repeats until its handle is cancelled", "[tui][scheduler]") {
    Loop loop;
    int fired = 0;
    TimerHandle handle = loop.scheduler.every(3ms, [&] { ++fired; });
    REQUIRE(loop.turnUntil([&] { return fired >= 3; }));
    handle.cancel();
    int const atCancel = fired;
    loop.turnFor(20ms);
    CHECK(fired == atCancel);
    CHECK(loop.scheduler.pendingTimers() == 0);
}

TEST_CASE("tui::LoopScheduler: destroying the handle cancels the timer", "[tui][scheduler]") {
    Loop loop;
    int fired = 0;
    {
        TimerHandle const transient = loop.scheduler.after(2ms, [&] { ++fired; });
    }
    loop.turnFor(20ms);
    CHECK(fired == 0);
    CHECK(loop.scheduler.pendingTimers() == 0);
}

TEST_CASE("tui::LoopScheduler: a callback may cancel its own timer", "[tui][scheduler]") {
    Loop loop;
    int fired = 0;
    TimerHandle handle;
    handle = loop.scheduler.every(2ms, [&] {
        ++fired;
        handle.cancel();
    });
    loop.turnFor(30ms);
    CHECK(fired == 1);
}

TEST_CASE("tui::LoopScheduler: a handle outliving its scheduler is safe", "[tui][scheduler]") {
    TimerHandle handle;
    {
        Loop loop;
        handle = loop.scheduler.every(10ms, [] {});
    }
    handle.cancel();
    CHECK_FALSE(handle.active());
}

TEST_CASE("tui::LoopScheduler: every refuses a non-positive period", "[tui][scheduler]") {
    Loop loop;
    CHECK_THROWS_AS(static_cast<void>(loop.scheduler.every(0ms, [] {})), std::invalid_argument);
}

TEST_CASE("tui::LoopScheduler: a callback cancelling a sibling due in the same turn stops it", "[tui][scheduler]") {
    ManualLoop manual;
    int fired = 0;
    TimerHandle once;
    TimerHandle repeating;
    // Each cancels the other, so exactly one may run whichever the loop runs first.
    once = manual.scheduler.after(5ms, [&] {
        ++fired;
        repeating.cancel();
    });
    repeating = manual.scheduler.every(5ms, [&] {
        ++fired;
        once.cancel();
    });
    manual.clock.advance(5ms);
    // This turn finds both due and queues them; the next turn runs them.
    static_cast<void>(manual.loop.tick());
    REQUIRE(fired == 0);
    REQUIRE(manual.loop.readyCount() == 2);
    static_cast<void>(manual.loop.drain());
    CHECK(fired == 1);
    manual.clock.advance(20ms);
    static_cast<void>(manual.loop.drain());
    CHECK(fired == 1);
    CHECK(manual.scheduler.pendingTimers() == 0);
}

TEST_CASE("tui::LoopScheduler: a blocked owner fires every once, next deadline from that firing", "[tui][scheduler]") {
    ManualLoop manual;
    int fired = 0;
    TimerHandle const handle = manual.scheduler.every(5ms, [&] { ++fired; });
    manual.clock.advance(32ms);  // the owner is blocked past six periods
    static_cast<void>(manual.loop.drain());
    CHECK(fired == 1);
    manual.clock.advance(4ms);  // 36ms: the next deadline is one period after the firing at 32ms
    static_cast<void>(manual.loop.drain());
    CHECK(fired == 1);
    manual.clock.advance(1ms);
    static_cast<void>(manual.loop.drain());
    CHECK(fired == 2);
    CHECK(manual.scheduler.pendingTimers() == 1);
}

TEST_CASE("tui::LoopScheduler: a callback may destroy the scheduler and its own handle", "[tui][scheduler]") {
    core::platform::ManualClock clock;
    core::net::testing::TestLoop loop{clock};
    LoopExecutor executor{loop};
    auto scheduler = std::make_unique<LoopScheduler>(loop, executor);
    int fired = 0;
    auto own = std::make_unique<TimerHandle>();
    // Armed first, so it runs first; the sibling is queued in the same turn and must not run after the
    // scheduler is gone.
    *own = scheduler->every(5ms, [&] {
        ++fired;
        scheduler.reset();
        own.reset();
    });
    TimerHandle sibling = scheduler->every(5ms, [&] { ++fired; });
    clock.advance(5ms);
    static_cast<void>(loop.drain());
    CHECK(fired == 1);
    CHECK(scheduler == nullptr);
    CHECK(own == nullptr);
    sibling.cancel();
    CHECK_FALSE(sibling.active());
    clock.advance(20ms);
    static_cast<void>(loop.drain());
    CHECK(fired == 1);
}
