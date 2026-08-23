// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <morph/core/executor.hpp>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "step_executor.hpp"

namespace {

using morph::ladder::testkit::StepExecutor;

}  // namespace

TEST_CASE("StepExecutor queues a posted task instead of running it", "[ladder][testkit][executor]") {
    StepExecutor exec;
    bool ran = false;
    exec.post([&] { ran = true; });

    // The whole point: after post() returns, the worker has provably not run.
    // Against a real pool this is exactly the state that can only be sampled.
    CHECK(exec.pending() == 1);
    CHECK_FALSE(ran);

    REQUIRE(exec.runOne());
    CHECK(ran);
    CHECK(exec.pending() == 0);
}

TEST_CASE("StepExecutor::runOne reports an empty queue rather than throwing", "[ladder][testkit][executor]") {
    StepExecutor exec;
    CHECK_FALSE(exec.runOne());

    exec.post([] {});
    REQUIRE(exec.runOne());
    // "Nothing more was queued" is an ordinary assertion, not a CHECK_THROWS --
    // which is what distinguishes this from DeterministicExecutor::step().
    CHECK_FALSE(exec.runOne());
}

TEST_CASE("StepExecutor runs tasks oldest-first", "[ladder][testkit][executor]") {
    StepExecutor exec;
    std::vector<int> order;
    exec.post([&] { order.push_back(1); });
    exec.post([&] { order.push_back(2); });
    exec.post([&] { order.push_back(3); });

    REQUIRE(exec.pending() == 3);
    REQUIRE(exec.runOne());
    CHECK(order == std::vector<int>{1});
    CHECK(exec.pending() == 2);

    CHECK(exec.runAll() == 2);
    CHECK(order == std::vector<int>{1, 2, 3});
}

TEST_CASE("StepExecutor::runAll picks up tasks posted by a running task", "[ladder][testkit][executor]") {
    // A chained job must run to completion rather than stranding its own
    // continuation -- the property that separates runAll() from "run the
    // N tasks that happened to be queued when I was called".
    StepExecutor exec;
    std::vector<std::string> steps;
    exec.post([&] {
        steps.emplace_back("first");
        exec.post([&] {
            steps.emplace_back("second");
            exec.post([&] { steps.emplace_back("third"); });
        });
    });

    CHECK(exec.runAll() == 3);
    CHECK(steps == std::vector<std::string>{"first", "second", "third"});
    CHECK(exec.pending() == 0);
}

TEST_CASE("StepExecutor::runAll is bounded against a self-reposting task", "[ladder][testkit][executor]") {
    StepExecutor exec;
    int runs = 0;
    // Deliberate harness misuse: without the bound this hangs the process with
    // no assertion failure and no diagnostic.
    std::function<void()> repost;
    repost = [&] {
        ++runs;
        exec.post(repost);
    };
    exec.post(repost);

    CHECK_THROWS_AS(exec.runAll(/*maxSteps=*/16), std::runtime_error);
    CHECK(runs == 16);
}

TEST_CASE("StepExecutor lets a task's exception reach the test", "[ladder][testkit][executor]") {
    // Unlike ThreadPoolExecutor, which catches and logs: a swallowed exception
    // here would be a swallowed REQUIRE failure.
    StepExecutor exec;
    exec.post([] { throw std::runtime_error{"boom"}; });
    CHECK_THROWS_AS(exec.runOne(), std::runtime_error);
    CHECK(exec.pending() == 0);
}

TEST_CASE("StepExecutor is usable through the IExecutor interface", "[ladder][testkit][executor]") {
    // How production code sees it: a model or App holding a
    // shared_ptr<IExecutor> substitutes this for its ThreadPoolExecutor.
    StepExecutor exec;
    ::morph::exec::IExecutor& iface = exec;
    bool ran = false;
    iface.post([&] { ran = true; });
    CHECK(exec.pending() == 1);
    CHECK_FALSE(ran);
    REQUIRE(exec.runOne());
    CHECK(ran);
}
