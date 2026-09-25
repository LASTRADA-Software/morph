// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <core/async/ExecutorContext.hpp>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <morph/core/strand.hpp>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// morph's strands are core-cpp's `KeyedStrands`, whose ordering and
// one-strand-per-key invariants core-cpp tests (`Strand_test.cpp`,
// `KeyedStrands_test.cpp`). What these cases pin is what `ModelStrands` adds:
// the base adapter, a posted task's throw logged rather than propagated,
// `runOnStrand`, `drain`, and the Task handler's resumer with the session.

namespace {

using morph::exec::detail::ModelId;
using morph::exec::detail::ModelStrands;
using morph::exec::detail::TaskResumer;

/// Collects every message logged while it lives.
class CapturedLog {
public:
    CapturedLog()
        : _override{[this](morph::log::LogLevel /*level*/, std::string_view message) {
              std::scoped_lock const lock{_mtx};
              _messages.emplace_back(message);
          }} {}

    [[nodiscard]] bool contains(std::string_view needle) {
        std::scoped_lock const lock{_mtx};
        for (auto const& message : _messages) {
            if (message.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

private:
    std::mutex _mtx;
    std::vector<std::string> _messages;
    morph::log::ScopedLoggerOverride _override;
};

/// A coroutine that only records where each of its resumptions ran.
struct Probe {
    struct promise_type {
        Probe get_return_object() { return Probe{std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };

    explicit Probe(std::coroutine_handle<promise_type> frame) : handle{frame} {}
    Probe(const Probe&) = delete;
    Probe& operator=(const Probe&) = delete;
    Probe(Probe&&) = delete;
    Probe& operator=(Probe&&) = delete;
    ~Probe() { handle.destroy(); }

    std::coroutine_handle<promise_type> handle;
};

struct Seen {
    std::string principal;
    bool onStrand = false;
    bool resumerCurrent = false;
};

Probe record(const std::shared_ptr<ModelStrands>& strands, ModelId key, const TaskResumer* resumer, Seen& seen) {
    seen.principal = morph::session::current() != nullptr ? morph::session::current()->principal : "";
    seen.onStrand = strands->runningHere(key);
    seen.resumerCurrent = core::async::currentExecutor() == resumer;
    co_return;
}

}  // namespace

TEST_CASE("ModelStrands serialises tasks for the same key, in order", "[strand]") {
    morph::exec::ThreadPoolExecutor pool{4};
    auto const strands = std::make_shared<ModelStrands>(pool);
    ModelId const key{1};

    std::atomic<int> concurrent{0};
    std::atomic<int> maxConcurrent{0};
    std::vector<int> order;
    constexpr int numTasks = 50;

    for (int i = 0; i < numTasks; ++i) {
        strands->post(key, [&, i] {
            int const cnt = concurrent.fetch_add(1) + 1;
            int prev = maxConcurrent.load();
            while (cnt > prev && !maxConcurrent.compare_exchange_weak(prev, cnt)) {
            }
            // Touched only on the key's strand, so it needs no lock.
            order.push_back(i);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            concurrent.fetch_sub(1);
        });
    }
    // The drain is the wait: every queued task has run when it returns.
    strands->drain();

    REQUIRE(maxConcurrent.load() == 1);
    REQUIRE(order.size() == static_cast<std::size_t>(numTasks));
    for (int i = 0; i < numTasks; ++i) {
        CHECK(order[static_cast<std::size_t>(i)] == i);
    }
}

TEST_CASE("ModelStrands runs tasks for different keys concurrently", "[strand]") {
    morph::exec::ThreadPoolExecutor pool{4};
    auto const strands = std::make_shared<ModelStrands>(pool);

    std::atomic<int> concurrent{0};
    std::atomic<int> maxConcurrent{0};
    std::atomic<int> completed{0};
    constexpr int numKeys = 4;

    for (int i = 0; i < numKeys; ++i) {
        strands->post(ModelId{static_cast<uint64_t>(i + 1)}, [&] {
            int const cnt = concurrent.fetch_add(1) + 1;
            int prev = maxConcurrent.load();
            while (cnt > prev && !maxConcurrent.compare_exchange_weak(prev, cnt)) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            concurrent.fetch_sub(1);
            completed.fetch_add(1);
        });
    }
    strands->drain();

    REQUIRE(completed.load() == numKeys);
    REQUIRE(maxConcurrent.load() > 1);
}

TEST_CASE("ModelStrands: independent keys each run exactly their own tasks", "[strand]") {
    morph::exec::ThreadPoolExecutor pool{4};
    auto const strands = std::make_shared<ModelStrands>(pool);

    constexpr std::size_t numKeys = 3;
    constexpr int tasksPerKey = 5;
    std::array<std::atomic<int>, numKeys> results{};

    for (std::size_t key = 0; key < numKeys; ++key) {
        for (int task = 0; task < tasksPerKey; ++task) {
            strands->post(ModelId{key + 1}, [&results, key] { results.at(key).fetch_add(1); });
        }
    }
    strands->drain();

    for (auto const& result : results) {
        REQUIRE(result.load() == tasksPerKey);
    }
    REQUIRE(strands->idle());
}

TEST_CASE("ModelStrands: a task's throw is logged and the next task still runs", "[strand]") {
    CapturedLog log;
    morph::exec::ThreadPoolExecutor pool{2};
    auto const strands = std::make_shared<ModelStrands>(pool);
    ModelId const key{42};

    std::atomic<bool> afterRan{false};
    strands->post(key, [] { throw std::runtime_error("strand bomb"); });
    strands->post(key, [] { throw 7; });  // NOLINT(hicpp-exception-baseclass) — exercises the catch(...) arm
    strands->post(key, [&] { afterRan.store(true); });
    strands->drain();

    REQUIRE(afterRan.load());
    CHECK(log.contains("[strand] task threw: strand bomb"));
    CHECK(log.contains("[strand] task threw unknown exception"));
}

TEST_CASE("ModelStrands::runOnStrand runs inline on the strand, posts off it, and runs inline once closed",
          "[strand]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto const strands = std::make_shared<ModelStrands>(pool);
    ModelId const key{5};
    auto const caller = std::this_thread::get_id();

    std::thread::id offStrand;
    bool offStrandWasOnStrand = false;
    strands->runOnStrand(key, [&] {
        offStrand = std::this_thread::get_id();
        offStrandWasOnStrand = strands->runningHere(key);
    });
    strands->drain();
    CHECK(offStrand != caller);
    CHECK(offStrandWasOnStrand);

    bool nestedInline = false;
    strands->post(key, [&] {
        bool ranBeforeReturn = false;
        strands->runOnStrand(key, [&] { ranBeforeReturn = true; });
        nestedInline = ranBeforeReturn;
    });
    strands->drain();
    CHECK(nestedInline);

    strands->close();
    std::thread::id afterClose;
    strands->runOnStrand(key, [&] { afterClose = std::this_thread::get_id(); });
    CHECK(afterClose == caller);

    bool posted = false;
    strands->post(key, [&] { posted = true; });
    CHECK_FALSE(posted);
}

TEST_CASE("ModelStrands: runningHere and runningAnyHere answer inside a task only", "[strand]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto const strands = std::make_shared<ModelStrands>(pool);
    ModelId const key{8};
    ModelId const other{9};

    bool here = false;
    bool otherHere = true;
    bool anyHere = false;
    strands->post(key, [&] {
        here = strands->runningHere(key);
        otherHere = strands->runningHere(other);
        anyHere = strands->runningAnyHere();
    });
    strands->drain();

    CHECK(here);
    CHECK_FALSE(otherHere);
    CHECK(anyHere);
    CHECK_FALSE(strands->runningAnyHere());
}

TEST_CASE("TaskResumer resumes on its key's strand with the session installed, and inline once closed",
          "[strand][coroutine]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto const strands = std::make_shared<ModelStrands>(pool);
    ModelId const key{11};
    morph::session::Context session;
    session.principal = "alice";
    auto const resumer = std::make_shared<TaskResumer>(strands, key, session);
    strands->enroll(key, resumer);

    SECTION("a submit through the strand, as an awaitable that parked there does") {
        Seen seen;
        Probe probe = record(strands, key, resumer.get(), seen);
        resumer->submit(probe.handle);
        strands->drain();
        CHECK(probe.handle.done());
        CHECK(seen.principal == "alice");
        CHECK(seen.onStrand);
        CHECK(seen.resumerCurrent);
    }

    SECTION("an ordinary task of an enrolled key runs inside the resumer too, until it is withdrawn") {
        std::string principal;
        bool resumerCurrent = false;
        strands->post(key, [&] {
            principal = morph::session::current() != nullptr ? morph::session::current()->principal : "";
            resumerCurrent = core::async::currentExecutor() == resumer.get();
        });
        strands->drain();
        CHECK(principal == "alice");
        CHECK(resumerCurrent);

        strands->withdraw(key);
        strands->post(key, [&] {
            principal = morph::session::current() != nullptr ? morph::session::current()->principal : "<none>";
            resumerCurrent = core::async::currentExecutor() == resumer.get();
        });
        strands->drain();
        CHECK(principal == "<none>");
        CHECK_FALSE(resumerCurrent);
    }

    SECTION("once closed, a submit resumes inline, still inside the resumer") {
        strands->close();
        Seen seen;
        Probe probe = record(strands, key, resumer.get(), seen);
        resumer->submit(probe.handle);
        CHECK(probe.handle.done());
        CHECK(seen.principal == "alice");
        CHECK_FALSE(seen.onStrand);
        CHECK(seen.resumerCurrent);
    }

    strands->withdraw(key);
}

// Regression test for ThreadPoolExecutor(0): a zero-worker pool used to accept
// tasks that could never run, hanging every post() forever. The constructor now
// clamps the worker count to at least 1, so a pool built with 0 is still usable.
TEST_CASE("ThreadPoolExecutor(0) yields a usable pool", "[executor]") {
    std::atomic<bool> ran{false};

    // Scoped so ~ThreadPoolExecutor's own drain-before-join (executor.hpp's own
    // doc comment on it) is the wait, not a fixed-iteration poll: the posted
    // task is queued before this block ends, so the destructor's join is
    // guaranteed not to return until it has run.
    {
        morph::exec::ThreadPoolExecutor pool{0};
        pool.post([&] { ran.store(true); });
    }

    REQUIRE(ran.load());
}
