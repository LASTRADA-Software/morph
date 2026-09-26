// SPDX-License-Identifier: Apache-2.0

// Completion<T>::onError() (and, symmetrically,
// then()) must not keep only the last-attached handler in a single field, silently
// discarding any earlier one. These tests pin down the fixed, composing behavior:
// every handler attached while the state is not yet ready runs when the outcome
// arrives, in attachment order.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/core/completion.hpp>
#include <morph/core/logger.hpp>
#include <stdexcept>
#include <string>
#include <vector>

#include "test_support.hpp"

using SyncExecutor = morph::testing::InlineExecutor;
using LogGuard = morph::log::ScopedLoggerOverride;

namespace {

/// A value whose *copy* constructor throws on demand. Under the value contract
/// nothing on the value path copies `T`, so an armed `ThrowOnCopy`
/// settling through `const T&` handlers is a booby trap that must never go off
/// -- which is what turns "zero copies" from a comment into a test. The flag
/// travels with the value rather than living in a global so two tests cannot
/// arm each other.
struct ThrowOnCopy {
    int payload = 0;
    bool explode = false;

    ThrowOnCopy() = default;
    ThrowOnCopy(int value, bool boom) : payload{value}, explode{boom} {}
    ThrowOnCopy(const ThrowOnCopy& other) : payload{other.payload}, explode{other.explode} {
        if (explode) {
            throw std::runtime_error{"copy ctor blew up"};
        }
    }
    ThrowOnCopy(ThrowOnCopy&&) noexcept = default;
    ThrowOnCopy& operator=(const ThrowOnCopy&) = default;
    ThrowOnCopy& operator=(ThrowOnCopy&&) noexcept = default;
    ~ThrowOnCopy() = default;
};

/// A value whose *move* constructor throws on demand. `setValue`'s only act on
/// `T` is `value = std::move(val)`, so this is what exercises the strong
/// exception guarantee it documents: the store happens before `onOk` is
/// drained, so an escape must leave the state exactly as it was.
///
/// `setValue(T val)` takes its argument by value and every call below passes a
/// prvalue, so C++17 guaranteed elision constructs it directly in the
/// parameter: the move inside `setValue` is the *first* move this type ever
/// sees, and arming it on construction is enough.
struct ThrowOnMove {
    int payload = 0;
    bool explode = false;

    ThrowOnMove() = default;
    ThrowOnMove(int value, bool boom) : payload{value}, explode{boom} {}
    ThrowOnMove(const ThrowOnMove&) = default;
    // A throwing, non-`noexcept` move constructor is the whole point of this
    // fixture -- `bugprone-exception-escape` and the two noexcept-move checks
    // are correct about ordinary code and wrong about a fault injector.
    // NOLINTNEXTLINE(bugprone-exception-escape,cppcoreguidelines-noexcept-move-operations,performance-noexcept-move-constructor)
    ThrowOnMove(ThrowOnMove&& other) : payload{other.payload}, explode{other.explode} {
        if (explode) {
            throw std::runtime_error{"move ctor blew up"};
        }
    }
    ThrowOnMove& operator=(const ThrowOnMove&) = default;
    ThrowOnMove& operator=(ThrowOnMove&&) = default;
    ~ThrowOnMove() = default;
};

}  // namespace

TEST_CASE("Completion: multiple onError handlers all fire, in attachment order", "[completion][issue-59]") {
    SyncExecutor exec;
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    morph::async::Completion<int> comp{state, &exec};

    std::vector<int> firedOrder;
    comp.onError([&](const std::exception_ptr&) { firedOrder.push_back(1); });
    comp.onError([&](const std::exception_ptr&) { firedOrder.push_back(2); });
    comp.onError([&](const std::exception_ptr&) { firedOrder.push_back(3); });

    state->setException(std::make_exception_ptr(std::runtime_error{"boom"}));

    REQUIRE(firedOrder == std::vector<int>{1, 2, 3});
}

TEST_CASE("Completion: multiple then handlers all fire, in attachment order", "[completion][issue-59]") {
    SyncExecutor exec;
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    morph::async::Completion<int> comp{state, &exec};

    std::vector<int> firedOrder;
    comp.then([&](int) { firedOrder.push_back(1); });
    comp.then([&](int) { firedOrder.push_back(2); });

    state->setValue(42);

    REQUIRE(firedOrder == std::vector<int>{1, 2});
}

TEST_CASE("Completion: onError handlers attached after error is ready all fire (fire-now composes too)",
          "[completion][issue-59]") {
    SyncExecutor exec;
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    morph::async::Completion<int> comp{state, &exec};

    state->setException(std::make_exception_ptr(std::runtime_error{"already-ready"}));

    int firstCount = 0;
    int secondCount = 0;
    comp.onError([&](const std::exception_ptr&) { firstCount++; });
    comp.onError([&](const std::exception_ptr&) { secondCount++; });

    REQUIRE(firstCount == 1);
    REQUIRE(secondCount == 1);
}

TEST_CASE("Completion: a second onError attached before ready does not discard the first", "[completion][issue-59]") {
    // The minimal reproducer for a single-slot handler field.
    SyncExecutor exec;
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    morph::async::Completion<int> comp{state, &exec};

    bool firstFired = false;
    bool secondFired = false;
    comp.onError([&](const std::exception_ptr&) { firstFired = true; });
    comp.onError([&](const std::exception_ptr&) { secondFired = true; });

    state->setException(std::make_exception_ptr(std::runtime_error{"err"}));

    REQUIRE(firstFired);
    REQUIRE(secondFired);
}

TEST_CASE("Completion: onErrAttached still suppresses orphan logging with multiple handlers",
          "[completion][issue-59]") {
    SyncExecutor exec;
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    {
        morph::async::Completion<int> comp{state, &exec};
        comp.onError([](const std::exception_ptr&) {});
        comp.onError([](const std::exception_ptr&) {});
    }
    state->setException(std::make_exception_ptr(std::runtime_error{"handled"}));
    REQUIRE(state->onErrAttached);
    // Destructor (state falls out of scope at end of test) must not log an orphan;
    // there is nothing to assert directly here beyond "does not crash", covered by
    // the shared-state destructor running at end of scope.
}

TEST_CASE("Completion: a throwing onError handler does not prevent later handlers from firing",
          "[completion][issue-59]") {
    // Regression: composing handlers in one posted closure must isolate each
    // invocation -- otherwise the first handler throwing would silently skip
    // every handler attached after it.
    LogGuard guard;
    SyncExecutor exec;
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    morph::async::Completion<int> comp{state, &exec};

    bool secondFired = false;
    bool thirdFired = false;
    comp.onError([&](const std::exception_ptr&) { throw std::runtime_error{"handler blew up"}; });
    comp.onError([&](const std::exception_ptr&) { secondFired = true; });
    comp.onError([&](const std::exception_ptr&) { thirdFired = true; });

    REQUIRE_NOTHROW(state->setException(std::make_exception_ptr(std::runtime_error{"err"})));

    REQUIRE(secondFired);
    REQUIRE(thirdFired);
}

TEST_CASE("Completion: a throwing then handler does not prevent later handlers from firing",
          "[completion][issue-59]") {
    LogGuard guard;
    SyncExecutor exec;
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    morph::async::Completion<int> comp{state, &exec};

    bool secondFired = false;
    bool thirdFired = false;
    comp.then([&](int) { throw std::runtime_error{"handler blew up"}; });
    comp.then([&](int) { secondFired = true; });
    comp.then([&](int) { thirdFired = true; });

    REQUIRE_NOTHROW(state->setValue(1));

    REQUIRE(secondFired);
    REQUIRE(thirdFired);
}

TEST_CASE("Completion: a throwing last then handler is isolated the same as a non-last one",
          "[completion][issue-59]") {
    // Regression: the final handler runs outside the non-last loop (it's the
    // one that receives the moved-from value), with its own separate
    // try/catch -- exercise that path specifically, not just a non-last
    // handler throwing.
    LogGuard guard;
    SyncExecutor exec;
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    morph::async::Completion<int> comp{state, &exec};

    bool firstFired = false;
    comp.then([&](int) { firstFired = true; });
    comp.then([&](int) { throw std::runtime_error{"handler blew up"}; });

    REQUIRE_NOTHROW(state->setValue(1));
    REQUIRE(firstFired);
}

TEST_CASE("Completion: a throwing then handler attached after settlement is isolated too", "[completion]") {
    // A handler attached after the value is already stored takes the attach's
    // own fire-now path rather than setValue's composed closure. It must get
    // the same isolation: otherwise whether a throwing handler reaches the
    // executor depends only on which side of settlement the attach landed.
    const LogGuard guard;
    SyncExecutor exec;
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    morph::async::Completion<int> comp{state, &exec};
    state->setValue(1);

    bool laterFired = false;
    REQUIRE_NOTHROW(comp.then([&](int) { throw std::runtime_error{"handler blew up"}; }));
    comp.then([&](int) { laterFired = true; });
    REQUIRE(laterFired);
}

TEST_CASE("Completion: a throwing onError handler attached after settlement is isolated too", "[completion]") {
    const LogGuard guard;
    SyncExecutor exec;
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    morph::async::Completion<int> comp{state, &exec};
    state->setException(std::make_exception_ptr(std::runtime_error{"err"}));

    bool laterFired = false;
    REQUIRE_NOTHROW(comp.onError([&](const std::exception_ptr&) { throw std::runtime_error{"handler blew up"}; }));
    comp.onError([&](const std::exception_ptr&) { laterFired = true; });
    REQUIRE(laterFired);
}

TEST_CASE("Completion: mismatched attach (onError on a value-ready state) is still a no-op for all handlers",
          "[completion][issue-59]") {
    SyncExecutor exec;
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    morph::async::Completion<int> comp{state, &exec};

    state->setValue(1);

    bool errFired1 = false;
    bool errFired2 = false;
    comp.onError([&](const std::exception_ptr&) { errFired1 = true; });
    comp.onError([&](const std::exception_ptr&) { errFired2 = true; });

    REQUIRE_FALSE(errFired1);
    REQUIRE_FALSE(errFired2);
}

// `setValue()` must not move out of its own `value` optional to build the settle-time
// fan-out closure for handlers attached *before* settling, leaving `value` engaged
// but holding a moved-from T. A then() attached *after* settling (attachThen's
// `ready && value` branch) then copied that husk instead of the real value. The
// value-path-before-settling and error-path-after-settling combinations already had
// coverage above (and in the onError-after-ready case); this is the one combination
// that did not: a value-path handler attached before settling, followed by a second
// one attached after. A short int (as every other test in this file uses) would not
// reveal the bug -- a moved-from int is still a well-defined int, often still 0 by
// luck or unchanged by move -- so this uses a heap-allocating string long enough that
// libstdc++'s SSO cannot mask a real move, matching the issue's own repro.
TEST_CASE("Completion: a then() attached after settlement observes the same value as one attached before",
          "[completion][issue-520]") {
    SyncExecutor exec;
    auto state = std::make_shared<morph::async::detail::CompletionState<std::string>>();
    morph::async::Completion<std::string> comp{state, &exec};

    const std::string original = "hello-world-long-enough-to-heap-allocate";

    // Two pre-settle handlers, not one. With a single handler `savedFns.size()`
    // is 1, so setValue's fan-out loop (`i + 1 < savedFns.size()`) never runs
    // and only the `savedFns.back()(std::move(savedVal))` arm is exercised --
    // yet the fan-out is exactly where `savedVal` is read repeatedly, and it is
    // the arm a future refactor is most likely to break by moving out of
    // `savedVal` early. The pre-existing [issue-59] multi-handler tests all use
    // `int`, which cannot reveal a moved-from value.
    std::string firstSeen;
    std::string fanOutSeen;
    comp.then([&](std::string v) { firstSeen = std::move(v); });   // attached BEFORE settling
    comp.then([&](std::string v) { fanOutSeen = std::move(v); });  // ditto: forces the fan-out arm

    state->setValue(original);

    std::string secondSeen;
    comp.then([&](std::string v) { secondSeen = std::move(v); });  // attached AFTER settling

    REQUIRE(firstSeen == original);
    REQUIRE(fanOutSeen == original);
    REQUIRE(secondSeen == original);
}

TEST_CASE("Completion: settling never copies T -- an armed throwing copy constructor never runs",
          "[completion][issue-553]") {
    // The value half of the contract, stated as a trap rather than a comment:
    // `onOk` is erased as `std::function<void(const T&)>` and both dispatch
    // paths read the stored value in place, so settling a state with `const T&`
    // handlers -- attached before *or* after -- copies `T` exactly zero times.
    // An armed `ThrowOnCopy` therefore settles without incident. Under an
    // erasure that copies, this throws: `setValue` copies into `savedVal` before
    // draining `onOk`, and the fire-now path copies twice more.
    SyncExecutor exec;
    auto state = std::make_shared<morph::async::detail::CompletionState<ThrowOnCopy>>();
    morph::async::Completion<ThrowOnCopy> comp{state, &exec};

    int fired = 0;
    int seen = 0;
    comp.then([&](const ThrowOnCopy& v) {
        ++fired;
        seen = v.payload;
    });
    comp.then([&](const ThrowOnCopy&) { ++fired; });

    REQUIRE_NOTHROW(state->setValue(ThrowOnCopy{7, true}));

    CHECK(state->ready);
    CHECK(fired == 2);
    CHECK(seen == 7);

    // The attach-after-ready path is copy-free too, and still sees the genuine
    // value rather than a husk -- the value is observed, never consumed.
    int lateSeen = 0;
    REQUIRE_NOTHROW(comp.then([&](const ThrowOnCopy& v) { lateSeen = v.payload; }));
    CHECK(lateSeen == 7);
}

TEST_CASE("Completion: a throwing T move leaves the state unsettled with every handler intact",
          "[completion][issue-520][issue-553]") {
    // The guarantee `setValue` documents: `value = std::move(val)` runs before
    // `onOk` is drained or `ready` is set, so a throwing move constructor must
    // leave the state exactly as it was -- unready, with every handler still
    // attached -- rather than half-settled with its handlers already lost.
    SyncExecutor exec;
    auto state = std::make_shared<morph::async::detail::CompletionState<ThrowOnMove>>();
    morph::async::Completion<ThrowOnMove> comp{state, &exec};

    int fired = 0;
    comp.then([&](const ThrowOnMove&) { ++fired; });
    comp.then([&](const ThrowOnMove&) { ++fired; });

    REQUIRE_THROWS_AS(state->setValue(ThrowOnMove{7, true}), std::runtime_error);

    CHECK_FALSE(state->ready);
    CHECK_FALSE(state->value.has_value());
    CHECK(state->onOk.size() == 2U);
    CHECK(fired == 0);

    // And the state is still usable afterwards: the failed settlement consumed
    // nothing, so a later non-throwing one settles normally and both handlers
    // -- the ones that survived the throw -- run.
    state->setValue(ThrowOnMove{9, false});

    CHECK(state->ready);
    CHECK(fired == 2);
}
