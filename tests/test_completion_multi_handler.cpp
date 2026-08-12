// SPDX-License-Identifier: Apache-2.0

// Regression coverage for issue #59: Completion<T>::onError() (and, symmetrically,
// then()) used to keep only the last-attached handler in a single field, silently
// discarding any earlier one. These tests pin down the fixed, composing behavior:
// every handler attached while the state is not yet ready runs when the outcome
// arrives, in attachment order.

#include <morph/core/completion.hpp>
#include <morph/core/logger.hpp>
#include <catch2/catch_test_macros.hpp>
#include <stdexcept>
#include <string>
#include <vector>

#include "test_support.hpp"

using SyncExecutor = morph::testing::InlineExecutor;
using LogGuard = morph::log::ScopedLoggerOverride;

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
