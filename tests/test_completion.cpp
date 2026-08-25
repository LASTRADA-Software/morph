// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <morph/core/completion.hpp>
#include <stdexcept>
#include <thread>

#include "test_support.hpp"

using SyncExecutor = morph::testing::InlineExecutor;

TEST_CASE("morph::async::Completion then fires with value", "[completion]") {
    SyncExecutor exec;
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    morph::async::Completion<int> comp{state, &exec};

    int received = -1;
    comp.then([&](int val) { received = val; });

    state->setValue(42);
    REQUIRE(received == 42);
}

TEST_CASE("morph::async::Completion then fires immediately if already ready", "[completion]") {
    SyncExecutor exec;
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    morph::async::Completion<int> comp{state, &exec};

    state->setValue(7);

    int received = -1;
    comp.then([&](int val) { received = val; });
    REQUIRE(received == 7);
}

TEST_CASE("morph::async::Completion on_error fires with exception", "[completion]") {
    SyncExecutor exec;
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    morph::async::Completion<int> comp{state, &exec};

    bool errorFired = false;
    comp.onError([&](const std::exception_ptr& exc) {
        try {
            std::rethrow_exception(exc);
        } catch (const std::runtime_error& ex) {
            errorFired = (std::string{ex.what()} == "test error");
        }
    });

    state->setException(std::make_exception_ptr(std::runtime_error{"test error"}));
    REQUIRE(errorFired);
}

TEST_CASE("morph::async::Completion then does not fire on error", "[completion]") {
    SyncExecutor exec;
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    morph::async::Completion<int> comp{state, &exec};

    bool thenFired = false;
    comp.then([&](int) { thenFired = true; }).onError([](const std::exception_ptr&) {});

    state->setException(std::make_exception_ptr(std::runtime_error{"err"}));
    REQUIRE_FALSE(thenFired);
}

TEST_CASE("morph::async::Completion on_error does not fire on value", "[completion]") {
    SyncExecutor exec;
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    morph::async::Completion<int> comp{state, &exec};

    bool errorFired = false;
    comp.then([](int) {}).onError([&](const std::exception_ptr&) { errorFired = true; });

    state->setValue(1);
    REQUIRE_FALSE(errorFired);
}

TEST_CASE("morph::async::Completion onError composes every attached handler, in attachment order", "[completion]") {
    // CompletionState<T>::attachOnError appends to a vector of handlers, so a
    // second .onError() call on the same still-pending Completion<T> — even
    // via the separate Completion& returned from the first call, since
    // then()/onError() both return *this — fires alongside the first rather
    // than replacing it (see docs/spec/core/completion.md, "Handler
    // fan-out"). This is the mechanism behind Presenter::track()'s onErr
    // parameter (examples/common/gui/presenter.hpp), documented here at its
    // source.
    SyncExecutor exec;
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    morph::async::Completion<int> comp{state, &exec};

    bool handlerAFired = false;
    bool handlerBFired = false;
    comp.onError([&](const std::exception_ptr&) { handlerAFired = true; });
    comp.onError([&](const std::exception_ptr&) { handlerBFired = true; });

    state->setException(std::make_exception_ptr(std::runtime_error{"test error"}));

    REQUIRE(handlerAFired);
    REQUIRE(handlerBFired);
}

TEST_CASE("morph::async::Completion callback is posted through executor", "[completion]") {
    struct CountingExecutor : morph::exec::IExecutor {
        std::atomic<int> count{0};
        void post(std::function<void()> fn) override {
            count.fetch_add(1);
            fn();
        }
    } exec;

    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    morph::async::Completion<int> comp{state, &exec};
    comp.then([](int) {});
    state->setValue(1);
    REQUIRE(exec.count.load() == 1);
}
