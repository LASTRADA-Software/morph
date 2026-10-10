// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/testing/owner_probe_recorder.hpp>
#include <stdexcept>
#include <thread>

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

// Mutation: RuntimeCore::flush carries on after a throwing Effect instead of breaking out of the loop.
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

// Two Effects that, once armed, write each other's source and then throw: every flush queues the other one and
// stops on a throw. Mutations: drop the `_throwReposts < maxThrowReposts` bound in RuntimeCore::flush (runAll throws
// past its step bound); drop the reset in RuntimeCore::endBatch (the write after the bound gets one flush, not three).
TEST_CASE("reactive misuse: re-posting after a throwing Effect is bounded, and a write starts a new count",
          "[reactive][misuse]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner, RuntimeOptions{.maxThrowReposts = 2}};
    Signal<int> armed{runtime, 1};
    Signal<int> ping{runtime, 0};
    Signal<int> pong{runtime, 0};
    Effect const first{runtime, [&] {
                           int const seen = ping.get();
                           if (armed.peek() == 1 && seen != 0) {
                               pong.set(seen + 1);
                               throw std::runtime_error{"first"};
                           }
                       }};
    Effect const second{runtime, [&] {
                            int const seen = pong.get();
                            if (armed.peek() == 1 && seen != 0) {
                                ping.set(seen + 1);
                                throw std::runtime_error{"second"};
                            }
                        }};
    ping.set(1);
    owner.runAll();  // the flush and two re-posts, each stopped by a throw
    CHECK(probe.count(site::kEffectThrew) == 3);
    CHECK(owner.pending() == 0);
    CHECK_FALSE(runtime.isFlushRequested());

    ping.set(100);  // a write from outside: a new count
    owner.runAll();
    CHECK(probe.count(site::kEffectThrew) == 6);
    CHECK(owner.pending() == 0);

    armed.set(0);  // nothing reads it tracked, but the write still posts what is queued
    CHECK(owner.pending() == 1);
    owner.runAll();
    CHECK(probe.count(site::kEffectThrew) == 6);
    CHECK(owner.pending() == 0);
}

// Mutation: call afterFlush outside the try/catch in RuntimeCore::flush.
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

// Mutation: drop the report in the Effect constructor's catch.
TEST_CASE("reactive misuse: an Effect that throws on construction is reported", "[reactive][misuse]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    Effect const thrower{runtime, [] { throw std::runtime_error{"boom"}; }};
    CHECK(probe.count(site::kEffectThrew) == 1);
}

// Mutation: disable the countRun bound in RuntimeCore::flush.
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

// Mutation: drop exposeStaleSources() from Node::settle.
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

// Mutation: keep _staleAboveClean set during the walk in Node::markStale (the walk recurses until the stack
// overflows).
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

// Mutation: drop the isComputing() refusal in Signal::writable.
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

// Mutation: Computed::refuseSelfRead lets the read through (the depth guard throws runtime_error instead).
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

// Mutation: Signal::writable ignores what checkOwner() says.
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

// Mutation: drop the kRuntimeOutlived report in ~Runtime; or drop detach(), and the posted flush runs the Effect.
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
