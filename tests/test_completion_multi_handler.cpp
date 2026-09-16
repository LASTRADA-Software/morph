// SPDX-License-Identifier: Apache-2.0

// Regression coverage for issue #59: Completion<T>::onError() (and, symmetrically,
// then()) used to keep only the last-attached handler in a single field, silently
// discarding any earlier one. These tests pin down the fixed, composing behavior:
// every handler attached while the state is not yet ready runs when the outcome
// arrives, in attachment order.

#include <catch2/catch_test_macros.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/logger.hpp>
#include <stdexcept>
#include <string>
#include <vector>

#include "test_support.hpp"

using SyncExecutor = morph::testing::InlineExecutor;
using LogGuard = morph::log::ScopedLoggerOverride;

namespace {

/// A value whose *copy* constructor throws on demand. `setValue`'s first act on
/// a state with handlers attached is `auto savedVal = val;` -- the copy
/// morph#520 introduced -- so this is what exercises the strong exception
/// guarantee documented there. The flag travels with the value rather than
/// living in a global so two tests cannot arm each other.
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
    // This is the exact reproducer from issue #59.
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

// Regression coverage for morph#520 (part of the sweep tracked in #518, finding F2).
// setValue() used to move out of its own `value` optional to build the settle-time
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

TEST_CASE("Completion: a throwing T copy leaves the state unsettled with every handler intact",
          "[completion][issue-520]") {
    // The guarantee `setValue` documents: the copy is taken *before* `onOk` is
    // drained or `value`/`ready` are set, so a throwing copy constructor must
    // leave the state exactly as it was -- unready, with every handler still
    // attached -- rather than half-settled with its handlers already lost.
    SyncExecutor exec;
    auto state = std::make_shared<morph::async::detail::CompletionState<ThrowOnCopy>>();
    morph::async::Completion<ThrowOnCopy> comp{state, &exec};

    int fired = 0;
    comp.then([&](ThrowOnCopy) { ++fired; });
    comp.then([&](ThrowOnCopy) { ++fired; });

    REQUIRE_THROWS_AS(state->setValue(ThrowOnCopy{7, true}), std::runtime_error);

    CHECK_FALSE(state->ready);
    CHECK_FALSE(state->value.has_value());
    CHECK(state->onOk.size() == 2U);
    CHECK(fired == 0);

    // And the state is still usable afterwards: the failed settlement consumed
    // nothing, so a later non-throwing one settles normally and both handlers
    // -- the ones that survived the throw -- run.
    state->setValue(ThrowOnCopy{9, false});

    CHECK(state->ready);
    CHECK(fired == 2);
}
