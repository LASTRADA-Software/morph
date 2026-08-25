// SPDX-License-Identifier: Apache-2.0
//
// Direct unit coverage for morph::async::detail::TimeoutScheduler (the
// threaded build, compiled whenever __EMSCRIPTEN__ without
// __EMSCRIPTEN_PTHREADS__ is not defined -- see timeout_scheduler.hpp's @file
// comment for the single-threaded browser build, which this file's own
// target never compiles and cannot exercise). Bridge::executeVia and
// RemoteServer only ever call schedule()/cancel() with callbacks that don't
// throw, so this file covers the case they don't: a scheduled callback that
// throws is logged and swallowed rather than propagating out of the
// scheduler's background thread.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <morph/core/timeout_scheduler.hpp>
#include <stdexcept>
#include <thread>

using morph::async::detail::TimeoutScheduler;
using namespace std::chrono_literals;

namespace {

// Polls until predicate is true or the deadline elapses, to avoid a
// fixed-sleep race between the test thread and the scheduler's background
// thread invoking the callback.
template <typename Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds timeout = 2s) {
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

}  // namespace

TEST_CASE("TimeoutScheduler: a callback throwing std::exception is logged and swallowed", "[timeout_scheduler]") {
    TimeoutScheduler scheduler;
    std::atomic<bool> fired{false};

    scheduler.schedule(1ms, [&fired] {
        fired = true;
        throw std::runtime_error{"boom"};
    });

    // The scheduler thread must survive the throw: the callback still runs
    // (fired becomes true) and the destructor below still joins cleanly
    // instead of std::terminate-ing on an escaped exception.
    REQUIRE(waitFor([&] { return fired.load(); }));
}

TEST_CASE("TimeoutScheduler: a callback throwing a non-std::exception is logged and swallowed",
          "[timeout_scheduler]") {
    TimeoutScheduler scheduler;
    std::atomic<bool> fired{false};

    scheduler.schedule(1ms, [&fired] {
        fired = true;
        throw 42;  // NOLINT(hicpp-exception-baseclass) -- exercises the catch(...) arm deliberately.
    });

    REQUIRE(waitFor([&] { return fired.load(); }));
}

TEST_CASE("TimeoutScheduler: a callback that throws does not stop later callbacks from firing",
          "[timeout_scheduler]") {
    TimeoutScheduler scheduler;
    std::atomic<bool> secondFired{false};

    scheduler.schedule(1ms, [] { throw std::runtime_error{"first callback throws"}; });
    scheduler.schedule(5ms, [&secondFired] { secondFired = true; });

    REQUIRE(waitFor([&] { return secondFired.load(); }));
}

TEST_CASE("TimeoutScheduler: cancel() on an unknown handle is a no-op", "[timeout_scheduler]") {
    TimeoutScheduler scheduler;
    // Never returned by schedule() on this instance -- exercises the
    // not-found arm of cancel() without racing a real entry.
    scheduler.cancel(TimeoutScheduler::Handle{999999});
    SUCCEED("cancel() on an unknown handle returned without firing or throwing");
}

TEST_CASE("TimeoutScheduler: cancel() before the deadline prevents the callback from firing", "[timeout_scheduler]") {
    TimeoutScheduler scheduler;
    std::atomic<bool> fired{false};

    auto const handle = scheduler.schedule(50ms, [&fired] { fired = true; });
    scheduler.cancel(handle);

    // Give the (cancelled) deadline time to have elapsed, then confirm the
    // callback never ran.
    std::this_thread::sleep_for(80ms);
    REQUIRE_FALSE(fired.load());
}
