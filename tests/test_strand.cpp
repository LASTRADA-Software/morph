// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <core/async/DetachedTask.hpp>
#include <core/async/ExecutorContext.hpp>
#include <core/async/ParkedWork.hpp>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <morph/core/detail/task_handler.hpp>
#include <morph/core/strand.hpp>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "test_support.hpp"

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
        return std::ranges::any_of(_messages,
                                   [needle](const std::string& message) { return message.contains(needle); });
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

Probe record(const ModelStrands* strands, ModelId key, const TaskResumer* resumer, Seen* seen) {
    seen->principal = morph::session::current() != nullptr ? morph::session::current()->principal : "";
    seen->onStrand = strands->runningHere(key);
    seen->resumerCurrent = core::async::currentExecutor() == resumer;
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
    strands->post(key, [&] { afterRan.store(true); });
    strands->drain();

    REQUIRE(afterRan.load());
    CHECK(log.contains("[strand] task threw: strand bomb"));
}

TEST_CASE("ModelStrands: a non-std::exception throw is swallowed and the next task still runs", "[strand]") {
    CapturedLog log;
    morph::exec::ThreadPoolExecutor pool{2};
    auto const strands = std::make_shared<ModelStrands>(pool);
    ModelId const key{43};

    std::atomic<bool> afterRan{false};
    strands->post(key, [] { throw 7; });  // NOLINT(hicpp-exception-baseclass) — exercises the catch(...) arm
    strands->post(key, [&] { afterRan.store(true); });
    strands->drain();

    REQUIRE(afterRan.load());
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
        Probe const probe = record(strands.get(), key, resumer.get(), &seen);
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

        // A withdraw naming another resumer leaves the enrolment alone.
        auto const other = std::make_shared<TaskResumer>(strands, key, morph::session::Context{});
        strands->withdraw(key, other.get());
        strands->post(key, [&] { resumerCurrent = core::async::currentExecutor() == resumer.get(); });
        strands->drain();
        CHECK(resumerCurrent);

        strands->withdraw(key, resumer.get());
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
        Probe const probe = record(strands.get(), key, resumer.get(), &seen);
        resumer->submit(probe.handle);
        CHECK(probe.handle.done());
        CHECK(seen.principal == "alice");
        CHECK_FALSE(seen.onStrand);
        CHECK(seen.resumerCurrent);
    }

    strands->withdraw(key, resumer.get());
}

namespace {

/// Parks the awaiting coroutine and hands its `ParkedWork` -- with the claim a
/// `DetachedTask` chain carries -- to the test, as `AsyncQueue::pop` hands it
/// to the executor it parked on.
struct ParkInto {
    core::async::ParkedWork* out;
    [[nodiscard]] bool await_ready() const noexcept { return false; }
    template <typename Promise>
    void await_suspend(std::coroutine_handle<Promise> awaiting) const {
        *out = core::async::detail::parkedWorkFor(awaiting);
    }
    void await_resume() const noexcept {}
};

core::async::DetachedTask parkOnce(core::async::ParkedWork* parked, std::atomic<bool>* finished) {
    co_await ParkInto{parked};
    finished->store(true);
}

}  // namespace

TEST_CASE("TaskResumer keeps a detached chain's claim until it resumes it, on the strand or inline",
          "[strand][coroutine]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto const strands = std::make_shared<ModelStrands>(pool);
    ModelId const key{13};
    auto const resumer = std::make_shared<TaskResumer>(strands, key, morph::session::Context{});

    SECTION("on the strand") {
        core::async::ParkedWork parked;
        std::atomic<bool> finished{false};
        parkOnce(&parked, &finished);
        REQUIRE(parked.abandon.armed());
        // The claim travels with the handle: dropping it here would free the
        // frame while its handle is queued.
        resumer->submit(std::move(parked));
        strands->drain();
        CHECK(finished.load());
    }

    SECTION("inline, once the strands are closed") {
        strands->close();
        core::async::ParkedWork parked;
        std::atomic<bool> finished{false};
        parkOnce(&parked, &finished);
        REQUIRE(parked.abandon.armed());
        resumer->submit(std::move(parked));
        CHECK(finished.load());
    }
}

TEST_CASE("ModelStrands teardown: a resumption or a handler's end arriving after the drain runs inline",
          "[strand][coroutine][lifetime]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto const strands = std::make_shared<ModelStrands>(pool);
    ModelId const key{21};
    auto const resumer = std::make_shared<TaskResumer>(strands, key, morph::session::Context{});

    // Teardown's steps, stopped between the drain and the close: work that
    // arrives here and is queued would be dropped by the close.
    strands->seal();
    strands->drain();

    Seen seen;
    Probe const probe = record(strands.get(), key, resumer.get(), &seen);
    resumer->submit(probe.handle);
    CHECK(probe.handle.done());
    CHECK_FALSE(seen.onStrand);
    CHECK(seen.resumerCurrent);

    auto const caller = std::this_thread::get_id();
    std::thread::id finishedOn;
    strands->runOnStrand(key, [&] { finishedOn = std::this_thread::get_id(); });
    CHECK(finishedOn == caller);

    strands->close();
}

TEST_CASE("ModelStrands teardown: after the seal, queued work runs and a handler's end is refused and runs inline",
          "[strand][lifetime]") {
    morph::exec::MainThreadExecutor exec;
    auto const strands = std::make_shared<ModelStrands>(exec);
    ModelId const key{22};
    bool ran = false;
    strands->post(key, [&] { ran = true; });
    strands->seal();
    // runOnStrand's post is a try-form, which the seal refuses: the end runs
    // here, before anything is pumped, rather than behind the queued work.
    bool endedInline = false;
    strands->runOnStrand(key, [&] { endedInline = !ran; });
    CHECK(endedInline);
    exec.drain();
    CHECK(ran);
    strands->close();
}

TEST_CASE("ModelStrands teardown, sealing first: a handler stopped with nothing to run its strand unwinds inline",
          "[strand][coroutine][lifetime]") {
    // The single-threaded build's situation on a native one: the executor is
    // pumped by the thread that tears the strands down, which is busy doing so.
    morph::exec::MainThreadExecutor exec;
    auto const strands = std::make_shared<ModelStrands>(exec);
    ModelId const key{23};
    auto const resumer = std::make_shared<TaskResumer>(strands, key, morph::session::Context{});
    Seen seen;
    Probe const probe = record(strands.get(), key, resumer.get(), &seen);

    // The stop resumes the suspended handler through its resumer, as a
    // stop-aware awaiter does. Bounded: were the resumption queued on the
    // executor nobody pumps, the drain would wait for it for ever, so the test
    // pumps it itself after the bound and fails.
    std::atomic<bool> tornDown{false};
    std::thread teardown{[&] {
        strands->teardown([&] { resumer->submit(probe.handle); }, morph::exec::detail::TeardownOrder::SealThenStop);
        tornDown = true;
    }};
    bool const inTime = morph::testing::waitUntil([&] { return tornDown.load(); });
    while (!tornDown.load()) {
        exec.runOnce();
    }
    teardown.join();
    CHECK(inTime);
    CHECK(probe.handle.done());
    CHECK_FALSE(seen.onStrand);
    CHECK(seen.resumerCurrent);
}

TEST_CASE("ModelStrands teardown: a handler's end arriving after the seal never overlaps its key's queued gate entry",
          "[strand][coroutine][lifetime]") {
    // A Task handler holds its model's action gate, parked on another
    // executor -- a core::net socket's loop -- and a failed call's task is
    // queued behind it on the same strand, where it tries the gate. The
    // teardown stops the handler, whose end arrives from the loop thread once
    // the strands are sealed, so it is refused and runs inline there. Were the
    // failed call's task still running on a pool thread then, two threads
    // would be inside one gate: the overlap counter counts that, and TSan
    // reports the race on the gate's state. The teardown drains before it
    // seals, so the task has finished by then.
    using morph::model::detail::ActionGate;
    ActionGate gate;
    morph::exec::ThreadPoolExecutor pool{2};
    auto const strands = std::make_shared<ModelStrands>(pool);
    ModelId const key{24};
    ModelId const probeKey{25};
    auto const overlapsBefore = ActionGate::overlapsObserved();

    std::atomic<bool> nextStarted{false};
    std::atomic<bool> nextSawFailedCall{false};
    std::atomic<bool> failedCallDone{false};
    std::atomic<bool> handlerHolds{false};
    strands->post(key, [&] {
        handlerHolds = gate.tryEnter();
        // The action queued behind the handler, which the handler's leave()
        // starts. It stays inside that leave() until the failed call is done.
        gate.enter([&] {
            nextStarted = true;
            nextSawFailedCall = morph::testing::waitUntil([&] { return failedCallDone.load(); });
        });
    });
    std::atomic<bool> failedCallStarted{false};
    strands->post(key, [&] {
        failedCallStarted = true;
        // Holds the task open for as long as the handler's end may take to
        // arrive: through it, when the end runs beside the task; to the budget,
        // when the end is queued behind it.
        (void)morph::testing::waitUntil([&] { return nextStarted.load(); },
                                        morph::testing::WaitBudget{std::chrono::milliseconds{500}});
        if (!gate.tryEnter()) {
            gate.enter([] {});
        }
        failedCallDone = true;
    });
    REQUIRE(morph::testing::waitUntil([&] { return failedCallStarted.load(); }));
    REQUIRE(handlerHolds.load());

    // The loop thread: once the strands are sealed, the handler's end.
    // A probe's runOnStrand runs inline on the loop thread only once the seal
    // refuses its post. One probe is out at a time, at a slow cadence, so the
    // drain before the seal finds the strands idle between probes instead of
    // racing a post per poll.
    std::atomic<bool> sealSeen{false};
    std::atomic<bool> probeOut{false};
    std::thread loop;
    strands->teardown([&] {
        loop = std::thread{[&] {
            auto const self = std::this_thread::get_id();
            (void)morph::testing::waitUntil(
                [&] {
                    if (!probeOut.exchange(true)) {
                        strands->runOnStrand(probeKey, [&] {
                            if (std::this_thread::get_id() == self) {
                                sealSeen = true;
                            }
                            probeOut = false;
                        });
                    }
                    return sealSeen.load();
                },
                morph::testing::WaitBudget{std::chrono::milliseconds{5000}},
                morph::testing::WaitStep{std::chrono::milliseconds{10}});
            strands->runOnStrand(key, [&] { gate.leave(); });
        }};
    });
    loop.join();

    CHECK(sealSeen.load());
    CHECK(nextStarted.load());
    CHECK(nextSawFailedCall.load());
    CHECK(failedCallDone.load());
    CHECK(ActionGate::overlapsObserved() == overlapsBefore);
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
