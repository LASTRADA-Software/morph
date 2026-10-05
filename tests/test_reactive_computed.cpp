// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "owner_probe_recorder.hpp"
#include "test_support.hpp"

namespace {

using morph::reactive::Computed;
using morph::reactive::Effect;
using morph::reactive::Runtime;
using morph::reactive::Signal;
using Owner = morph::testing::StepExecutor;
using Probe = morph::testing::OwnerProbeRecorder;
namespace site = morph::reactive::detail::site;

class NoDefault {
public:
    explicit NoDefault(int value) : _value{value} {}
    [[nodiscard]] int value() const { return _value; }
    bool operator==(NoDefault const&) const = default;

private:
    int _value;
};

// No operator==: the equality gate must not apply to it.
struct Opaque {
    int value = 0;
};

// 100 / divisor, failing with std::domain_error when the divisor is zero.
int divide(Signal<int> const& divisor) {
    if (divisor.get() == 0) {
        throw std::domain_error{"division by zero"};
    }
    return 100 / divisor.get();
}

}  // namespace

static_assert(!std::is_copy_constructible_v<Computed<int>> && !std::is_move_constructible_v<Computed<int>>);
static_assert(!std::is_copy_assignable_v<Computed<int>> && !std::is_move_assignable_v<Computed<int>>);

TEST_CASE("reactive::Computed: lazy, cached, recomputed only after a source changes", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> base{runtime, 2};
    int calls = 0;
    Computed<int> const twice{runtime, [&] {
                                  ++calls;
                                  return base.get() * 2;
                              }};
    CHECK(calls == 0);
    CHECK(twice.get() == 4);
    CHECK(twice.get() == 4);
    CHECK(calls == 1);
    base.set(3);
    CHECK(twice.get() == 6);
    CHECK(calls == 2);
}

TEST_CASE("reactive::Computed: a diamond is seen consistently, once per change", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> base{runtime, 1};
    Computed<int> const twice{runtime, [&] { return base.get() * 2; }};
    Computed<int> const thrice{runtime, [&] { return base.get() * 3; }};
    Computed<int> const sum{runtime, [&] { return twice.get() + thrice.get(); }};
    std::vector<int> seen;
    Effect const log{runtime, [&] { seen.push_back(sum.get()); }};
    base.set(2);
    owner.runAll();
    CHECK(seen == std::vector{5, 10});
}

TEST_CASE("reactive::Computed: an Effect reading A before Computed(A) never sees them disagree", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> base{runtime, 1};
    Computed<int> const twice{runtime, [&] { return base.get() * 2; }};
    std::vector<std::pair<int, int>> seen;
    Effect const log{runtime, [&] {
                         int const raw = base.get();
                         seen.emplace_back(raw, twice.get());
                     }};
    base.set(5);
    owner.runAll();
    REQUIRE(seen.size() == 2);
    for (auto const& [raw, doubled] : seen) {
        CHECK(doubled == raw * 2);
    }
}

TEST_CASE("reactive::Computed: an unchanged result stops propagation", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> base{runtime, 1};
    Computed<bool> const positive{runtime, [&] { return base.get() > 0; }};
    int runs = 0;
    Effect const watch{runtime, [&] {
                           ++runs;
                           static_cast<void>(positive.get());
                       }};
    base.set(2);
    owner.runAll();
    CHECK(runs == 1);
    base.set(-1);
    owner.runAll();
    CHECK(runs == 2);
}

TEST_CASE("reactive::Computed: a non-comparable result always propagates", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> base{runtime, 1};
    Computed<Opaque> const wrapped{runtime, [&] { return Opaque{base.get() > 0 ? 1 : 0}; }};
    int runs = 0;
    Effect const watch{runtime, [&] {
                           ++runs;
                           static_cast<void>(wrapped.get());
                       }};
    base.set(2);
    owner.runAll();
    CHECK(runs == 2);
}

TEST_CASE("reactive::Computed: a container of a non-comparable type always propagates", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> base{runtime, 1};
    Computed<std::vector<Opaque>> const rows{runtime, [&] { return std::vector{Opaque{base.get() > 0 ? 1 : 0}}; }};
    int runs = 0;
    Effect const watch{runtime, [&] {
                           ++runs;
                           static_cast<void>(rows.get());
                       }};
    base.set(2);
    owner.runAll();
    CHECK(runs == 2);
}

TEST_CASE("reactive::Computed: T need not be default-constructible", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> base{runtime, 7};
    Computed<NoDefault> const boxed{runtime, [&] { return NoDefault{base.get()}; }};
    CHECK(boxed.get().value() == 7);
}

TEST_CASE("reactive::Computed: accepts a move-only computation", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> base{runtime, 1};
    Computed<int> const offset{runtime, [owned = std::make_unique<int>(10), &base] { return *owned + base.get(); }};
    CHECK(offset.get() == 11);
}

TEST_CASE("reactive::Computed: a throw reaches the reader, the next read retries", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> divisor{runtime, 0};
    int calls = 0;
    Computed<int> const quotient{runtime, [&] {
                                     ++calls;
                                     return divide(divisor);
                                 }};
    CHECK_THROWS_AS(quotient.get(), std::domain_error);
    CHECK_THROWS_AS(quotient.get(), std::domain_error);
    CHECK(calls == 2);
    CHECK(runtime.core()->tracking() == nullptr);
    divisor.set(4);
    CHECK(quotient.get() == 25);
}

TEST_CASE("reactive::Computed: a computed that failed still hears about later changes", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> divisor{runtime, 0};
    Computed<int> const quotient{runtime, [&] { return divide(divisor); }};
    int seen = -1;
    int failures = 0;
    Effect const watch{runtime, [&] {
                           try {
                               seen = quotient.get();
                           } catch (std::domain_error const&) {
                               ++failures;
                           }
                       }};
    CHECK(failures == 1);
    divisor.set(5);
    owner.runAll();
    CHECK(seen == 20);
}

TEST_CASE("reactive::Computed: a failure reaches a reader through another Computed, every time", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> divisor{runtime, 5};
    int calls = 0;
    Computed<int> const quotient{runtime, [&] {
                                     ++calls;
                                     return divide(divisor);
                                 }};
    Computed<int> const plusOne{runtime, [&] { return quotient.get() + 1; }};
    CHECK(plusOne.get() == 21);
    divisor.set(0);
    CHECK_THROWS_AS(plusOne.get(), std::domain_error);
    CHECK(calls == 2);  // once per read, not once per level
    CHECK_THROWS_AS(plusOne.get(), std::domain_error);
    CHECK(calls == 3);
    divisor.set(4);
    CHECK(plusOne.get() == 26);
}

TEST_CASE("reactive::Computed: an Effect that catches a failure sees it, and is not reported", "[reactive]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    Signal<int> divisor{runtime, 5};
    Computed<int> const quotient{runtime, [&] { return divide(divisor); }};
    std::vector<int> seen;  // -1 for a failure
    Effect const watch{runtime, [&] {
                           try {
                               seen.push_back(quotient.get());
                           } catch (std::domain_error const&) {
                               seen.push_back(-1);
                           }
                       }};
    divisor.set(0);
    owner.runAll();
    divisor.set(4);
    owner.runAll();
    CHECK(seen == std::vector{20, -1, 25});
    CHECK(probe.count(site::kEffectThrew) == 0);
}

TEST_CASE("reactive::Computed: recovering to the value it had before a failure still propagates", "[reactive]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    Signal<int> divisor{runtime, 5};
    Computed<int> const quotient{runtime, [&] { return divide(divisor); }};
    std::vector<int> seen;  // -1 for a failure
    Effect const watch{runtime, [&] {
                           try {
                               seen.push_back(quotient.get());
                           } catch (std::domain_error const&) {
                               seen.push_back(-1);
                           }
                       }};
    divisor.set(0);
    owner.runAll();
    divisor.set(5);
    owner.runAll();
    CHECK(seen == std::vector{20, -1, 20});
}

TEST_CASE("reactive::Computed: a failure that repeats on a retry wakes no other reader", "[reactive]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    Signal<int> divisor{runtime, 5};
    Computed<int> const quotient{runtime, [&] { return divide(divisor); }};
    auto const reader = [&quotient](int& runs, int& failures) {
        return [&quotient, &runs, &failures] {
            ++runs;
            try {
                static_cast<void>(quotient.get());
            } catch (std::domain_error const&) {
                ++failures;
            }
        };
    };
    int firstRuns = 0;
    int firstFailures = 0;
    int secondRuns = 0;
    int secondFailures = 0;
    Effect const first{runtime, reader(firstRuns, firstFailures)};
    Effect const second{runtime, reader(secondRuns, secondFailures)};
    divisor.set(0);
    owner.runAll();
    CHECK(probe.count(site::kWriteCycle) == 0);
    CHECK(probe.count(site::kEffectThrew) == 0);
    CHECK(firstRuns == 2);
    CHECK(secondRuns == 2);
    CHECK(firstFailures == 1);
    CHECK(secondFailures == 1);
}

TEST_CASE("reactive::Computed: a different failure after a source change propagates", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> value{runtime, 1};
    Computed<int> const checked{runtime, [&] {
                                    int const current = value.get();
                                    if (current < 0) {
                                        throw std::runtime_error{"bad " + std::to_string(current)};
                                    }
                                    return current;
                                }};
    std::vector<std::string> seen;
    Effect const watch{runtime, [&] {
                           try {
                               seen.push_back(std::to_string(checked.get()));
                           } catch (std::runtime_error const& failure) {
                               seen.emplace_back(failure.what());
                           }
                       }};
    value.set(-1);
    owner.runAll();
    value.set(-2);
    owner.runAll();
    CHECK(seen == std::vector<std::string>{"1", "bad -1", "bad -2"});

    // A read before the flush recomputes the stale Computed; that is not a retry, so the Effect still
    // re-runs.
    value.set(-3);
    CHECK_THROWS_AS(checked.get(), std::runtime_error);
    owner.runAll();
    CHECK(seen == std::vector<std::string>{"1", "bad -1", "bad -2", "bad -3"});
}

TEST_CASE("reactive::Computed: a read of a failed Computed whose sources changed but compared equal is a retry",
          "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> base{runtime, 1};
    Computed<int> const parity{runtime, [&] { return base.get() % 2; }};
    Computed<int> const failing{runtime, [&]() -> int {
                                    static_cast<void>(parity.get());
                                    throw std::domain_error{"always"};
                                }};
    int runs = 0;
    int failures = 0;
    Effect const watch{runtime, [&] {
                           ++runs;
                           try {
                               static_cast<void>(failing.get());
                           } catch (std::domain_error const&) {
                               ++failures;
                           }
                       }};
    base.set(3);  // `parity` is stale but will compare equal; `failing` is only Check
    CHECK_THROWS_AS(failing.get(), std::domain_error);
    owner.runAll();
    CHECK(runs == 1);
    CHECK(failures == 1);
}

TEST_CASE("reactive::Computed: a retry that recovers outside a flush posts one", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> base{runtime, 5};
    bool broken = true;  // a non-reactive input
    Computed<int> const twice{runtime, [&] {
                                  if (broken) {
                                      throw std::domain_error{"broken"};
                                  }
                                  return base.get() * 2;
                              }};
    std::vector<int> seen;  // -1 for a failure
    Effect const first{runtime, [&] {
                           try {
                               seen.push_back(twice.get());
                           } catch (std::domain_error const&) {
                               seen.push_back(-1);
                           }
                       }};
    broken = false;
    Effect const second{runtime, [&] { static_cast<void>(twice.get()); }};  // retries, and recovers
    CHECK(owner.pending() == 1);
    owner.runAll();
    CHECK(seen == std::vector{-1, 10});
}

TEST_CASE("reactive::Computed: a cycle formed after the first evaluation is reported and refused",
          "[reactive][misuse]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    Signal<int> value{runtime, 0};
    std::unique_ptr<Computed<int>> outer;
    Computed<int> const inner{runtime, [&] { return value.get() == 1 ? outer->get() : value.get(); }};
    outer = std::make_unique<Computed<int>>(runtime, [&] { return inner.get() + 1; });
    CHECK(outer->get() == 1);
    value.set(1);  // now `inner` reads `outer`, which reads `inner`
    CHECK_THROWS_AS(outer->get(), std::logic_error);
    CHECK(probe.count(site::kComputedReadsItself) == 1);
    value.set(2);
    CHECK(outer->get() == 3);
}

TEST_CASE("reactive::Computed: peek does not subscribe", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> base{runtime, 1};
    Computed<int> const twice{runtime, [&] { return base.get() * 2; }};
    int runs = 0;
    Effect const watch{runtime, [&] {
                           ++runs;
                           static_cast<void>(twice.peek());
                       }};
    base.set(2);
    CHECK(owner.pending() == 0);
    CHECK(twice.peek() == 4);
}

TEST_CASE("reactive::Computed: one destroyed by its own computation during a flush leaves the graph intact",
          "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> value{runtime, 0};
    std::unique_ptr<Computed<int>> doomed;
    doomed = std::make_unique<Computed<int>>(runtime, [&] {
        if (value.get() == 1) {
            doomed.reset();
        }
        return value.peek();  // reached through the closure after its Computed is gone
    });
    int runs = 0;
    Effect const watch{runtime, [&] {
                           ++runs;
                           if (doomed != nullptr) {
                               static_cast<void>(doomed->get());
                           }
                       }};
    value.set(1);  // the Effect's pull recomputes `doomed`, whose computation destroys it
    owner.runAll();
    CHECK(doomed == nullptr);
    CHECK(runs == 1);
    value.set(2);
    CHECK(owner.pending() == 0);
}

TEST_CASE("reactive::Computed: a read whose computation destroys the Computed throws logic_error", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> value{runtime, 0};
    std::unique_ptr<Computed<int>> doomed;
    doomed = std::make_unique<Computed<int>>(runtime, [&] {
        doomed.reset();
        return value.get();  // reached through the closure after its Computed is gone
    });
    CHECK_THROWS_AS(doomed->get(), std::logic_error);
    CHECK(doomed == nullptr);
    CHECK(runtime.core()->tracking() == nullptr);
    CHECK_FALSE(runtime.core()->isComputing());
    value.set(1);
    CHECK(owner.pending() == 0);
}

TEST_CASE("reactive::Computed: a read off the owner is reported, recomputes nothing and subscribes nothing",
          "[reactive][misuse]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    Signal<int> base{runtime, 1};
    int calls = 0;
    Computed<int> const twice{runtime, [&] {
                                  ++calls;
                                  return base.get() * 2;
                              }};
    Computed<int> const unread{runtime, [&] {
                                   ++calls;
                                   return 0;
                               }};
    CHECK(twice.get() == 2);
    base.set(5);  // `twice` is stale now, and nothing observes it
    int read = 0;
    int peeked = 0;
    bool unreadThrew = false;
    int runs = 0;
    Effect const watch{runtime, [&] {
                           ++runs;
                           // Off the owner while this Effect's tracking frame is installed.
                           std::thread{[&] {
                               read = twice.get();
                               peeked = twice.peek();
                               try {
                                   static_cast<void>(unread.get());
                               } catch (std::logic_error const&) {
                                   unreadThrew = true;
                               }
                           }}.join();
                       }};
    CHECK(probe.count(site::kOffOwner) == 3);
    CHECK(read == 2);
    CHECK(peeked == 2);
    CHECK(unreadThrew);
    CHECK(calls == 1);
    base.set(6);
    CHECK(owner.pending() == 0);
    CHECK(runs == 1);
}
