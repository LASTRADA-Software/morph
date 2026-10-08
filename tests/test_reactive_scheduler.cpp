// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/reactive/control.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/scheduler.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/reactive/testing/manual_scheduler.hpp>
#include <morph/testing/owner_probe_recorder.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "reactive_fake_server.hpp"
#include "test_support.hpp"

namespace {

using morph::reactive::Query;
using morph::reactive::QueryOptions;
using morph::reactive::Runtime;
using morph::reactive::Signal;
using morph::reactive::TimerHandle;
using morph::reactive::testing::ManualScheduler;
using Owner = morph::testing::StepExecutor;
using FakeServer = morph::testing::FakeServer<>;
namespace site = morph::reactive::detail::site;
using namespace std::chrono_literals;

struct Lookup {
    int id = 0;
    bool operator==(Lookup const&) const = default;
};

using Detail = Query<Lookup, std::string>;

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

// ── Query's timed refresh ──────────────────────────────────────────────────

// Mutation: Query::issue does not arm the refresh timer.
TEST_CASE("reactive::Query: refreshEvery re-fetches one period after each call is issued",
          "[reactive][scheduler][control]") {
    Owner owner;
    Runtime runtime{owner};
    ManualScheduler scheduler;
    FakeServer server{owner};
    auto query = std::make_unique<Detail>(
        runtime, server.via<Lookup>(), [] { return std::optional{Lookup{1}}; },
        QueryOptions{.scheduler = &scheduler, .refreshEvery = 1000ms});
    REQUIRE(server.calls() == 1);
    server.resolve(0, "one");
    owner.runAll();
    scheduler.advance(999ms);
    CHECK(server.calls() == 1);
    scheduler.advance(1ms);
    CHECK(server.calls() == 2);
    CHECK(server.idOf(1) == 1);
    server.resolve(1, "two");
    owner.runAll();
    scheduler.advance(1000ms);
    CHECK(server.calls() == 3);
    server.resolve(2, "three");
    owner.runAll();
    CHECK(query->value() == std::optional<std::string>{"three"});
    query.reset();
    CHECK(scheduler.pendingTimers() == 0);
    scheduler.advance(5000ms);
    CHECK(server.calls() == 3);
}

// Mutation: Query::armRefresh arms a timer for a zero period too.
TEST_CASE("reactive::Query: without refreshEvery no timer is scheduled", "[reactive][scheduler][control]") {
    Owner owner;
    Runtime runtime{owner};
    ManualScheduler scheduler;
    FakeServer server{owner};
    Detail const query{runtime, server.via<Lookup>(), [] { return std::optional{Lookup{1}}; },
                       QueryOptions{.scheduler = &scheduler}};
    CHECK(scheduler.pendingTimers() == 0);
    scheduler.advance(10s);
    CHECK(server.calls() == 1);
}

// Mutation: drop refreshEvery from the constructor's scheduler check (the first tick dereferences null; crashes).
TEST_CASE("reactive::Query: refreshEvery without a scheduler is refused before anything is fetched",
          "[reactive][scheduler][control]") {
    Owner owner;
    Runtime runtime{owner};
    FakeServer server{owner};
    CHECK_THROWS_AS((Detail{runtime, server.via<Lookup>(), [] { return std::optional{Lookup{1}}; },
                            QueryOptions{.refreshEvery = 1000ms}}),
                    std::invalid_argument);
    CHECK(server.calls() == 0);
}

// Mutation: drop refreshEvery from the constructor's negative check.
TEST_CASE("reactive::Query: a negative refreshEvery is refused before anything is fetched",
          "[reactive][scheduler][control]") {
    Owner owner;
    Runtime runtime{owner};
    ManualScheduler scheduler;
    FakeServer server{owner};
    CHECK_THROWS_AS((Detail{runtime, server.via<Lookup>(), [] { return std::optional{Lookup{1}}; },
                            QueryOptions{.scheduler = &scheduler, .refreshEvery = -1ms}}),
                    std::invalid_argument);
    CHECK(server.calls() == 0);
    CHECK(scheduler.pendingTimers() == 0);
}

// Mutation: the idle branch of Query::issue keeps the refresh timer.
TEST_CASE("reactive::Query: while idle no timer runs and nothing is issued", "[reactive][scheduler][control]") {
    Owner owner;
    Runtime runtime{owner};
    ManualScheduler scheduler;
    FakeServer server{owner};
    Signal<std::optional<int>> selected{runtime, std::nullopt};
    Detail const query{runtime, server.via<Lookup>(),
                       [&]() -> std::optional<Lookup> {
                           if (auto const id = selected.get()) {
                               return Lookup{*id};
                           }
                           return std::nullopt;
                       },
                       QueryOptions{.scheduler = &scheduler, .refreshEvery = 100ms}};
    CHECK(scheduler.pendingTimers() == 0);
    scheduler.advance(300ms);
    CHECK(server.calls() == 0);
    CHECK_FALSE(query.pending());

    selected.set(7);
    owner.runAll();
    REQUIRE(server.calls() == 1);
    server.resolve(0, "seven");
    owner.runAll();
    scheduler.advance(100ms);
    CHECK(server.calls() == 2);
    CHECK(server.idOf(1) == 7);

    selected.set(std::nullopt);
    owner.runAll();
    CHECK(scheduler.pendingTimers() == 0);
    scheduler.advance(500ms);
    CHECK(server.calls() == 2);
}

// Mutation: Query::onRefreshTick issues whatever `pending()` says.
TEST_CASE("reactive::Query: a tick while a call is in flight is skipped and restarts the period",
          "[reactive][scheduler][control]") {
    Owner owner;
    Runtime runtime{owner};
    ManualScheduler scheduler;
    FakeServer server{owner};
    Detail const query{runtime, server.via<Lookup>(), [] { return std::optional{Lookup{1}}; },
                       QueryOptions{.scheduler = &scheduler, .refreshEvery = 100ms}};
    scheduler.advance(100ms);
    CHECK(server.calls() == 1);
    CHECK_FALSE(server.stopRequested(0));
    CHECK(query.pending());

    server.resolve(0, "one");
    owner.runAll();
    CHECK(query.value() == std::optional<std::string>{"one"});
    scheduler.advance(99ms);
    CHECK(server.calls() == 1);
    scheduler.advance(1ms);
    CHECK(server.calls() == 2);
}

// Mutation: Query::onRefreshTick issues whatever `pending()` says.
TEST_CASE("reactive::Query: a fetch slower than the period still delivers", "[reactive][scheduler][control]") {
    Owner owner;
    Runtime runtime{owner};
    ManualScheduler scheduler;
    FakeServer server{owner};
    Detail const query{runtime, server.via<Lookup>(), [] { return std::optional{Lookup{1}}; },
                       QueryOptions{.scheduler = &scheduler, .refreshEvery = 100ms}};
    // Every call replies just after the first tick that follows its issue.
    std::size_t settled = 0;
    for (int tick = 0; tick < 10; ++tick) {
        std::size_t const issuedBefore = server.calls();
        scheduler.advance(100ms);
        for (; settled < issuedBefore; ++settled) {
            server.resolve(settled, "v" + std::to_string(settled));
        }
        owner.runAll();
    }
    CHECK(server.calls() == 6);
    CHECK(query.value() == std::optional<std::string>{"v4"});
    for (std::size_t call = 0; call < server.calls(); ++call) {
        CHECK_FALSE(server.stopRequested(call));
    }
}

// Mutation: Query::onRefreshTick issues whatever `pending()` says.
TEST_CASE("reactive::Query: ticks skipped across a long advance keep one timer and refetch after the reply",
          "[reactive][scheduler][control]") {
    Owner owner;
    Runtime runtime{owner};
    ManualScheduler scheduler;
    FakeServer server{owner};
    Detail const query{runtime, server.via<Lookup>(), [] { return std::optional{Lookup{1}}; },
                       QueryOptions{.scheduler = &scheduler, .refreshEvery = 100ms}};
    scheduler.advance(1000ms);  // ten ticks, each finding the first call in flight
    CHECK(server.calls() == 1);
    CHECK_FALSE(server.stopRequested(0));
    CHECK(scheduler.pendingTimers() == 1);
    server.resolve(0, "one");
    owner.runAll();
    CHECK(query.value() == std::optional<std::string>{"one"});
    scheduler.advance(100ms);
    CHECK(server.calls() == 2);
}

// Mutation: Query::armRefresh keeps a timer that is still scheduled instead of restarting it.
TEST_CASE("reactive::Query: a key change restarts the period", "[reactive][scheduler][control]") {
    Owner owner;
    Runtime runtime{owner};
    ManualScheduler scheduler;
    FakeServer server{owner};
    Signal<int> selected{runtime, 1};
    Detail const query{runtime, server.via<Lookup>(), [&] { return std::optional{Lookup{selected.get()}}; },
                       QueryOptions{.scheduler = &scheduler, .refreshEvery = 1000ms}};
    server.resolve(0, "one");
    owner.runAll();
    scheduler.advance(999ms);
    selected.set(2);
    owner.runAll();
    REQUIRE(server.calls() == 2);
    scheduler.advance(1ms);  // the first period would end here; the key change restarted it
    CHECK(server.calls() == 2);
    CHECK_FALSE(server.stopRequested(1));

    server.resolve(1, "two");
    owner.runAll();
    CHECK(query.value() == std::optional<std::string>{"two"});
    scheduler.advance(998ms);
    CHECK(server.calls() == 2);
    scheduler.advance(1ms);
    CHECK(server.calls() == 3);
    CHECK(server.idOf(2) == 2);
}

// Mutation: Query::armRefresh keeps a timer that is still scheduled instead of restarting it.
TEST_CASE("reactive::Query: refetch() restarts the period", "[reactive][scheduler][control]") {
    Owner owner;
    Runtime runtime{owner};
    ManualScheduler scheduler;
    FakeServer server{owner};
    Detail query{runtime, server.via<Lookup>(), [] { return std::optional{Lookup{1}}; },
                 QueryOptions{.scheduler = &scheduler, .refreshEvery = 1000ms}};
    server.resolve(0, "one");
    owner.runAll();
    scheduler.advance(500ms);
    query.refetch();
    REQUIRE(server.calls() == 2);
    server.resolve(1, "two");
    owner.runAll();
    scheduler.advance(999ms);
    CHECK(server.calls() == 2);
    scheduler.advance(1ms);
    CHECK(server.calls() == 3);
}

// Mutation: Query::issue arms the refresh only after the fetcher returned.
TEST_CASE("reactive::Query: a fetcher that throws is retried one period later", "[reactive][scheduler][control]") {
    Owner owner;
    Runtime runtime{owner};
    ManualScheduler scheduler;
    FakeServer server{owner};
    int attempts = 0;
    auto const fetch = [&](Lookup const& action) {
        if (++attempts == 1) {
            throw std::runtime_error{"down"};
        }
        return server.fetch(action);
    };
    Detail const query{runtime, fetch, [] { return std::optional{Lookup{1}}; },
                       QueryOptions{.scheduler = &scheduler, .refreshEvery = 100ms}};
    CHECK(query.error() != nullptr);
    CHECK_FALSE(query.pending());
    scheduler.advance(100ms);
    CHECK(attempts == 2);
    CHECK(server.calls() == 1);
}

// Mutation: Query::issue carries on after the fetcher destroyed the Query (heap corruption, aborts).
TEST_CASE("reactive::Query: a timed refresh whose fetcher destroys the Query leaves it alone afterwards",
          "[reactive][scheduler][control][lifetime]") {
    Owner owner;
    Runtime runtime{owner};
    ManualScheduler scheduler;
    FakeServer server{owner};
    std::unique_ptr<Detail> query;
    auto const fetch = [&](Lookup const& action) {
        Lookup const copy = action;  // the action belongs to the Query: read it before destroying the Query
        if (server.calls() == 1) {
            query.reset();
        }
        return server.fetch(copy);
    };
    query = std::make_unique<Detail>(
        runtime, fetch, [] { return std::optional{Lookup{1}}; },
        QueryOptions{.scheduler = &scheduler, .refreshEvery = 100ms});
    REQUIRE(server.calls() == 1);
    server.resolve(0, "one");
    owner.runAll();
    scheduler.advance(100ms);  // ASan observes any touch of the destroyed Query
    CHECK(query == nullptr);
    CHECK(server.calls() == 2);
    CHECK(scheduler.pendingTimers() == 0);
    server.resolve(1, "late");
    owner.runAll();
}

namespace {

// Breaks the Scheduler contract on purpose: cancelling does nothing, so a callback can run after its handle is
// gone.
class LeakyScheduler final : public morph::reactive::Scheduler {
public:
    [[nodiscard]] TimerHandle after(std::chrono::milliseconds /*delay*/, std::function<void()> fn) override {
        _callbacks.push_back(std::move(fn));
        return TimerHandle{};
    }

    [[nodiscard]] TimerHandle every(std::chrono::milliseconds /*period*/, std::function<void()> fn) override {
        _callbacks.push_back(std::move(fn));
        return TimerHandle{};
    }

    void fireAll() {
        auto const callbacks = std::exchange(_callbacks, {});
        for (auto const& callback : callbacks) {
            callback();
        }
    }

private:
    std::vector<std::function<void()>> _callbacks;
};

}  // namespace

// Mutation: Query::armRefresh schedules the tick without `_lifetime.guard`.
TEST_CASE("reactive::Query: a tick that arrives after the Query is gone is dropped",
          "[reactive][scheduler][control][lifetime]") {
    Owner owner;
    Runtime runtime{owner};
    LeakyScheduler scheduler;
    FakeServer server{owner};
    {
        Detail const query{runtime, server.via<Lookup>(), [] { return std::optional{Lookup{1}}; },
                           QueryOptions{.scheduler = &scheduler, .refreshEvery = 100ms}};
        server.resolve(0, "one");
        owner.runAll();
    }
    scheduler.fireAll();  // ASan observes any touch of the destroyed Query
    CHECK(server.calls() == 1);
}

// Mutation: Query::onRefreshTick ignores checkOwner().
TEST_CASE("reactive::Query: a timed refresh off the owner is reported and refused",
          "[reactive][scheduler][control][misuse]") {
    Owner owner;
    morph::testing::OwnerProbeRecorder const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    ManualScheduler scheduler;
    FakeServer server{owner};
    Detail const query{runtime, server.via<Lookup>(), [] { return std::optional{Lookup{1}}; },
                       QueryOptions{.scheduler = &scheduler, .refreshEvery = 100ms}};
    server.resolve(0, "one");
    owner.runAll();
    std::thread{[&] { scheduler.advance(100ms); }}.join();
    CHECK(probe.count(site::kOffOwner) == 1);
    CHECK(server.calls() == 1);
    CHECK(query.value() == std::optional<std::string>{"one"});
}

// ── Over a real LocalBackend ───────────────────────────────────────────────

// Model, action and result types need external linkage: the BRIDGE_REGISTER_* macros specialise templates at
// global scope.
// NOLINTBEGIN(misc-use-internal-linkage)
struct ScheduledReadHits {};
struct ScheduledBump {};

struct ScheduledHitModel {
    std::int64_t hits = 0;

    [[nodiscard]] std::int64_t execute(ScheduledReadHits const& /*action*/) const { return hits; }

    std::int64_t execute(ScheduledBump const& /*action*/) { return ++hits; }
};

BRIDGE_REGISTER_MODEL(ScheduledHitModel, "Test_ScheduledHitModel")
BRIDGE_REGISTER_ACTION(ScheduledHitModel, ScheduledReadHits, "Test_ScheduledReadHits")
BRIDGE_REGISTER_ACTION(ScheduledHitModel, ScheduledBump, "Test_ScheduledBump")
// NOLINTEND(misc-use-internal-linkage)

namespace {

struct ScheduledWiring {
    morph::exec::ThreadPoolExecutor pool{1};
    morph::exec::MainThreadExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    morph::bridge::BridgeHandler<ScheduledHitModel> handler{bridge, &owner};
};

}  // namespace

// Mutation: Query::issue does not arm the refresh timer.
TEST_CASE("reactive::Query: a bridge query refreshes on its period", "[reactive][scheduler][control]") {
    ScheduledWiring wiring;
    Runtime runtime{wiring.owner};
    ManualScheduler scheduler;
    Query<ScheduledReadHits> const hits{runtime, wiring.handler, [] { return std::optional{ScheduledReadHits{}}; },
                                        QueryOptions{.scheduler = &scheduler, .refreshEvery = 1000ms}};
    REQUIRE(
        morph::testing::pumpOwnerUntil(wiring.owner, [&] { return hits.value() == std::optional<std::int64_t>{0}; }));

    CHECK(morph::testing::awaitValueOn(wiring.owner, wiring.handler.execute(ScheduledBump{})) == 1);
    CHECK(hits.value() == std::optional<std::int64_t>{0});  // nothing told the query

    scheduler.advance(1000ms);
    REQUIRE(
        morph::testing::pumpOwnerUntil(wiring.owner, [&] { return hits.value() == std::optional<std::int64_t>{1}; }));
    CHECK_FALSE(hits.pending());
}

// Mutation: drop refreshEvery from the constructor's scheduler check (crashes).
TEST_CASE("reactive::Query: a bridge query refuses refreshEvery without a scheduler",
          "[reactive][scheduler][control]") {
    ScheduledWiring wiring;
    Runtime runtime{wiring.owner};
    CHECK_THROWS_AS(
        (Query<ScheduledReadHits>{runtime, wiring.handler, [] { return std::optional{ScheduledReadHits{}}; },
                                  QueryOptions{.refreshEvery = 1000ms}}),
        std::invalid_argument);
}

// ── Query's debounce ───────────────────────────────────────────────────────

// Mutation: Query::onKey fetches a changed key at once, whatever the debounce says.
TEST_CASE("reactive::Query: debounce fetches a changed key once it has been stable that long",
          "[reactive][scheduler][control]") {
    Owner owner;
    Runtime runtime{owner};
    ManualScheduler scheduler;
    FakeServer server{owner};
    Signal<int> id{runtime, 1};
    Detail const detail{runtime, server.via<Lookup>(), [&] { return std::optional{Lookup{id.get()}}; },
                        QueryOptions{.scheduler = &scheduler, .debounce = 300ms}};
    REQUIRE(server.calls() == 1);  // the first key is fetched at once
    server.resolve(0, "one");
    owner.runAll();

    id.set(2);
    owner.runAll();
    CHECK(detail.pending());
    CHECK(detail.value() == std::optional<std::string>{"one"});  // kept while the query waits
    scheduler.advance(200ms);
    id.set(3);  // restarts the wait
    owner.runAll();
    scheduler.advance(200ms);
    CHECK(server.calls() == 1);  // 3 has been stable for only 200 ms
    scheduler.advance(100ms);
    REQUIRE(server.calls() == 2);  // 2 was never fetched
    CHECK(server.idOf(1) == 3);
    server.resolve(1, "three");
    owner.runAll();
    CHECK_FALSE(detail.pending());
    CHECK(detail.value() == std::optional<std::string>{"three"});
}

// Mutation: drop the `_inflight.reset()` from the debounce branch of Query::onKey; the reply for the replaced key
// lands while the query waits.
TEST_CASE("reactive::Query: debounce drops the reply for the key it replaces at once",
          "[reactive][scheduler][control]") {
    Owner owner;
    Runtime runtime{owner};
    ManualScheduler scheduler;
    FakeServer server{owner};
    Signal<int> id{runtime, 1};
    Detail const detail{runtime, server.via<Lookup>(), [&] { return std::optional{Lookup{id.get()}}; },
                        QueryOptions{.scheduler = &scheduler, .debounce = 300ms}};
    id.set(2);
    owner.runAll();
    CHECK(server.stopRequested(0));
    server.resolve(0, "one");  // the reply for the key being replaced
    owner.runAll();
    CHECK_FALSE(detail.value().has_value());
    CHECK(detail.pending());
}

// Mutations: debounce a change to idle (the query stays pending and keeps the old value until the timer fires);
// Query::issue does not cancel the wait (refetch() and the timer both fetch).
TEST_CASE("reactive::Query: a change to idle and refetch() end a debounce wait at once",
          "[reactive][scheduler][control]") {
    Owner owner;
    Runtime runtime{owner};
    ManualScheduler scheduler;
    FakeServer server{owner};
    Signal<std::optional<int>> id{runtime, 1};
    Detail detail{runtime, server.via<Lookup>(),
                  [&]() -> std::optional<Lookup> {
                      if (auto const current = id.get()) {
                          return Lookup{*current};
                      }
                      return std::nullopt;
                  },
                  QueryOptions{.scheduler = &scheduler, .debounce = 300ms}};
    server.resolve(0, "one");
    owner.runAll();

    id.set(std::nullopt);
    owner.runAll();
    CHECK_FALSE(detail.pending());
    CHECK_FALSE(detail.value().has_value());
    CHECK(scheduler.pendingTimers() == 0);

    id.set(2);  // from idle: the first key after idle waits like any other change
    owner.runAll();
    CHECK(detail.pending());
    detail.refetch();  // fetches now and ends the wait
    CHECK(server.calls() == 2);
    CHECK(scheduler.pendingTimers() == 0);
    scheduler.advance(300ms);
    CHECK(server.calls() == 2);
}

// Mutation: drop the debounce from the constructor's checks; the query is built and the wait dereferences a null
// scheduler (crashes).
TEST_CASE("reactive::Query: debounce without a scheduler, or negative, is refused before anything is fetched",
          "[reactive][scheduler][control]") {
    Owner owner;
    Runtime runtime{owner};
    FakeServer server{owner};
    CHECK_THROWS_AS((Detail{runtime, server.via<Lookup>(), [] { return std::optional{Lookup{1}}; },
                            QueryOptions{.debounce = 300ms}}),
                    std::invalid_argument);
    ManualScheduler scheduler;
    CHECK_THROWS_AS((Detail{runtime, server.via<Lookup>(), [] { return std::optional{Lookup{1}}; },
                            QueryOptions{.scheduler = &scheduler, .debounce = -1ms}}),
                    std::invalid_argument);
    CHECK(server.calls() == 0);
}

// Mutation: Query::onKey leaks the debounce handle (`new TimerHandle{...}`) instead of keeping it in `_debounce`, so
// destroying the query leaves the wait scheduled.
TEST_CASE("reactive::Query: destroying a debouncing query cancels the wait", "[reactive][scheduler][control]") {
    Owner owner;
    Runtime runtime{owner};
    ManualScheduler scheduler;
    FakeServer server{owner};
    Signal<int> id{runtime, 1};
    auto detail = std::make_unique<Detail>(
        runtime, server.via<Lookup>(), [&] { return std::optional{Lookup{id.get()}}; },
        QueryOptions{.scheduler = &scheduler, .debounce = 300ms});
    id.set(2);
    owner.runAll();
    REQUIRE(scheduler.pendingTimers() == 1);
    detail.reset();
    CHECK(scheduler.pendingTimers() == 0);
    scheduler.advance(300ms);
    CHECK(server.calls() == 1);
}
