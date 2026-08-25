// SPDX-License-Identifier: Apache-2.0

// Covers the public "settleable promise" seam for `morph::async::Completion<T>`
// (issue #55, use case 1): a test-facing way to construct a `Completion<T>` it
// can resolve on demand, without reaching into `morph::async::detail::CompletionState<T>`.

#include <catch2/catch_test_macros.hpp>
#include <morph/core/completion.hpp>
#include <stdexcept>
#include <string>

#include "test_support.hpp"

using SyncExec = morph::testing::InlineExecutor;

TEST_CASE("morph::async::Completion::makeSettleable resolves the paired Completion via resolve()",
          "[completion][promise]") {
    SyncExec exec;
    auto [completion, promise] = morph::async::Completion<int>::makeSettleable(&exec);

    int received = -1;
    completion.then([&](int val) { received = val; });

    promise.resolve(42);
    REQUIRE(received == 42);
}

TEST_CASE("morph::async::Completion::makeSettleable rejects the paired Completion via reject()",
          "[completion][promise]") {
    SyncExec exec;
    auto [completion, promise] = morph::async::Completion<int>::makeSettleable(&exec);

    bool errorFired = false;
    completion.onError([&](const std::exception_ptr& exc) {
        try {
            std::rethrow_exception(exc);
        } catch (const std::runtime_error& ex) {
            errorFired = (std::string{ex.what()} == "settle failure");
        }
    });

    promise.reject(std::make_exception_ptr(std::runtime_error{"settle failure"}));
    REQUIRE(errorFired);
}

TEST_CASE("morph::async::Completion::makeSettleable: then() attached after resolve() fires immediately",
          "[completion][promise]") {
    SyncExec exec;
    auto [completion, promise] = morph::async::Completion<int>::makeSettleable(&exec);

    promise.resolve(7);

    int received = -1;
    completion.then([&](int val) { received = val; });
    REQUIRE(received == 7);
}

TEST_CASE("morph::async::Completion::makeSettleable: resolve() after reject() is a no-op (first result wins)",
          "[completion][promise]") {
    SyncExec exec;
    auto [completion, promise] = morph::async::Completion<int>::makeSettleable(&exec);

    bool errorFired = false;
    completion.onError([&](const std::exception_ptr&) { errorFired = true; });

    promise.reject(std::make_exception_ptr(std::runtime_error{"first"}));
    promise.resolve(99);  // must not overwrite the already-settled error

    REQUIRE(errorFired);
}

TEST_CASE("morph::async::Completion::makeSettleable works with a null executor (write-only endpoint)",
          "[completion][promise]") {
    auto [completion, promise] = morph::async::Completion<int>::makeSettleable(nullptr);

    bool handlerRan = false;
    completion.then([&](int) { handlerRan = true; });

    promise.resolve(1);
    REQUIRE_FALSE(handlerRan);  // no executor: never delivered, but must not throw/crash
}

TEST_CASE("morph::async::Completion::makeSettleable: Promise<T> does not expose CompletionState",
          "[completion][promise]") {
    // Compile-time-ish check: Promise<T> is usable without ever naming
    // morph::async::detail::CompletionState<T>. If this test compiles and
    // links, the seam does not require reaching into `detail`.
    SyncExec exec;
    auto [completion, promise] = morph::async::Completion<std::string>::makeSettleable(&exec);
    (void)completion;
    promise.resolve("ok");
    SUCCEED();
}
