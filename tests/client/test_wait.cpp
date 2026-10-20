// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <exception>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <stdexcept>
#include <testkit/step_executor.hpp>
#include <testkit/wait.hpp>
#include <thread>

using morph::examples::testing::awaitOn;
using morph::examples::testing::pumpUntil;
using namespace std::chrono_literals;

TEST_CASE("pumpUntil: runs posted work, including work posted from another thread", "[client][wait]") {
    morph::exec::MainThreadExecutor owner;
    bool done = false;
    std::thread poster{[&owner, &done] { owner.post([&owner, &done] { owner.post([&done] { done = true; }); }); }};
    CHECK(pumpUntil(owner, [&done] { return done; }));
    poster.join();
}

TEST_CASE("pumpUntil: gives up when the budget runs out", "[client][wait]") {
    morph::exec::MainThreadExecutor owner;
    CHECK_FALSE(pumpUntil(owner, [] { return false; }, 20ms));
    morph::ladder::testkit::StepExecutor stepped;
    CHECK_FALSE(pumpUntil(stepped, [] { return false; }, 20ms));
}

TEST_CASE("pumpUntil: steps a StepExecutor until the predicate holds", "[client][wait]") {
    morph::ladder::testkit::StepExecutor owner;
    std::atomic<bool> done{false};
    std::thread poster{[&owner, &done] { owner.post([&done] { done = true; }); }};
    CHECK(pumpUntil(owner, [&done] { return done.load(); }));
    poster.join();
}

TEST_CASE("awaitOn: returns the value, rethrows the failure, and gives up at the budget", "[client][wait]") {
    morph::exec::MainThreadExecutor owner;
    {
        auto [completion, promise] = morph::async::Completion<int>::makeSettleable(&owner);
        std::thread settler{[&promise] { promise.resolve(7); }};
        int const value = awaitOn(owner, std::move(completion));
        CHECK(value == 7);
        settler.join();
    }
    {
        auto [completion, promise] = morph::async::Completion<int>::makeSettleable(&owner);
        promise.reject(std::make_exception_ptr(std::runtime_error{"nope"}));
        CHECK_THROWS_WITH(awaitOn(owner, std::move(completion)), "nope");
    }
    {
        auto [completion, promise] = morph::async::Completion<int>::makeSettleable(&owner);
        CHECK_THROWS_AS(awaitOn(owner, std::move(completion), 20ms), std::runtime_error);
    }
}
