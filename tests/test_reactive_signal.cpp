// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/testing/owner_probe_recorder.hpp>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "test_support.hpp"

namespace {

using morph::reactive::Computed;
using morph::reactive::Effect;
using morph::reactive::EqualityPolicy;
using morph::reactive::kEqualityUsable;
using morph::reactive::Runtime;
using morph::reactive::RuntimeOptions;
using morph::reactive::Signal;
using Owner = morph::testing::StepExecutor;

// No operator==: the equality skip must not apply to it.
struct Opaque {
    int value = 0;
};

// A StepExecutor whose first post throws, as a post that fails to allocate does.
class FailingFirstPost : public morph::exec::IExecutor {
public:
    void post(std::function<void()> task) override {
        if (_failNext) {
            _failNext = false;
            throw std::bad_alloc{};
        }
        _steps.post(std::move(task));
    }

    std::size_t runAll() { return _steps.runAll(); }
    [[nodiscard]] std::size_t pending() const { return _steps.pending(); }

private:
    morph::testing::StepExecutor _steps;
    bool _failNext = true;
};

// A pool runs two posted tasks at once, so it cannot own a graph.
class ParallelExecutor : public morph::exec::IExecutor {
public:
    void post(std::function<void()> task) override { task(); }
    [[nodiscard]] bool isSerial() const noexcept override { return false; }
};

}  // namespace

static_assert(!std::is_copy_constructible_v<Signal<int>> && !std::is_move_constructible_v<Signal<int>>);
static_assert(!std::is_copy_assignable_v<Signal<int>> && !std::is_move_assignable_v<Signal<int>>);
static_assert(!std::is_copy_constructible_v<Effect> && !std::is_move_constructible_v<Effect>);
static_assert(!std::is_copy_constructible_v<Runtime> && !std::is_move_constructible_v<Runtime>);

// The standard containers declare an unconstrained operator==, so std::equality_comparable says yes for a
// container of a type that has none; the gate must look through them. Mutation: make the range
// specialisation of detail::EqualityUsable check std::equality_comparable alone, and these fail to compile.
static_assert(kEqualityUsable<int>);
static_assert(kEqualityUsable<std::string>);
static_assert(kEqualityUsable<std::vector<int>>);
static_assert(kEqualityUsable<std::optional<std::vector<int>>>);
static_assert(kEqualityUsable<std::map<int, std::string>>);
static_assert(kEqualityUsable<std::tuple<int, std::pair<std::string, double>>>);
static_assert(kEqualityUsable<std::variant<int, std::string>>);
static_assert(!kEqualityUsable<Opaque>);
static_assert(!kEqualityUsable<std::vector<Opaque>>);
static_assert(!kEqualityUsable<std::optional<Opaque>>);
static_assert(!kEqualityUsable<std::optional<std::vector<Opaque>>>);
static_assert(!kEqualityUsable<std::vector<std::vector<Opaque>>>);
static_assert(!kEqualityUsable<std::map<int, Opaque>>);
static_assert(!kEqualityUsable<std::pair<int, Opaque>>);
static_assert(!kEqualityUsable<std::tuple<int, std::vector<Opaque>>>);
static_assert(!kEqualityUsable<std::variant<int, Opaque>>);

// Mutation: Signal::set notifies without assigning the value.
TEST_CASE("reactive::Signal: get returns the initial value and set replaces it", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> count{runtime, 1};
    CHECK(count.get() == 1);
    count.set(2);
    CHECK(count.peek() == 2);
}

// Mutation: RuntimeCore::endBatch calls flush() itself after requesting one, so the Effect runs inside set().
TEST_CASE("reactive::Effect: runs on construction, then in a posted flush after a source changes", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> count{runtime, 1};
    std::vector<int> seen;
    Effect const log{runtime, [&] { seen.push_back(count.get()); }};
    CHECK(seen == std::vector{1});

    count.set(2);
    CHECK(seen == std::vector{1});
    owner.runAll();
    CHECK(seen == std::vector{1, 2});
}

// Mutation: RuntimeCore::endBatch requests a flush at every depth, not only the outermost.
TEST_CASE("reactive::Runtime: nested batches are one flush and one run", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> first{runtime, 0};
    Signal<int> second{runtime, 0};
    int runs = 0;
    Effect const sum{runtime, [&] {
                         static_cast<void>(first.get() + second.get());
                         ++runs;
                     }};
    runtime.batch([&] {
        first.set(1);
        second.set(2);
        runtime.batch([&] { first.set(3); });
        CHECK(owner.pending() == 0);
    });
    CHECK(owner.pending() == 1);
    owner.runAll();
    CHECK(runs == 2);
}

// Mutation: drop `_isFlushRequested ||` from RuntimeCore::requestFlush, and three writes post three flushes.
TEST_CASE("reactive::Runtime: exactly one post per idle-to-pending transition", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> value{runtime, 0};
    Effect const watch{runtime, [&] { static_cast<void>(value.get()); }};
    value.set(1);
    value.set(2);
    value.set(3);
    CHECK(owner.pending() == 1);
    CHECK(runtime.isFlushRequested());
    owner.runAll();
    CHECK_FALSE(runtime.isFlushRequested());
    value.set(4);
    CHECK(owner.pending() == 1);
}

// Mutation: drop the early return on an equal value in Signal::set.
TEST_CASE("reactive::Signal: an equal value notifies nobody; a non-comparable one always does", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> comparable{runtime, 1};
    Signal<Opaque> opaque{runtime, Opaque{1}};
    Effect const watch{runtime, [&] {
                           static_cast<void>(comparable.get());
                           static_cast<void>(opaque.get());
                       }};
    comparable.set(1);
    CHECK(owner.pending() == 0);
    opaque.set(Opaque{1});
    CHECK(owner.pending() == 1);
}

// Mutation: drop the equality check in Signal::set, and the comparable writes post a flush.
TEST_CASE("reactive::Signal: a container of a non-comparable type compiles and always notifies", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<std::vector<Opaque>> rows{runtime, {Opaque{1}}};
    Signal<std::optional<std::vector<Opaque>>> maybeRows{runtime, std::nullopt};
    Signal<std::map<int, Opaque>> byId{runtime, {}};
    Signal<std::variant<int, Opaque>> either{runtime, 0};
    Signal<std::vector<int>> numbers{runtime, {1}};
    Signal<std::map<int, std::string>> names{runtime, {{1, "one"}}};
    int runs = 0;
    Effect const watch{runtime, [&] {
                           static_cast<void>(rows.get());
                           static_cast<void>(maybeRows.get());
                           static_cast<void>(byId.get());
                           static_cast<void>(either.get());
                           static_cast<void>(numbers.get());
                           static_cast<void>(names.get());
                           ++runs;
                       }};
    numbers.set({1});
    names.set({{1, "one"}});
    CHECK(owner.pending() == 0);

    rows.set({Opaque{1}});
    owner.runAll();
    CHECK(runs == 2);

    maybeRows.set(std::nullopt);
    owner.runAll();
    CHECK(runs == 3);

    byId.set({});
    owner.runAll();
    CHECK(runs == 4);

    either.set(0);
    owner.runAll();
    CHECK(runs == 5);
}

// Mutation: ignore the policy in Signal::set (compare whenever == is usable).
TEST_CASE("reactive::Signal: EqualityPolicy::Always notifies on an equal write", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> tick{runtime, 1, EqualityPolicy::Always};
    int runs = 0;
    Effect const watch{runtime, [&] {
                           static_cast<void>(tick.get());
                           ++runs;
                       }};
    tick.set(1);
    CHECK(owner.pending() == 1);
    owner.runAll();
    CHECK(runs == 2);
}

// Mutation: drop the notify() in Signal::mutate.
TEST_CASE("reactive::Signal: mutate always notifies", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<std::vector<int>> list{runtime, {}};
    std::size_t lastSize = 99;
    Effect const watch{runtime, [&] { lastSize = list.get().size(); }};
    CHECK(lastSize == 0);
    list.mutate([](std::vector<int>& items) { items.push_back(7); });
    owner.runAll();
    CHECK(lastSize == 1);
}

// Mutation: constrain Effect's constructor on std::copy_constructible<F>: it no longer compiles.
TEST_CASE("reactive::Effect: accepts a move-only body", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> value{runtime, 1};
    int seen = 0;
    Effect const watch{runtime, [owned = std::make_unique<int>(10), &seen, &value] { seen = *owned + value.get(); }};
    CHECK(seen == 11);
    value.set(2);
    owner.runAll();
    CHECK(seen == 12);
}

// Mutation: Node::adoptSources keeps the sources the latest run did not read.
TEST_CASE("reactive::Effect: a source it stops reading no longer triggers it", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<bool> useFirst{runtime, true};
    Signal<int> first{runtime, 0};
    Signal<int> second{runtime, 0};
    int runs = 0;
    Effect const pick{runtime, [&] {
                          ++runs;
                          static_cast<void>(useFirst.get() ? first.get() : second.get());
                      }};
    useFirst.set(false);
    owner.runAll();
    CHECK(runs == 2);

    first.set(1);
    CHECK(owner.pending() == 0);

    second.set(1);
    owner.runAll();
    CHECK(runs == 3);
}

// Mutation: adoptSources instead of mergeSources in Effect::recompute's catch.
TEST_CASE("reactive::Effect: a run that throws keeps every source, old and new", "[reactive]") {
    Owner owner;
    morph::testing::OwnerProbeRecorder const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    Signal<int> value{runtime, 0};
    Signal<int> offset{runtime, 0};
    Signal<int> reason{runtime, 0};
    std::vector<int> seen;
    Effect const watch{runtime, [&] {
                           int const current = value.get();
                           if (current == 1) {
                               static_cast<void>(reason.get());
                               throw std::runtime_error{"effect failed"};
                           }
                           seen.push_back(current + offset.get());
                       }};
    value.set(1);  // the run reads `value` and `reason`, then throws
    owner.runAll();
    CHECK(probe.count(morph::reactive::detail::site::kEffectThrew) == 1);

    offset.set(10);  // old: read by the earlier, successful run only
    CHECK(owner.pending() == 1);
    owner.runAll();
    CHECK(probe.count(morph::reactive::detail::site::kEffectThrew) == 2);

    reason.set(1);  // new: read by the failed runs only
    CHECK(owner.pending() == 1);
    owner.runAll();
    CHECK(probe.count(morph::reactive::detail::site::kEffectThrew) == 3);

    value.set(2);  // read by every run
    CHECK(owner.pending() == 1);
    owner.runAll();
    CHECK(seen == std::vector{0, 12});
}

// Mutation: drop exposeStaleSources() from Node::endPull.
TEST_CASE("reactive::Effect: a run that throws before reading a stale Computed still hears it change later",
          "[reactive]") {
    Owner owner;
    morph::testing::OwnerProbeRecorder const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    Signal<int> gate{runtime, 0};
    Signal<int> base{runtime, 0};
    Computed<int> const derived{runtime, [&] { return base.get(); }};
    bool thrown = false;
    std::vector<int> seen;
    Effect const watch{runtime, [&] {
                           if (gate.get() == 1 && !thrown) {
                               thrown = true;
                               throw std::runtime_error{"effect failed"};
                           }
                           seen.push_back(derived.get());
                       }};
    // `gate` makes the Effect Dirty, so it runs without pulling `derived` first, and throws before
    // reading it: `derived` is left stale under an Effect that ends Clean.
    runtime.batch([&] {
        gate.set(1);
        base.set(5);
    });
    owner.runAll();
    CHECK(probe.count(morph::reactive::detail::site::kEffectThrew) == 1);
    base.set(6);
    CHECK(owner.pending() == 1);
    owner.runAll();
    CHECK(seen == std::vector{0, 6});
}

// Mutation: adopt the run's sources in Effect::recompute even when the frame saw its observer destroyed (crashes).
TEST_CASE("reactive::Effect: an Effect that destroys itself during its run leaves the graph intact", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> value{runtime, 0};
    std::unique_ptr<Effect> self;
    int runs = 0;
    self = std::make_unique<Effect>(runtime, [&] {
        if (value.get() == 1) {
            self.reset();
        }
        ++runs;  // reached through the closure after its Effect is gone
    });
    value.set(1);
    owner.runAll();
    CHECK(self == nullptr);
    CHECK(runs == 2);
    value.set(2);
    CHECK(owner.pending() == 0);
}

// Mutation: merge the run's sources in Effect::recompute's catch even when its observer was destroyed (crashes).
TEST_CASE("reactive::Effect: an Effect that destroys itself and then throws is reported once", "[reactive]") {
    Owner owner;
    morph::testing::OwnerProbeRecorder const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    Signal<int> value{runtime, 0};
    std::unique_ptr<Effect> self;
    self = std::make_unique<Effect>(runtime, [&] {
        if (value.get() == 1) {
            self.reset();
            throw std::runtime_error{"after its own destruction"};
        }
    });
    value.set(1);
    owner.runAll();
    CHECK(self == nullptr);
    CHECK(probe.count(morph::reactive::detail::site::kEffectThrew) == 1);
    value.set(2);
    CHECK(owner.pending() == 0);
}

// Mutation: RuntimeCore::forget leaves a destroyed node in the queue (crashes).
TEST_CASE("reactive::Effect: an Effect that destroys itself and a queued Effect skips the other", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> value{runtime, 0};
    std::unique_ptr<Effect> self;
    std::unique_ptr<Effect> other;
    int otherRuns = 0;
    self = std::make_unique<Effect>(runtime, [&] {
        if (value.get() == 1) {
            other.reset();
            self.reset();
        }
    });
    other = std::make_unique<Effect>(runtime, [&] {
        static_cast<void>(value.get());
        ++otherRuns;
    });
    value.set(1);  // queues self, then other
    owner.runAll();
    CHECK(self == nullptr);
    CHECK(other == nullptr);
    CHECK(otherRuns == 1);
}

// Mutation: skip unlinking from the sources in ~Node: the write reaches the freed Effect (ASan heap-use-after-free).
TEST_CASE("reactive::Effect: destruction unlinks it from its sources", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> value{runtime, 0};
    {
        Effect const transient{runtime, [&] { static_cast<void>(value.get()); }};
    }
    value.set(1);
    CHECK(owner.pending() == 0);
}

// Mutation: skip unlinking from the observers in ~Node: the re-run reads the freed Signal (ASan
// heap-use-after-free).
TEST_CASE("reactive::Signal: destruction unlinks it from its observers", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    auto source = std::make_unique<Signal<int>>(runtime, 0);
    Signal<int> other{runtime, 0};
    int runs = 0;
    Effect const watch{runtime, [&] {
                           ++runs;
                           static_cast<void>(other.get());
                           if (source) {
                               static_cast<void>(source->get());
                           }
                       }};
    source.reset();
    other.set(1);
    owner.runAll();  // the effect re-tracks without the destroyed source; ASan would flag a dangling link
    CHECK(runs == 2);
}

// Mutation: call afterFlush after every Effect the flush runs.
TEST_CASE("reactive::Runtime: afterFlush runs once per flush that processed a queued Effect", "[reactive]") {
    Owner owner;
    int frames = 0;
    Runtime runtime{owner, RuntimeOptions{.afterFlush = [&] { ++frames; }}};
    Signal<int> value{runtime, 0};
    Effect const watch{runtime, [&] { static_cast<void>(value.get()); }};
    value.set(1);
    value.set(2);
    owner.runAll();
    CHECK(frames == 1);
}

// Mutation: reverse the comparison in Node::runsAfter.
TEST_CASE("reactive::Runtime: a flush runs queued Effects oldest first, and one made during it last", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> early{runtime, 0};
    Signal<int> late{runtime, 0};
    Signal<int> go{runtime, 0};
    std::vector<std::string> log;
    std::unique_ptr<Effect> newer;
    Effect const older{runtime, [&] {
                           static_cast<void>(early.get());
                           log.emplace_back("older");
                       }};
    Effect const maker{runtime, [&] {
                           if (go.get() == 0) {
                               return;
                           }
                           newer = std::make_unique<Effect>(runtime, [&] {
                               static_cast<void>(late.get());
                               log.emplace_back("newer");
                           });
                           // Queues the newer Effect before the older one.
                           late.set(1);
                           early.set(1);
                       }};
    log.clear();
    go.set(1);
    owner.runAll();
    // The newer Effect's first run happens in its constructor; queued, it runs after the older one.
    CHECK(log == std::vector<std::string>{"newer", "older", "newer"});
}

// Mutation: RuntimeCore::forget leaves a destroyed node in the queue (crashes); or reverse Node::runsAfter.
TEST_CASE("reactive::Runtime: a queued Effect destroyed during the flush leaves the rest in creation order",
          "[reactive]") {
    // Every pair of a running Effect and a newer queued one it destroys, so the destroyed one sits at every
    // place the queue can hold it.
    constexpr int kEffects = 8;
    int const killer = GENERATE(Catch::Generators::range(0, kEffects - 1));
    int const victim = GENERATE_COPY(Catch::Generators::range(killer + 1, kEffects));
    Owner owner;
    Runtime runtime{owner};
    Signal<int> value{runtime, 0};
    std::vector<int> order;
    std::vector<std::unique_ptr<Effect>> effects;
    effects.reserve(kEffects);
    for (int i = 0; i < kEffects; ++i) {
        effects.push_back(std::make_unique<Effect>(runtime, [&, i] {
            if (value.get() == 0) {
                return;
            }
            order.push_back(i);
            if (i == killer) {
                effects.at(static_cast<std::size_t>(victim)).reset();
            }
        }));
    }
    value.set(1);
    owner.runAll();
    std::vector<int> expected;
    expected.reserve(kEffects);
    for (int i = 0; i < kEffects; ++i) {
        if (i != victim) {
            expected.push_back(i);
        }
    }
    CHECK(order == expected);
}

// Mutation: Runtime::untracked runs the body without its null-observer TrackingFrame.
TEST_CASE("reactive::Runtime: untracked reads do not subscribe", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> value{runtime, 0};
    int runs = 0;
    Effect const watch{runtime, [&] {
                           ++runs;
                           static_cast<void>(runtime.untracked([&] { return value.get(); }));
                       }};
    value.set(1);
    CHECK(owner.pending() == 0);
    CHECK(runs == 1);
}

// Mutation: Node::pull marks the node Clean before recompute() and endPull keeps a colour raised during the run, so
// the Effect's own write queues it again.
TEST_CASE("reactive::Effect: an Effect that writes what it reads does not re-run itself", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> trigger{runtime, 0};
    Signal<int> counter{runtime, 0};
    int runs = 0;
    Effect const bump{runtime, [&] {
                          static_cast<void>(trigger.get());
                          counter.set(counter.get() + 1);
                          ++runs;
                      }};
    trigger.set(1);
    owner.runAll();
    CHECK(runs == 2);
    CHECK(counter.peek() == 2);
    CHECK(owner.pending() == 0);
}

// Mutation: drop exposeStaleSources() from Node::endPull.
TEST_CASE("reactive::Effect: an Effect that writes what it reads through a Computed still hears later writes",
          "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> counter{runtime, 0};
    Computed<int> const viewed{runtime, [&] { return counter.get(); }};
    std::vector<int> seen;
    Effect const bump{runtime, [&] {
                          seen.push_back(viewed.get());
                          if (seen.size() == 1) {
                              counter.set(counter.peek() + 1);
                          }
                      }};
    CHECK(owner.pending() == 0);  // nothing else pulls `viewed`, so its own write has not re-run it yet
    counter.set(10);
    owner.runAll();
    CHECK(seen == std::vector{0, 10});
}

// Mutation: Node::pull marks the node Clean before recompute(), so a direct self-write queues the Effect again.
TEST_CASE("reactive::Effect: writing what it reads is exempt only when read directly", "[reactive]") {
    Owner owner;
    morph::testing::OwnerProbeRecorder const probe{owner.coreExecutor()};
    Runtime runtime{owner, RuntimeOptions{.maxEffectRunsPerFlush = 10}};
    Signal<int> trigger{runtime, 0};
    Signal<int> counter{runtime, 0};
    Computed<int> const viewed{runtime, [&] { return counter.get(); }};
    bool const throughComputed = GENERATE(false, true);
    CAPTURE(throughComputed);
    int runs = 0;
    Effect const bump{runtime, [&] {
                          static_cast<void>(trigger.get());
                          static_cast<void>(throughComputed ? viewed.get() : counter.get());
                          counter.set(counter.peek() + 1);
                          ++runs;
                      }};
    // A second reader of `viewed` pulls it after `bump`'s write, which wakes `bump` again.
    Effect const reader{runtime, [&] { static_cast<void>(viewed.get()); }};
    owner.runAll();
    runs = 0;
    std::size_t const cuts = probe.count(morph::reactive::detail::site::kWriteCycle);
    trigger.set(1);
    owner.runAll();
    if (throughComputed) {
        // A feedback loop: cut at the bound and reported.
        CHECK(runs == 10);
        CHECK(probe.count(morph::reactive::detail::site::kWriteCycle) == cuts + 1);
    } else {
        CHECK(runs == 1);
        CHECK(probe.count(morph::reactive::detail::site::kWriteCycle) == cuts);
    }
    CHECK(owner.pending() == 0);
}

// Mutation: RuntimeCore::forget leaves a destroyed node in the queue (crashes).
TEST_CASE("reactive::Effect: a queued Effect destroyed during the flush is skipped", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> value{runtime, 0};
    std::unique_ptr<Effect> victim;
    int victimRuns = 0;
    Effect const killer{runtime, [&] {
                            if (value.get() == 1) {
                                victim.reset();
                            }
                        }};
    victim = std::make_unique<Effect>(runtime, [&] {
        static_cast<void>(value.get());
        ++victimRuns;
    });
    value.set(1);  // queues killer, then victim
    owner.runAll();
    CHECK(victimRuns == 1);
    CHECK(victim == nullptr);
}

// Mutation: BatchScope's destructor skips endBatch() while an exception unwinds.
TEST_CASE("reactive::Runtime: a throwing batch body still flushes what it wrote", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> value{runtime, 0};
    std::vector<int> seen;
    Effect const watch{runtime, [&] { seen.push_back(value.get()); }};
    CHECK_THROWS_AS(runtime.batch([&] {
        value.set(5);
        throw std::runtime_error{"body failed"};
    }),
                    std::runtime_error);
    CHECK(owner.pending() == 1);
    owner.runAll();
    CHECK(seen == std::vector{0, 5});
    value.set(6);  // the batch depth returned to zero, so this posts again
    CHECK(owner.pending() == 1);
}

// Mutation: leave `_isFlushRequested` set when post() throws, and the second write posts nothing.
TEST_CASE("reactive::Runtime: a post that throws is recovered by the next write", "[reactive]") {
    FailingFirstPost owner;
    Runtime runtime{owner};
    Signal<int> value{runtime, 0};
    std::vector<int> seen;
    Effect const watch{runtime, [&] { seen.push_back(value.get()); }};
    value.set(1);  // the post throws; the write and the queue are kept
    CHECK(value.peek() == 1);
    CHECK_FALSE(runtime.isFlushRequested());
    CHECK(owner.pending() == 0);
    value.set(2);
    CHECK(owner.pending() == 1);
    owner.runAll();
    CHECK(seen == std::vector{0, 2});
}

// A handler that spins a nested event loop (a modal dialog) runs posted tasks inside the widget event. Mutations:
// run the queue in RuntimeCore::flush despite the open widget event (the Effect runs under the handler); re-post
// instead of returning (the nested loop spins and runAll throws past its step bound).
TEST_CASE("reactive::Runtime: a flush due inside a nested widget event waits for the outermost one to end",
          "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> value{runtime, 0};
    std::vector<int> seen;
    Effect const watch{runtime, [&] { seen.push_back(value.get()); }};
    value.set(1);
    REQUIRE(owner.pending() == 1);
    runtime.widgetEvent([&] {
        runtime.widgetEvent([&] {
            owner.runAll();  // the nested loop picks up the posted flush
            CHECK(seen == std::vector{0});
            CHECK(owner.pending() == 0);
            value.set(2);
            CHECK(owner.pending() == 0);
        });
        CHECK(seen == std::vector{0});
        CHECK(owner.pending() == 0);
    });
    CHECK(owner.pending() == 1);
    owner.runAll();
    CHECK(seen == std::vector{0, 2});
}

// Mutation: drop the isSerial() check in Runtime::makeCore.
TEST_CASE("reactive::Runtime: an owner that is not serial is refused", "[reactive]") {
    ParallelExecutor pool;
    CHECK_THROWS_AS(Runtime{pool}, std::invalid_argument);
}

// Mutation: Signal::get tracks the read whatever checkOwner() says.
TEST_CASE("reactive::Signal: a get off the owner is reported and subscribes nothing", "[reactive][misuse]") {
    Owner owner;
    morph::testing::OwnerProbeRecorder const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    Signal<int> foreign{runtime, 7};
    int read = 0;
    int runs = 0;
    Effect const watch{runtime, [&] {
                           ++runs;
                           // Off the owner while this Effect's tracking frame is installed.
                           std::thread{[&] { read = foreign.get(); }}.join();
                       }};
    CHECK(probe.count(morph::reactive::detail::site::kOffOwner) == 1);
    CHECK(read == 7);
    foreign.set(8);
    CHECK(owner.pending() == 0);
    CHECK(runs == 1);
}

// Each off-owner body below runs while the owner thread is blocked in join(), so reading the core's
// state from inside it is ordered by the thread's start and join.

// Mutation: Runtime::batch opens its BatchScope off the owner too.
TEST_CASE("reactive::Runtime: batch off the owner is reported and opens no batch", "[reactive][misuse]") {
    Owner owner;
    morph::testing::OwnerProbeRecorder const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    std::size_t depthInside = 99;
    std::thread{[&] { runtime.batch([&] { depthInside = runtime.core()->batchDepth(); }); }}.join();
    CHECK(probe.count(morph::reactive::detail::site::kOffOwner) == 1);
    CHECK(depthInside == 0);
    CHECK(runtime.core()->batchDepth() == 0);
    CHECK(owner.pending() == 0);
}

// Mutation: Runtime::widgetEvent opens its WidgetEventScope off the owner too.
TEST_CASE("reactive::Runtime: widgetEvent off the owner is reported and marks nothing", "[reactive][misuse]") {
    Owner owner;
    morph::testing::OwnerProbeRecorder const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    bool inWidgetEvent = true;
    std::size_t depthInside = 99;
    std::thread{[&] {
        runtime.widgetEvent([&] {
            inWidgetEvent = runtime.core()->isInWidgetEvent();
            depthInside = runtime.core()->batchDepth();
        });
    }}.join();
    CHECK(probe.count(morph::reactive::detail::site::kOffOwner) == 1);
    CHECK_FALSE(inWidgetEvent);
    CHECK(depthInside == 0);
}

// Mutation: Runtime::untracked installs its frame off the owner too.
TEST_CASE("reactive::Runtime: untracked off the owner is reported and leaves the owner's frame installed",
          "[reactive][misuse]") {
    Owner owner;
    morph::testing::OwnerProbeRecorder const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    morph::reactive::detail::TrackingFrame const* ownerFrame = nullptr;
    morph::reactive::detail::TrackingFrame const* frameInside = nullptr;
    Effect const watch{
        runtime, [&] {
            ownerFrame = runtime.core()->tracking();
            std::thread{[&] { runtime.untracked([&] { frameInside = runtime.core()->tracking(); }); }}.join();
        }};
    CHECK(probe.count(morph::reactive::detail::site::kOffOwner) == 1);
    CHECK(ownerFrame != nullptr);
    CHECK(frameInside == ownerFrame);
}

// Mutation: the Node constructor counts the node whatever checkOwner() says.
TEST_CASE("reactive::Runtime: a node built off the owner is reported and not counted", "[reactive][misuse]") {
    Owner owner;
    morph::testing::OwnerProbeRecorder const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    std::unique_ptr<Signal<int>> stray;
    std::thread{[&] { stray = std::make_unique<Signal<int>>(runtime, 0); }}.join();
    CHECK(probe.count(morph::reactive::detail::site::kOffOwner) == 1);
    CHECK(runtime.core()->liveNodes() == 0);
    stray.reset();
    CHECK(runtime.core()->liveNodes() == 0);
}

// Mutation: drop the checkOwner() call from ~Node.
TEST_CASE("reactive::Runtime: a node destroyed off the owner is reported", "[reactive][misuse]") {
    Owner owner;
    morph::testing::OwnerProbeRecorder const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    auto doomed = std::make_unique<Signal<int>>(runtime, 0);
    // The owner is idle until join() returns, so the destructor's bookkeeping races nothing here.
    std::thread{[&] { doomed.reset(); }}.join();
    CHECK(probe.count(morph::reactive::detail::site::kOffOwner) == 1);
}

// Mutation: drop the untracked TrackingFrame in Runtime::widgetEvent.
TEST_CASE("reactive::Runtime: a widget callback run from inside an Effect subscribes it to nothing", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> items{runtime, 1};
    int runs = 0;
    int seen = 0;
    Effect const binding{runtime, [&] {
                             ++runs;
                             // A renderer whose setter echoes into its own handler.
                             runtime.widgetEvent([&] { seen = items.get(); });
                         }};
    owner.runAll();
    REQUIRE(runs == 1);
    CHECK(seen == 1);

    items.set(5);
    owner.runAll();
    CHECK(runs == 1);
}

// Mutation: drop the ExecutorScope StepExecutor::runOne states around each task.
TEST_CASE("testing::StepExecutor: its own tasks run on it", "[reactive][testing]") {
    Owner owner;
    bool onOwner = false;
    owner.post([&] { onOwner = morph::exec::runningOn(owner); });
    owner.runAll();
    CHECK(onOwner);
    CHECK_FALSE(morph::exec::runningOn(owner));
}
