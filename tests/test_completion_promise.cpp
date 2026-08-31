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

// ── Issue #347: a null exception_ptr must not settle the error arm ──────────
//
// `setException(nullptr)` used to set `ready` while leaving `error` falsy — a
// state neither `attachOnError` (which tests `ready && error`) nor `attachThen`
// (which tests `ready && value`) can act on. Every handler attached afterwards
// was silently discarded: not fired, not queued, and not even reported by the
// orphan logger, since `attachOnError` sets `onErrAttached` on entry. The
// completion simply never resolved for that caller, in either direction.
//
// `Promise<T>::reject()` takes the `exception_ptr` by value and does not
// document a non-null precondition, so `reject(nullptr)` is a call an ordinary
// caller can make today — and a forwarding producer (`.onError([state](auto e)
// { state->setException(e); })`, of which there are about ten in the tree)
// propagates whatever it is handed. The guard therefore lives in
// `CompletionState::setException` rather than at any one producer.

TEST_CASE("morph::async::Completion::makeSettleable: reject(nullptr) still reaches an onError attached afterwards",
          "[completion][promise][issue347]") {
    SyncExec exec;
    auto [completion, promise] = morph::async::Completion<int>::makeSettleable(&exec);

    promise.reject(nullptr);

    bool errorFired = false;
    completion.onError([&](const std::exception_ptr& exc) { errorFired = (exc != nullptr); });
    REQUIRE(errorFired);
}

TEST_CASE(
    "morph::async::Completion::makeSettleable: reject(nullptr) hands an already-attached handler a real exception",
    "[completion][promise][issue347]") {
    // The other half of the same defect: a handler attached *before* the null
    // rejection did fire, but with a null `exception_ptr` — and
    // `std::rethrow_exception` requires a non-null argument, so the idiomatic
    // handler body (including morph's own orphan logger) was undefined
    // behaviour. Whatever is substituted must be rethrowable.
    SyncExec exec;
    auto [completion, promise] = morph::async::Completion<int>::makeSettleable(&exec);

    bool sawNonNull = false;
    bool rethrowWorked = false;
    completion.onError([&](const std::exception_ptr& exc) {
        sawNonNull = (exc != nullptr);
        if (!sawNonNull) {
            return;  // rethrowing a null exception_ptr is UB; do not.
        }
        try {
            std::rethrow_exception(exc);
        } catch (const std::exception&) {
            rethrowWorked = true;
        } catch (...) {
        }
    });

    promise.reject(nullptr);
    CHECK(sawNonNull);
    REQUIRE(rethrowWorked);
}

TEST_CASE("morph::async::Completion::makeSettleable: reject(nullptr) settles the completion rather than wedging it",
          "[completion][promise][issue347]") {
    // Before the fix this state was dead in both directions: `ready` was set,
    // so `setValue` returned early and `attachThen` found no value, while
    // `attachOnError` found no error. Neither arm could ever fire again, from
    // any caller. After the fix it is an ordinary settled-with-error state, so
    // first-result-wins still holds and the success arm stays silent.
    SyncExec exec;
    auto [completion, promise] = morph::async::Completion<int>::makeSettleable(&exec);

    promise.reject(nullptr);

    bool thenFired = false;
    int errorCount = 0;
    completion.then([&](int) { thenFired = true; }).onError([&](const std::exception_ptr&) { ++errorCount; });
    CHECK_FALSE(thenFired);
    CHECK(errorCount == 1);

    // A later resolve() must not revive it -- the first result already won.
    promise.resolve(7);
    CHECK_FALSE(thenFired);

    // ...and a second onError still fires against the settled error, exactly
    // as it would for a rejection carrying a caller-supplied exception.
    int secondCount = 0;
    completion.onError([&](const std::exception_ptr& exc) { secondCount += (exc != nullptr) ? 1 : 0; });
    CHECK(secondCount == 1);
}
