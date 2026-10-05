// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <stdexcept>
#include <thread>

#include "owner_probe_recorder.hpp"
#include "test_support.hpp"

namespace {

using morph::reactive::Computed;
using morph::reactive::Effect;
using morph::reactive::Runtime;
using morph::reactive::RuntimeOptions;
using morph::reactive::Signal;
using Owner = morph::testing::StepExecutor;
using Probe = morph::testing::OwnerProbeRecorder;
namespace site = morph::reactive::detail::site;

}  // namespace

TEST_CASE("reactive misuse: an exception escaping an Effect stops that flush", "[reactive][misuse]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    Signal<int> value{runtime, 0};
    Effect const thrower{runtime, [&] {
                             if (value.get() == 1) {
                                 throw std::runtime_error{"boom"};
                             }
                         }};
    int laterRuns = 0;
    Effect const later{runtime, [&] {
                           static_cast<void>(value.get());
                           ++laterRuns;
                       }};
    value.set(1);
    REQUIRE(owner.runOne());
    CHECK(probe.count(site::kEffectThrew) == 1);
    CHECK(laterRuns == 1);  // still queued: the flush stopped at the thrower
    owner.runAll();         // what was still queued runs in a flush of its own
    CHECK(laterRuns == 2);
    CHECK(probe.count(site::kEffectThrew) == 1);
}

TEST_CASE("reactive misuse: an exception from afterFlush is reported and stays in the flush", "[reactive][misuse]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    int frames = 0;
    Runtime runtime{owner, RuntimeOptions{.afterFlush = [&] {
                        ++frames;
                        throw std::runtime_error{"redraw failed"};
                    }}};
    Signal<int> value{runtime, 0};
    int runs = 0;
    Effect const watch{runtime, [&] {
                           static_cast<void>(value.get());
                           ++runs;
                       }};
    value.set(1);
    CHECK_NOTHROW(owner.runAll());
    CHECK(probe.count(site::kAfterFlushThrew) == 1);
    CHECK_FALSE(runtime.isFlushRequested());

    // The next write flushes as usual.
    value.set(2);
    CHECK_NOTHROW(owner.runAll());
    CHECK(runs == 3);
    CHECK(frames == 2);
    CHECK(probe.count(site::kAfterFlushThrew) == 2);
}

TEST_CASE("reactive misuse: an Effect that throws on construction is reported", "[reactive][misuse]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    Effect const thrower{runtime, [] { throw std::runtime_error{"boom"}; }};
    CHECK(probe.count(site::kEffectThrew) == 1);
}

TEST_CASE("reactive misuse: a write cycle is reported and the flush stops", "[reactive][misuse]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner, RuntimeOptions{.maxEffectRunsPerFlush = 10}};
    Signal<int> ping{runtime, 0};
    Signal<int> pong{runtime, 0};
    // The run counters end the cycle by hand after 1000 runs, so a missing bound shows up as a failed
    // assertion below instead of a flush that never returns.
    int forwardRuns = 0;
    int backwardRuns = 0;
    Effect const forward{runtime, [&] {
                             if (++forwardRuns > 1000) {
                                 return;
                             }
                             pong.set(ping.get() + 1);
                         }};
    Effect const backward{runtime, [&] {
                              if (++backwardRuns > 1000) {
                                  return;
                              }
                              ping.set(pong.get() + 1);
                          }};
    owner.runAll();
    CHECK(probe.count(site::kWriteCycle) == 1);
    // The construction run plus maxEffectRunsPerFlush runs in the flush.
    CHECK(forwardRuns == 11);
    CHECK(backwardRuns == 11);
    CHECK(owner.pending() == 0);
}

namespace {

// How `forward` reads ping in the write-cycle-recovery case below.
enum class Path : std::uint8_t {
    Direct,       // ping itself
    OneComputed,  // Computed(ping)
    TwoComputeds  // Computed(Computed(ping))
};

}  // namespace

TEST_CASE("reactive misuse: after a write cycle is cut, the next write reaches the dropped Effects",
          "[reactive][misuse]") {
    auto const path = GENERATE(Path::Direct, Path::OneComputed, Path::TwoComputeds);
    CAPTURE(path);
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner, RuntimeOptions{.maxEffectRunsPerFlush = 10}};
    Signal<int> ping{runtime, 0};
    Signal<int> pong{runtime, 0};
    Computed<int> const inner{runtime, [&] { return ping.get(); }};
    Computed<int> const outer{runtime, [&] { return inner.get(); }};
    auto const readPing = [&] {
        if (path == Path::Direct) {
            return ping.get();
        }
        return path == Path::OneComputed ? inner.get() : outer.get();
    };
    int forwardRuns = 0;
    int backwardRuns = 0;
    Effect const forward{runtime, [&] {
                             if (++forwardRuns > 1000) {
                                 return;
                             }
                             pong.set(readPing() + 1);
                         }};
    Effect const backward{runtime, [&] {
                              if (++backwardRuns > 1000) {
                                  return;
                              }
                              ping.set(pong.get() + 1);
                          }};
    owner.runAll();
    REQUIRE(probe.count(site::kWriteCycle) == 1);
    REQUIRE(forwardRuns == 11);
    // The cut left a Computed between ping and forward stale; the write must still pass through it.
    ping.set(1000);
    owner.runAll();
    // The write re-enters the cycle, which runs into the bound again.
    CHECK(forwardRuns == 21);
    CHECK(backwardRuns == 21);
    CHECK(probe.count(site::kWriteCycle) == 2);
    CHECK(owner.pending() == 0);
}

TEST_CASE("reactive misuse: a write reaching two Computeds left reading each other terminates", "[reactive][misuse]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    Signal<int> flag{runtime, 0};
    Computed<int> const* xp = nullptr;
    Computed<int> const y{runtime, [&]() -> int { return flag.get() != 0 ? xp->get() : 1; }};
    Computed<int> const x{runtime, [&] { return y.get() + 1; }};
    xp = &x;
    CHECK(x.get() == 2);
    // y now reads x, which reads y: the self-read is refused, and the failed runs keep the links in both
    // directions.
    flag.set(1);
    CHECK_THROWS_AS(y.get(), std::logic_error);
    CHECK(probe.count(site::kComputedReadsItself) > 0);
    int runs = 0;
    Effect const watch{runtime, [&] {
                           ++runs;
                           try {
                               static_cast<void>(x.get());
                           } catch (std::logic_error const&) {  // NOLINT(bugprone-empty-catch)
                               // The cycle is refused on every read; this test is about the write below.
                           }
                           if (runs == 1) {
                               flag.set(2);  // leaves x and y stale under the Effect, both flagged
                           }
                       }};
    owner.runAll();
    flag.set(3);  // the walk meets x and y, each flagged and each the other's observer
    owner.runAll();
    CHECK(runs == 2);
    CHECK(owner.pending() == 0);
}

TEST_CASE("reactive misuse: set() inside a Computed is dropped", "[reactive][misuse]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    Signal<int> base{runtime, 0};
    Signal<int> other{runtime, 0};
    Computed<int> const sneaky{runtime, [&] {
                                   other.set(7);
                                   return base.get();
                               }};
    CHECK(sneaky.get() == 0);
    CHECK(other.peek() == 0);
    CHECK(probe.count(site::kSetInComputed) == 1);
}

TEST_CASE("reactive misuse: a Computed reading itself throws logic_error", "[reactive][misuse]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    std::function<int()> body;
    Computed<int> loop{runtime, [&] { return body(); }};
    // The depth counter turns a missing guard into a wrong exception type instead of a stack overflow.
    int depth = 0;
    body = [&] {
        if (++depth > 64) {
            throw std::runtime_error{"the Computed read itself without being refused"};
        }
        return loop.get() + 1;
    };
    CHECK_THROWS_AS(loop.get(), std::logic_error);
    CHECK(probe.count(site::kComputedReadsItself) == 1);
    CHECK_FALSE(runtime.core()->isComputing());
    CHECK(runtime.core()->tracking() == nullptr);
}

TEST_CASE("reactive misuse: a write from a foreign thread is dropped", "[reactive][misuse]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    Signal<int> value{runtime, 0};
    std::thread{[&] { value.set(1); }}.join();
    CHECK(value.peek() == 0);
    CHECK(owner.pending() == 0);
    CHECK(probe.count(site::kOffOwner) == 1);
}

TEST_CASE("reactive misuse: a Runtime destroyed before its nodes", "[reactive][misuse]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    auto runtime = std::make_unique<Runtime>(owner);
    Signal<int> value{*runtime, 0};
    int runs = 0;
    Effect const watch{*runtime, [&] {
                           static_cast<void>(value.get());
                           ++runs;
                       }};
    value.set(1);
    runtime.reset();
    CHECK(probe.count(site::kRuntimeOutlived) == 1);
    owner.runAll();  // the posted flush finds the core detached
    CHECK(runs == 1);
    value.set(2);  // a write still marks the Effect, but nothing posts a flush any more
    CHECK(owner.pending() == 0);
    CHECK(runs == 1);
}

TEST_CASE("reactive misuse: a flush inside a widget event is re-posted", "[reactive][misuse]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    Signal<int> value{runtime, 0};
    int runs = 0;
    Effect const watch{runtime, [&] {
                           static_cast<void>(value.get());
                           ++runs;
                       }};
    value.set(1);
    runtime.widgetEvent([&] {
        CHECK(owner.runOne());
        // Re-posted at the point of refusal, not only when the widget event's batch ends.
        CHECK(owner.pending() == 1);
    });
    CHECK(probe.count(site::kFlushInWidgetEvent) == 1);
    CHECK(runs == 1);
    CHECK(owner.pending() == 1);
    owner.runAll();
    CHECK(runs == 2);
}
