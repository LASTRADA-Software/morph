// SPDX-License-Identifier: Apache-2.0
//
// The model side of docs/spec/core/coroutines.md: an action handler returning
// core::async::Task<R>, driven on its model's strand by LocalBackend and by
// RemoteServer, one action at a time.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <core/async/AsyncQueue.hpp>
#include <core/async/Cancellation.hpp>
#include <core/async/IExecutor.hpp>
#include <core/async/ResumeOn.hpp>
#include <core/async/Task.hpp>
#include <core/async/ThreadPoolExecutor.hpp>
#include <coroutine>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <future>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/coroutine.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/journal/action_log.hpp>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "test_support.hpp"

using namespace std::chrono_literals;

// A named namespace, not an anonymous one: ActionDispatcher::registerAction
// files each action's schema, and glaze's reflection cannot name a type with
// internal linkage under MSVC (C7631).
namespace coro_test {

/// @return Whether @p error holds an `Error`.
template <typename Error>
[[nodiscard]] bool holds(const std::exception_ptr& error) {
    try {
        std::rethrow_exception(error);
    } catch (const Error&) {
        return true;
    } catch (...) {
        return false;
    }
}

template <typename Pred>
[[nodiscard]] bool pumpUntil(morph::exec::MainThreadExecutor& exec, Pred pred) {
    auto const deadline = std::chrono::steady_clock::now() + morph::testing::kDefaultWaitBudget;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        exec.runFor(morph::testing::kDefaultWaitStep);
    }
    return true;
}

// ── A model with an ordinary handler, for a Task handler to await ────────────
struct CoroLookup {
    int x = 0;
};

struct CoroSourceModel {
    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    int execute(const CoroLookup& lookup) { return lookup.x * 10; }
};

// ── The model under test: every handler is a coroutine ───────────────────────
struct CoroModel;
struct CoroDouble {
    int x = 0;
};
struct CoroAwaitOther {
    int x = 0;
};
struct CoroHold {
    int tag = 0;
};
struct CoroLog {
    int tag = 0;
};
struct CoroThrow {
    bool fail = true;
};
struct CoroSleep {
    int ms = 0;
};
struct CoroTick {
    int ms = 0;
};
struct CoroForeign {
    int tag = 0;
};
struct CoroPop {
    int tag = 0;
};
struct CoroPopBack {
    int tag = 0;
};
/// A Task handler's action with a validator: only a positive `x` is ready.
struct CoroValidated {
    int x = 0;
    [[nodiscard]] bool validate() const { return x > 0; }
};
/// A Task handler that completes without suspending.
struct CoroQuick {
    int x = 0;
};

}  // namespace coro_test

using coro_test::CoroAwaitOther;
using coro_test::CoroDouble;
using coro_test::CoroForeign;
using coro_test::CoroHold;
using coro_test::CoroLog;
using coro_test::CoroLookup;
using coro_test::CoroModel;
using coro_test::CoroPop;
using coro_test::CoroPopBack;
using coro_test::CoroQuick;
using coro_test::CoroSleep;
using coro_test::CoroSourceModel;
using coro_test::CoroThrow;
using coro_test::CoroTick;
using coro_test::CoroValidated;

// Hand-written traits, as the other bridge tests use: the JSON codecs only
// matter on the remote path, and there only for ints.
// A macro because an explicit specialisation cannot be produced by a template.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define CORO_INT_ACTION(ACTION, NAME)                                                             \
    template <>                                                                                   \
    struct morph::model::ActionTraits<ACTION> {                                                   \
        using Result = int;                                                                       \
        static constexpr std::string_view typeId() { return NAME; }                               \
        static std::string toJson(const ACTION&) { return "{}"; }                                 \
        static ACTION fromJson(std::string_view) { return {}; }                                   \
        static std::string resultToJson(const int& result) { return std::to_string(result); }     \
        static int resultFromJson(std::string_view json) { return std::stoi(std::string{json}); } \
    };

CORO_INT_ACTION(CoroLookup, "Coro_Lookup")
CORO_INT_ACTION(CoroDouble, "Coro_Double")
CORO_INT_ACTION(CoroAwaitOther, "Coro_AwaitOther")
CORO_INT_ACTION(CoroHold, "Coro_Hold")
CORO_INT_ACTION(CoroLog, "Coro_Log")
CORO_INT_ACTION(CoroThrow, "Coro_Throw")
CORO_INT_ACTION(CoroSleep, "Coro_Sleep")
CORO_INT_ACTION(CoroTick, "Coro_Tick")
CORO_INT_ACTION(CoroForeign, "Coro_Foreign")
CORO_INT_ACTION(CoroPop, "Coro_Pop")
CORO_INT_ACTION(CoroPopBack, "Coro_PopBack")
CORO_INT_ACTION(CoroValidated, "Coro_Validated")
CORO_INT_ACTION(CoroQuick, "Coro_Quick")
#undef CORO_INT_ACTION

template <>
struct morph::model::ModelTraits<CoroSourceModel> {
    static constexpr std::string_view typeId() { return "Coro_SourceModel"; }
};
template <>
struct morph::model::ModelTraits<CoroModel> {
    static constexpr std::string_view typeId() { return "Coro_Model"; }
};

namespace coro_test {

/// What the handlers report back, shared with the test body. The model is
/// default-constructed by the bridge, so it reaches this through a global.
struct CoroProbe {
    std::mutex mtx;
    std::vector<std::string> order;
    std::vector<bool> onOwnStrand;
    std::vector<std::thread::id> threads;
    morph::bridge::BridgeHandler<CoroSourceModel>* source = nullptr;
    std::optional<morph::async::Completion<int>::Promise> release;
    std::optional<morph::async::Completion<int>> held;
    std::atomic<bool> holdCancelled{false};
    std::atomic<bool> holdFinished{false};
    std::atomic<int> ticks{0};
    /// A core-cpp queue whose consumer resumes on the queue's own executor.
    std::unique_ptr<core::async::AsyncQueue<int>> queue;
    std::atomic<std::thread::id> popThread;
    std::atomic<std::thread::id> logThread;

    void note(std::string entry) {
        std::scoped_lock const lock{mtx};
        order.push_back(std::move(entry));
    }
    void noteStrand(bool own) {
        std::scoped_lock const lock{mtx};
        onOwnStrand.push_back(own);
        threads.push_back(std::this_thread::get_id());
    }
};

/// A coroutine type from outside core::async: its promise carries no stop
/// token, so a morph awaiter inside it cannot observe a stop.
struct Unstoppable {
    struct promise_type {
        std::coroutine_handle<> continuation;

        Unstoppable get_return_object() noexcept {
            return Unstoppable{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        [[nodiscard]] std::suspend_always initial_suspend() const noexcept { return {}; }
        struct Final {
            [[nodiscard]] bool await_ready() const noexcept { return false; }
            [[nodiscard]] std::coroutine_handle<> await_suspend(
                std::coroutine_handle<promise_type> self) const noexcept {
                return self.promise().continuation;
            }
            void await_resume() const noexcept {}
        };
        [[nodiscard]] Final final_suspend() const noexcept { return {}; }
        void return_void() const noexcept {}
        [[noreturn]] void unhandled_exception() const noexcept { std::terminate(); }
    };

    explicit Unstoppable(std::coroutine_handle<promise_type> handle) noexcept : _handle{handle} {}
    Unstoppable(const Unstoppable&) = delete;
    Unstoppable& operator=(const Unstoppable&) = delete;
    Unstoppable(Unstoppable&& other) noexcept : _handle{std::exchange(other._handle, {})} {}
    Unstoppable& operator=(Unstoppable&&) = delete;
    ~Unstoppable() {
        if (_handle) {
            _handle.destroy();
        }
    }

    [[nodiscard]] bool await_ready() const noexcept { return false; }
    [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) const noexcept {
        _handle.promise().continuation = continuation;
        return _handle;
    }
    void await_resume() const noexcept {}

private:
    std::coroutine_handle<promise_type> _handle;
};

namespace {

CoroProbe& probe() {
    static CoroProbe instance;
    return instance;
}

}  // namespace

namespace {

/// The resumption context a Task handler runs in: its model's strand executor.
::core::async::IExecutor* strandContext() {
    auto* context = morph::async::detail::currentResumeContext();
    return dynamic_cast<morph::exec::StrandCoroExecutor*>(context) != nullptr ? context : nullptr;
}

}  // namespace

struct CoroModel {
    // NOLINTBEGIN(readability-convert-member-functions-to-static)
    core::async::Task<int> execute(CoroDouble action) {
        auto* const strand = strandContext();
        probe().noteStrand(strand != nullptr);
        co_await morph::async::delay(scheduler(), 5ms);
        probe().noteStrand(strand != nullptr && morph::async::detail::currentResumeContext() == strand);
        co_return action.x * 2;
    }

    core::async::Task<int> execute(CoroAwaitOther action) {
        auto* const strand = strandContext();
        probe().noteStrand(strand != nullptr);
        auto pending = probe().source->execute(CoroLookup{.x = action.x});
        int const looked = co_await std::move(pending);
        // The other model's completion is delivered on its handler's executor;
        // this handler must still resume on its own strand.
        probe().noteStrand(strand != nullptr && morph::async::detail::currentResumeContext() == strand);
        co_return looked + 1;
    }

    core::async::Task<int> execute(CoroHold action) {
        probe().note("hold-start");
        try {
            auto pending = std::move(*probe().held);
            int const value = co_await std::move(pending);
            probe().note("hold-end");
            probe().holdFinished = true;
            co_return value + action.tag;
        } catch (const core::async::OperationCancelled&) {
            probe().holdCancelled = true;
            probe().holdFinished = true;
            throw;
        }
    }

    core::async::Task<int> execute(CoroValidated action) { co_return action.x; }

    core::async::Task<int> execute(CoroQuick action) { co_return action.x + 1; }

    int execute(const CoroLog& action) {
        probe().logThread = std::this_thread::get_id();
        probe().note("log-" + std::to_string(action.tag));
        return action.tag;
    }

    // Awaits a core-cpp awaiter, then goes back to its strand the documented way.
    core::async::Task<int> execute(CoroPopBack action) {
        auto* const strand = morph::async::resumeContext();
        probe().note("popback-start");
        auto const item = co_await probe().queue->pop();
        probe().noteStrand(strand != nullptr && morph::async::resumeContext() == strand);
        co_await core::async::ResumeOn{*strand};
        probe().noteStrand(morph::async::resumeContext() == strand);
        co_return item.value_or(0) + action.tag;
    }

    // Parks on a core-cpp awaiter, which resumes it on the queue's executor --
    // a stop included -- not through the model's strand.
    core::async::Task<int> execute(CoroPop action) {
        probe().note("pop-start");
        try {
            auto const item = co_await probe().queue->pop();
            probe().note("pop-end");
            co_return item.value_or(0) + action.tag;
        } catch (const core::async::OperationCancelled&) {
            probe().popThread = std::this_thread::get_id();
            probe().note("pop-cancelled");
            probe().holdCancelled = true;
            probe().holdFinished = true;
            throw;
        }
    }

    core::async::Task<int> execute(CoroSleep action) {
        probe().note("sleep-start");
        try {
            co_await morph::async::delay(scheduler(), std::chrono::milliseconds{action.ms});
        } catch (const core::async::OperationCancelled&) {
            probe().note("sleep-cancelled");
            probe().holdCancelled = true;
            probe().holdFinished = true;
            throw;
        }
        probe().note("sleep-end");
        probe().holdFinished = true;
        co_return action.ms;
    }

    // Awaits the held completion from inside a coroutine that cannot see a stop.
    core::async::Task<int> execute(CoroForeign action) {
        probe().note("foreign-start");
        auto inner = [](morph::async::Completion<int> pending) -> Unstoppable {
            int const value = co_await std::move(pending);
            static_cast<void>(value);
        }(std::move(*probe().held));
        co_await std::move(inner);
        probe().note("foreign-end");
        probe().holdFinished = true;
        co_return action.tag;
    }

    // Ticks until it is stopped: a handler that only a stop can end.
    core::async::Task<int> execute(CoroTick action) {
        probe().note("tick-start");
        int ticks = 0;
        try {
            while (ticks < 1'000'000) {
                co_await morph::async::delay(scheduler(), std::chrono::milliseconds{action.ms});
                ++ticks;
                probe().ticks = ticks;
            }
        } catch (const core::async::OperationCancelled&) {
            probe().note("tick-cancelled");
            probe().holdCancelled = true;
            probe().holdFinished = true;
            throw;
        }
        co_return ticks;
    }

    core::async::Task<int> execute(CoroThrow action) {
        co_await morph::async::delay(scheduler(), 1ms);
        // Conditional, so the coroutine still ends in a co_return: MSVC
        // reports one that ends in a throw as returning no value (C4033).
        if (action.fail) {
            throw std::runtime_error{"handler failed after a suspension"};
        }
        co_return 0;
    }
    // NOLINTEND(readability-convert-member-functions-to-static)

    static morph::async::detail::TimeoutScheduler& scheduler();
};

/// Owns the scheduler CoroModel's handlers delay on, for one test, and destroys
/// it -- joining its thread -- when the test ends. It is not a function-local
/// static: one that outlived its test kept its thread for the rest of the run,
/// and every later fork() child (the registration-phase tests fork) inherited
/// the thread's state without the thread, which Valgrind reports as definitely
/// lost and turns into the child's exit status.
class SchedulerScope {
public:
    SchedulerScope() { current = &_scheduler; }
    SchedulerScope(const SchedulerScope&) = delete;
    SchedulerScope& operator=(const SchedulerScope&) = delete;
    SchedulerScope(SchedulerScope&&) = delete;
    SchedulerScope& operator=(SchedulerScope&&) = delete;
    ~SchedulerScope() { current = nullptr; }

    static inline morph::async::detail::TimeoutScheduler* current = nullptr;

private:
    morph::async::detail::TimeoutScheduler _scheduler;
};

morph::async::detail::TimeoutScheduler& CoroModel::scheduler() {
    if (SchedulerScope::current == nullptr) {
        throw std::logic_error{"a CoroModel handler delayed in a test that declares no SchedulerScope"};
    }
    return *SchedulerScope::current;
}

namespace {

/// Arms `probe().held` with a completion the test settles through `release`.
void armHold(morph::exec::IExecutor* exec) {
    auto [completion, promise] = morph::async::Completion<int>::makeSettleable(exec);
    probe().held.emplace(std::move(completion));
    probe().release.emplace(std::move(promise));
    probe().holdCancelled = false;
    probe().holdFinished = false;
    probe().ticks = 0;
    probe().popThread = std::thread::id{};
    probe().logThread = std::thread::id{};
    std::scoped_lock const lock{probe().mtx};
    probe().order.clear();
    probe().onOwnStrand.clear();
    probe().threads.clear();
}

}  // namespace

}  // namespace coro_test

using coro_test::armHold;
using coro_test::holds;
using coro_test::probe;
using coro_test::pumpUntil;

static_assert(morph::model::isTaskHandler<decltype(std::declval<CoroModel&>().execute(CoroDouble{}))>);
static_assert(!morph::model::isTaskHandler<decltype(std::declval<CoroModel&>().execute(CoroLog{}))>);
static_assert(std::is_same_v<morph::model::HandlerResultT<core::async::Task<int>>, int>);

TEST_CASE("a Task handler's result reaches the client, and every resumption runs on the model's strand",
          "[coroutine][model]") {
    coro_test::SchedulerScope const timers;
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor exec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CoroModel> handler{bridge, &exec};
    armHold(&exec);

    std::optional<int> result;
    handler.execute(CoroDouble{.x = 21})
        .then([&](int value) { result = value; })
        .onError([](const std::exception_ptr&) {});

    REQUIRE(pumpUntil(exec, [&] { return result.has_value(); }));
    REQUIRE(*result == 42);

    // The delay fires on the scheduler's thread; the step after it must not
    // run there, nor on this one, which only delivers the result.
    std::atomic<std::thread::id> timerThread{};
    std::atomic<bool> timerRan{false};
    CoroModel::scheduler().schedule(0ms, [&] {
        timerThread = std::this_thread::get_id();
        timerRan = true;
    });
    REQUIRE(pumpUntil(exec, [&] { return timerRan.load(); }));

    std::scoped_lock const lock{probe().mtx};
    REQUIRE(probe().onOwnStrand == std::vector<bool>{true, true});
    REQUIRE(probe().threads.size() == 2);
    REQUIRE(probe().threads[1] != timerThread.load());
    REQUIRE(probe().threads[1] != std::this_thread::get_id());
}

namespace coro_test {

/// A step of a coroutine that hops through @p executor before running.
struct Hop {
    ::core::async::IExecutor* executor;
    [[nodiscard]] constexpr bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> awaiting) const { executor->submit(awaiting); }
    void await_resume() const noexcept {}
};

/// Counts how many tasks are inside a critical section at once.
struct Overlap {
    std::atomic<int> active{0};
    std::atomic<int> overlaps{0};

    /// Holds the section for 200 us. It spins rather than sleeps: Windows rounds
    /// a sleep up to its timer resolution, up to 15.6 ms, which made the 250
    /// serial steps of the test below outlast the default wait budget on CI.
    void step() {
        if (active.fetch_add(1) != 0) {
            overlaps.fetch_add(1);
        }
        auto const until = std::chrono::steady_clock::now() + std::chrono::microseconds{200};
        while (std::chrono::steady_clock::now() < until) {
            std::this_thread::yield();
        }
        active.fetch_sub(1);
    }
};

namespace {

::core::async::Task<void> hopSteps(::core::async::IExecutor* executor, Overlap* overlap, int count,
                                   std::atomic<bool>* done) {
    for (int step = 0; step < count; ++step) {
        co_await Hop{executor};
        overlap->step();
    }
    *done = true;
}

}  // namespace

}  // namespace coro_test

TEST_CASE("StrandCoroExecutor resumes a coroutine as one of its strand's tasks, never beside them",
          "[coroutine][model][strand]") {
    morph::exec::ThreadPoolExecutor pool{4};
    morph::exec::detail::StrandExecutor strand{pool};
    auto link = std::make_shared<morph::exec::detail::StrandLink>(strand);
    morph::exec::detail::ModelId const key{7};
    auto executor = std::make_shared<morph::exec::StrandCoroExecutor>(link, key, morph::session::Context{});

    coro_test::Overlap overlap;
    std::atomic<bool> done{false};
    std::atomic<int> posted{0};
    morph::async::spawn(pool, coro_test::hopSteps(executor.get(), &overlap, 50, &done));
    // Other tasks on the same strand, interleaved with the coroutine's steps
    // on a pool of four: a step resumed anywhere but on the strand overlaps
    // one of them.
    for (int task = 0; task < 200; ++task) {
        REQUIRE(link->post(key, [&] {
            overlap.step();
            posted.fetch_add(1);
        }));
    }

    REQUIRE(morph::testing::waitUntil([&] { return done.load() && posted.load() == 200; }));
    REQUIRE(overlap.overlaps.load() == 0);
    link->close();
}

TEST_CASE("a Task handler can await another model's execute and comes back to its own strand", "[coroutine][model]") {
    coro_test::SchedulerScope const timers;
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::ThreadPoolExecutor otherCallbacks{1};
    morph::exec::MainThreadExecutor exec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CoroSourceModel> source{bridge, &otherCallbacks};
    morph::bridge::BridgeHandler<CoroModel> handler{bridge, &exec};
    armHold(&exec);
    probe().source = &source;

    std::optional<int> result;
    handler.execute(CoroAwaitOther{.x = 4})
        .then([&](int value) { result = value; })
        .onError([](const std::exception_ptr&) {});

    REQUIRE(pumpUntil(exec, [&] { return result.has_value(); }));
    REQUIRE(*result == 41);
    std::scoped_lock const lock{probe().mtx};
    REQUIRE(probe().onOwnStrand == std::vector<bool>{true, true});
}

TEST_CASE("the next action on a model starts only once a suspended Task handler has completed", "[coroutine][model]") {
    coro_test::SchedulerScope const timers;
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor exec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CoroModel> handler{bridge, &exec};
    armHold(&exec);

    std::optional<int> held;
    std::optional<int> logged;
    handler.execute(CoroHold{.tag = 1}).then([&](int value) { held = value; }).onError([](const std::exception_ptr&) {
    });
    handler.execute(CoroLog{.tag = 2}).then([&](int value) { logged = value; }).onError([](const std::exception_ptr&) {
    });

    // The first handler is suspended on a completion nobody has settled; the
    // second action must still be waiting behind it.
    REQUIRE(pumpUntil(exec, [&] {
        std::scoped_lock const lock{probe().mtx};
        return !probe().order.empty();
    }));
    exec.runFor(50ms);
    {
        std::scoped_lock const lock{probe().mtx};
        REQUIRE(probe().order == std::vector<std::string>{"hold-start"});
    }
    REQUIRE_FALSE(logged.has_value());

    probe().release->resolve(100);

    REQUIRE(pumpUntil(exec, [&] { return held.has_value() && logged.has_value(); }));
    REQUIRE(*held == 101);
    REQUIRE(*logged == 2);
    std::scoped_lock const lock{probe().mtx};
    REQUIRE(probe().order == std::vector<std::string>{"hold-start", "hold-end", "log-2"});
}

TEST_CASE("an execute deadline cancels a suspended Task handler and rejects with ClientTimeoutError",
          "[coroutine][model]") {
    coro_test::SchedulerScope const timers;
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor exec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    bridge.setExecuteDeadline(50ms);
    morph::bridge::BridgeHandler<CoroModel> handler{bridge, &exec};
    armHold(&exec);

    bool timedOut = false;
    handler.execute(CoroHold{.tag = 1}).onError([&](const std::exception_ptr& error) {
        timedOut = holds<morph::backend::ClientTimeoutError>(error);
    });

    REQUIRE(pumpUntil(exec, [&] { return timedOut && probe().holdFinished.load(); }));
    REQUIRE(probe().holdCancelled.load());

    // The gate was released: the next action on the model runs.
    std::optional<int> logged;
    handler.execute(CoroLog{.tag = 3}).then([&](int value) { logged = value; }).onError([](const std::exception_ptr&) {
    });
    REQUIRE(pumpUntil(exec, [&] { return logged.has_value(); }));
}

namespace coro_test {

namespace {

/// Switches @p bridge to a fresh LocalBackend over @p pool on a helper thread,
/// and ends the process with a message if the switch does not return: a
/// `~LocalBackend` stuck draining its strand would otherwise hold the whole run
/// until ctest's timeout, with nothing said about why.
void switchWithin(morph::bridge::Bridge& bridge, morph::exec::IExecutor& pool) {
    std::promise<void> returned;
    auto finished = returned.get_future();
    std::thread switcher{[&] {
        bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool));
        returned.set_value();
    }};
    if (finished.wait_for(morph::testing::kDefaultWaitBudget * 5) != std::future_status::ready) {
        static_cast<void>(std::fputs(
            "switchBackend did not return: the outgoing LocalBackend is stuck draining its strand\n", stderr));
        static_cast<void>(std::fflush(stderr));
        std::abort();
    }
    switcher.join();
}

}  // namespace

/// Records that a call failed because its backend was switched away.
struct SwitchedFlag {
    std::atomic<bool> seen{false};

    auto handler() {
        return [this](const std::exception_ptr& error) {
            if (holds<morph::backend::BackendChangedError>(error)) {
                seen = true;
            }
        };
    }
};

}  // namespace coro_test

// A backend switch fails every pending call and destroys the outgoing
// LocalBackend. Its destructor stops every Task handler it started, lets their
// cancelled resumptions and its queued work drain on the strand, and only then
// closes the strand to them.
TEST_CASE("a backend switch stops a Task handler suspended on a completion", "[coroutine][model][lifetime]") {
    coro_test::SchedulerScope const timers;
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor exec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CoroModel> handler{bridge, &exec};
    armHold(&exec);

    coro_test::SwitchedFlag switched;
    handler.execute(CoroHold{.tag = 1}).then([](int) {}).onError(switched.handler());
    REQUIRE(pumpUntil(exec, [&] {
        std::scoped_lock const lock{probe().mtx};
        return !probe().order.empty();
    }));

    coro_test::switchWithin(bridge, pool);
    REQUIRE(pumpUntil(exec, [&] { return switched.seen.load() && probe().holdFinished.load(); }));
    REQUIRE(probe().holdCancelled.load());

    // The await was withdrawn: settling the completion now reaches nothing.
    probe().release->resolve(100);
    exec.runFor(20ms);
    std::scoped_lock const lock{probe().mtx};
    REQUIRE(probe().order == std::vector<std::string>{"hold-start"});
}

TEST_CASE("a backend switch stops a Task handler suspended on a delay", "[coroutine][model][lifetime]") {
    coro_test::SchedulerScope const timers;
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor exec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CoroModel> handler{bridge, &exec};
    armHold(&exec);

    coro_test::SwitchedFlag switched;
    handler.execute(CoroSleep{.ms = 60'000}).then([](int) {}).onError(switched.handler());
    REQUIRE(pumpUntil(exec, [&] {
        std::scoped_lock const lock{probe().mtx};
        return !probe().order.empty();
    }));

    coro_test::switchWithin(bridge, pool);
    REQUIRE(pumpUntil(exec, [&] { return switched.seen.load() && probe().holdFinished.load(); }));
    std::scoped_lock const lock{probe().mtx};
    REQUIRE(probe().order == std::vector<std::string>{"sleep-start", "sleep-cancelled"});
}

TEST_CASE("a backend switch stops a Task handler that loops on delay", "[coroutine][model][lifetime]") {
    coro_test::SchedulerScope const timers;
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor exec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CoroModel> handler{bridge, &exec};
    armHold(&exec);

    coro_test::SwitchedFlag switched;
    handler.execute(CoroTick{.ms = 2}).then([](int) {}).onError(switched.handler());
    REQUIRE(pumpUntil(exec, [&] { return probe().ticks.load() >= 3; }));

    coro_test::switchWithin(bridge, pool);
    REQUIRE(pumpUntil(exec, [&] { return switched.seen.load() && probe().holdFinished.load(); }));
    REQUIRE(probe().holdCancelled.load());
    // Stopped for good: no tick after the cancellation.
    int const ticksAtStop = probe().ticks.load();
    exec.runFor(30ms);
    REQUIRE(probe().ticks.load() == ticksAtStop);
    std::scoped_lock const lock{probe().mtx};
    REQUIRE(probe().order == std::vector<std::string>{"tick-start", "tick-cancelled"});
}

TEST_CASE("an action queued behind a suspended Task handler does not run once a backend switch failed it",
          "[coroutine][model][lifetime]") {
    coro_test::SchedulerScope const timers;
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor exec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CoroModel> handler{bridge, &exec};
    armHold(&exec);

    auto const overlapsBefore = morph::model::detail::ActionGate::overlapsObserved();
    coro_test::SwitchedFlag holdSwitched;
    coro_test::SwitchedFlag logSwitched;
    handler.execute(CoroHold{.tag = 1}).then([](int) {}).onError(holdSwitched.handler());
    handler.execute(CoroLog{.tag = 2}).then([](int) {}).onError(logSwitched.handler());
    REQUIRE(pumpUntil(exec, [&] {
        std::scoped_lock const lock{probe().mtx};
        return !probe().order.empty();
    }));

    coro_test::switchWithin(bridge, pool);
    REQUIRE(pumpUntil(exec, [&] { return holdSwitched.seen.load() && logSwitched.seen.load(); }));

    // Whatever settles the held completion now, the queued action stays unrun.
    probe().release->resolve(100);
    REQUIRE(pumpUntil(exec, [&] { return probe().holdFinished.load(); }));
    exec.runFor(20ms);
    REQUIRE(morph::model::detail::ActionGate::overlapsObserved() == overlapsBefore);
    std::scoped_lock const lock{probe().mtx};
    REQUIRE(probe().order == std::vector<std::string>{"hold-start"});
}

// The one handler a stop cannot end: it is suspended in an awaitable that
// ignores stops. It outlives the strand, and resumes inline, not into it.
TEST_CASE("a handler that ignores the stop resumes inline once its backend is gone", "[coroutine][model][lifetime]") {
    coro_test::SchedulerScope const timers;
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor exec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CoroModel> handler{bridge, &exec};
    armHold(&exec);
    auto const overlapsBefore = morph::model::detail::ActionGate::overlapsObserved();

    coro_test::SwitchedFlag switched;
    handler.execute(CoroForeign{.tag = 1}).then([](int) {}).onError(switched.handler());
    REQUIRE(pumpUntil(exec, [&] {
        std::scoped_lock const lock{probe().mtx};
        return !probe().order.empty();
    }));

    coro_test::switchWithin(bridge, pool);
    REQUIRE(pumpUntil(exec, [&] { return switched.seen.load(); }));
    REQUIRE_FALSE(probe().holdFinished.load());

    // Settles on `exec`: the resumption runs here, with the strand gone.
    probe().release->resolve(100);
    REQUIRE(pumpUntil(exec, [&] { return probe().holdFinished.load(); }));
    REQUIRE(morph::model::detail::ActionGate::overlapsObserved() == overlapsBefore);
    std::scoped_lock const lock{probe().mtx};
    REQUIRE(probe().order == std::vector<std::string>{"foreign-start", "foreign-end"});
}

// A core-cpp awaiter resumes a stopped handler on its own executor, not the
// strand. The handler finishes there, but what follows -- leaving the gate and
// starting the next action -- must still happen on the strand.
TEST_CASE("the next action starts on the strand after a handler unwound on a core-cpp awaiter's executor",
          "[coroutine][model][foreign]") {
    coro_test::SchedulerScope const timers;
    morph::exec::ThreadPoolExecutor pool{2};
    core::async::ThreadPoolExecutor foreign{1};
    morph::exec::MainThreadExecutor exec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    bridge.setExecuteDeadline(50ms);
    morph::bridge::BridgeHandler<CoroModel> handler{bridge, &exec};
    armHold(&exec);
    probe().queue = std::make_unique<core::async::AsyncQueue<int>>(foreign, core::async::AsyncQueueOptions{});
    auto const overlapsBefore = morph::model::detail::ActionGate::overlapsObserved();

    handler.execute(CoroPop{}).then([](int) {}).onError([](const std::exception_ptr&) {});
    handler.execute(CoroLog{.tag = 5}).then([](int) {}).onError([](const std::exception_ptr&) {});

    // The deadline stops the pop; the queued action then runs.
    REQUIRE(pumpUntil(exec, [&] {
        std::scoped_lock const lock{probe().mtx};
        return probe().order.size() == 3;
    }));
    {
        std::scoped_lock const lock{probe().mtx};
        REQUIRE(probe().order == std::vector<std::string>{"pop-start", "pop-cancelled", "log-5"});
    }
    // The pop unwound on the queue's executor; the action after it did not.
    REQUIRE(probe().popThread.load() != std::thread::id{});
    REQUIRE(probe().logThread.load() != probe().popThread.load());
    REQUIRE(morph::model::detail::ActionGate::overlapsObserved() == overlapsBefore);
    probe().queue.reset();
}

TEST_CASE("a backend switch stops a handler parked on a core-cpp queue, and skips the action behind it",
          "[coroutine][model][foreign][lifetime]") {
    coro_test::SchedulerScope const timers;
    morph::exec::ThreadPoolExecutor pool{2};
    core::async::ThreadPoolExecutor foreign{1};
    morph::exec::MainThreadExecutor exec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CoroModel> handler{bridge, &exec};
    armHold(&exec);
    probe().queue = std::make_unique<core::async::AsyncQueue<int>>(foreign, core::async::AsyncQueueOptions{});
    auto const overlapsBefore = morph::model::detail::ActionGate::overlapsObserved();

    coro_test::SwitchedFlag popSwitched;
    coro_test::SwitchedFlag logSwitched;
    handler.execute(CoroPop{}).then([](int) {}).onError(popSwitched.handler());
    handler.execute(CoroLog{.tag = 6}).then([](int) {}).onError(logSwitched.handler());
    REQUIRE(pumpUntil(exec, [&] {
        std::scoped_lock const lock{probe().mtx};
        return !probe().order.empty();
    }));

    coro_test::switchWithin(bridge, pool);
    REQUIRE(pumpUntil(
        exec, [&] { return popSwitched.seen.load() && logSwitched.seen.load() && probe().holdFinished.load(); }));
    exec.runFor(20ms);
    REQUIRE(morph::model::detail::ActionGate::overlapsObserved() == overlapsBefore);
    std::scoped_lock const lock{probe().mtx};
    REQUIRE(probe().order == std::vector<std::string>{"pop-start", "pop-cancelled"});
    probe().queue.reset();
}

TEST_CASE("a handler returns to its strand from a core-cpp awaiter with ResumeOn and resumeContext()",
          "[coroutine][model][foreign]") {
    coro_test::SchedulerScope const timers;
    morph::exec::ThreadPoolExecutor pool{2};
    core::async::ThreadPoolExecutor foreign{1};
    morph::exec::MainThreadExecutor exec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CoroModel> handler{bridge, &exec};
    armHold(&exec);
    probe().queue = std::make_unique<core::async::AsyncQueue<int>>(foreign, core::async::AsyncQueueOptions{});

    std::optional<int> result;
    handler.execute(CoroPopBack{.tag = 1})
        .then([&](int value) { result = value; })
        .onError([](const std::exception_ptr&) {});
    REQUIRE(pumpUntil(exec, [&] {
        std::scoped_lock const lock{probe().mtx};
        return !probe().order.empty();
    }));
    static_cast<void>(probe().queue->push(41));

    REQUIRE(pumpUntil(exec, [&] { return result.has_value(); }));
    REQUIRE(*result == 42);
    {
        // Off the strand after the pop, back on it after ResumeOn.
        std::scoped_lock const lock{probe().mtx};
        REQUIRE(probe().onOwnStrand == std::vector<bool>{false, true});
    }
    probe().queue.reset();
}

TEST_CASE("an exception a Task handler throws after a suspension reaches onError", "[coroutine][model]") {
    coro_test::SchedulerScope const timers;
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor exec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CoroModel> handler{bridge, &exec};
    armHold(&exec);

    std::string message;
    handler.execute(CoroThrow{}).then([](int) {}).onError([&](const std::exception_ptr& error) {
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& exc) {
            message = exc.what();
        }
    });

    REQUIRE(pumpUntil(exec, [&] { return !message.empty(); }));
    REQUIRE(message == "handler failed after a suspension");
}

TEST_CASE("RemoteServer's executeTimeout stops a suspended Task handler and releases the model",
          "[coroutine][model][remote]") {
    coro_test::SchedulerScope const timers;
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor exec;
    morph::model::detail::ModelRegistryFactory registry;
    morph::model::detail::ActionDispatcher dispatcher;
    registry.registerModel<CoroModel>("Coro_Model");
    dispatcher.registerAction<CoroModel, CoroHold>("Coro_Model", "Coro_Hold");
    dispatcher.registerAction<CoroModel, CoroLog>("Coro_Model", "Coro_Log");
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, dispatcher, registry);
    morph::backend::LimitPolicy policy;
    policy.executeTimeout = 50ms;
    server->setLimitPolicy(policy);
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server)};
    morph::bridge::BridgeHandler<CoroModel> handler{bridge, &exec};
    armHold(&exec);

    bool timedOut = false;
    handler.execute(CoroHold{}).then([](int) {}).onError([&](const std::exception_ptr& error) {
        timedOut = holds<morph::backend::TimeoutError>(error);
    });

    // The held completion is never settled: only the server's timeout can
    // end the handler, and it must, or the model stays gated for good.
    REQUIRE(pumpUntil(exec, [&] { return timedOut && probe().holdFinished.load(); }));
    REQUIRE(probe().holdCancelled.load());

    std::optional<int> logged;
    handler.execute(CoroLog{.tag = 4}).then([&](int value) { logged = value; }).onError([](const std::exception_ptr&) {
    });
    REQUIRE(pumpUntil(exec, [&] { return logged.has_value(); }));
}

TEST_CASE("a Task handler behind RemoteServer replies with its result, and its failures as err",
          "[coroutine][model][remote]") {
    coro_test::SchedulerScope const timers;
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor exec;
    morph::model::detail::ModelRegistryFactory registry;
    morph::model::detail::ActionDispatcher dispatcher;
    registry.registerModel<CoroModel>("Coro_Model");
    dispatcher.registerAction<CoroModel, CoroDouble>("Coro_Model", "Coro_Double");
    dispatcher.registerAction<CoroModel, CoroThrow>("Coro_Model", "Coro_Throw");
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, dispatcher, registry);
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server)};
    morph::bridge::BridgeHandler<CoroModel> handler{bridge, &exec};
    armHold(&exec);

    std::optional<int> result;
    std::string message;
    handler.execute(CoroDouble{}).then([&](int value) { result = value; }).onError([](const std::exception_ptr&) {});
    handler.execute(CoroThrow{}).then([](int) {}).onError([&](const std::exception_ptr& error) {
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& exc) {
            message = exc.what();
        }
    });

    // The action decodes to {} on the server, so x == 0.
    REQUIRE(pumpUntil(exec, [&] { return result.has_value() && !message.empty(); }));
    REQUIRE(*result == 0);
    REQUIRE(message.contains("handler failed after a suspension"));
    // A synchronous dispatch of a Task handler is refused rather than blocking.
    auto holder = morph::model::detail::ModelFactory::create<CoroModel>();
    REQUIRE_THROWS_AS(dispatcher.dispatch("Coro_Model", "Coro_Double", *holder, "{}"), std::logic_error);
}

namespace coro_test {
namespace {

/// A sink that refuses to record a success, as test_action_log.cpp's does: an
/// action that committed, and whose entry did not reach the backend.
class SuccessRefusingLog : public morph::journal::IActionLog {
public:
    void append(morph::journal::LogEntry entry) override {
        std::scoped_lock const lock{_mtx};
        _offered.push_back(entry);
        if (entry.outcome == morph::journal::Outcome::Succeeded) {
            throw std::runtime_error{"journal sink unavailable"};
        }
    }
    void flush() override {}
    [[nodiscard]] std::vector<morph::journal::LogEntry> entries(std::string_view /*entityKey*/ = {}) const override {
        return {};
    }

    /// Every entry the framework asked this sink to record, refused ones included.
    [[nodiscard]] std::vector<morph::journal::LogEntry> offered() const {
        std::scoped_lock const lock{_mtx};
        return _offered;
    }

private:
    mutable std::mutex _mtx;
    std::vector<morph::journal::LogEntry> _offered;
};

/// @return The ActionRecordingError @p error holds, or nothing for any other std::exception.
std::optional<morph::model::ActionRecordingError> recordingError(const std::exception_ptr& error) {
    try {
        std::rethrow_exception(error);
    } catch (const morph::model::ActionRecordingError& err) {
        return err;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

}  // namespace
}  // namespace coro_test

TEST_CASE("a Task handler whose success the journal refuses reports ActionRecordingError, not a rejection",
          "[coroutine][model][action_log]") {
    coro_test::SchedulerScope const timers;
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor exec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    auto log = std::make_shared<coro_test::SuccessRefusingLog>();
    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = "Coro_Model";
    binding->modelFactory = [log] {
        auto holder = morph::model::detail::ModelFactory::create<CoroModel>();
        holder->attachActionLog(log, "coro-sink-down");
        return holder;
    };
    morph::bridge::BridgeHandler<CoroModel> handler{bridge, &exec, binding};

    std::exception_ptr failure;
    handler.execute(CoroDouble{.x = 21}).then([](int) {}).onError([&](const std::exception_ptr& error) {
        failure = error;
    });
    REQUIRE(pumpUntil(exec, [&] { return failure != nullptr; }));

    // The Task completed, so the model's mutation committed: the caller is told
    // it ran and was not recorded, with the result the journal never received.
    auto const seen = coro_test::recordingError(failure);
    REQUIRE(seen.has_value());
    REQUIRE(std::string{seen->what()} == "action executed but was not recorded: journal sink unavailable");
    REQUIRE(seen->result() == "42");
    // Offered as a success and refused; never filed as a rejection.
    auto const offered = log->offered();
    REQUIRE(offered.size() == 1);
    REQUIRE(offered.front().outcome == morph::journal::Outcome::Succeeded);
}

TEST_CASE("dispatchAsync reports a Task handler's refused journal append as ActionRecordingError",
          "[coroutine][model][action_log][remote]") {
    coro_test::SchedulerScope const timers;
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::detail::StrandExecutor strand{pool};
    auto link = std::make_shared<morph::exec::detail::StrandLink>(strand);
    morph::exec::detail::ModelId const key{9};
    auto executor = std::make_shared<morph::exec::StrandCoroExecutor>(link, key, morph::session::Context{});
    morph::model::detail::ActionDispatcher dispatcher;
    dispatcher.registerAction<CoroModel, CoroDouble>("Coro_Model", "Coro_Double");
    auto holder = morph::model::detail::ModelFactory::create<CoroModel>();
    auto log = std::make_shared<coro_test::SuccessRefusingLog>();
    holder->attachActionLog(log, "coro-sink-down-remote");

    std::promise<std::exception_ptr> outcome;
    auto settled = outcome.get_future();
    REQUIRE(link->post(key, [&] {
        dispatcher.dispatchAsync(
            "Coro_Model", "Coro_Double", *holder, "{}", executor, core::async::StopToken{},
            [&](const std::string&, const std::exception_ptr& error) { outcome.set_value(error); });
    }));
    REQUIRE(settled.wait_for(5s) == std::future_status::ready);
    auto const error = settled.get();

    // The wire payload decodes to x == 0, so the committed result is 0.
    auto const seen = coro_test::recordingError(error);
    REQUIRE(seen.has_value());
    REQUIRE(seen->cause() == "journal sink unavailable");
    REQUIRE(seen->result() == "0");
    auto const offered = log->offered();
    REQUIRE(offered.size() == 1);
    REQUIRE(offered.front().outcome == morph::journal::Outcome::Succeeded);
    link->close();
}

TEST_CASE("ActionGate queues an action that arrives during its drain behind the ones already waiting",
          "[coroutine][gate]") {
    morph::model::detail::ActionGate gate;
    std::vector<std::string> order;

    gate.enter([&] { order.emplace_back("a"); });  // takes the gate and keeps it
    REQUIRE_FALSE(gate.tryEnter());
    gate.enter([&] {
        order.emplace_back("b");
        gate.leave();
        // b is done, but c still waits: a newcomer queues behind it rather
        // than starting ahead of it.
        REQUIRE_FALSE(gate.tryEnter());
        gate.enter([&] {
            order.emplace_back("d");
            gate.leave();
        });
    });
    gate.enter([&] {
        order.emplace_back("c");
        gate.leave();
    });
    gate.leave();
    REQUIRE(order == std::vector<std::string>{"a", "b", "c", "d"});

    // Drained, the gate starts an action at once again, by either door.
    REQUIRE(gate.tryEnter());
    gate.leave();
    bool ran = false;
    gate.enter([&] {
        ran = true;
        gate.leave();
    });
    REQUIRE(ran);
}

TEST_CASE("StrandLink::postOrRun runs a task in place once the link is closed", "[coroutine][strand]") {
    morph::exec::ThreadPoolExecutor pool{1};
    morph::exec::detail::StrandExecutor strand{pool};
    morph::exec::detail::StrandLink link{strand};
    morph::exec::detail::ModelId const key{3};

    std::atomic<std::thread::id> postedOn{};
    link.postOrRun(key, [&] { postedOn = std::this_thread::get_id(); });
    REQUIRE(morph::testing::waitUntil([&] { return postedOn.load() != std::thread::id{}; }));
    REQUIRE(postedOn.load() != std::this_thread::get_id());

    link.close();
    std::thread::id ranOn;
    link.postOrRun(key, [&] { ranOn = std::this_thread::get_id(); });
    REQUIRE(ranOn == std::this_thread::get_id());
}

TEST_CASE("StrandExecutor::runningHere answers for the running task's own strand only", "[coroutine][strand]") {
    morph::exec::ThreadPoolExecutor pool{1};
    morph::exec::detail::StrandExecutor strand{pool};
    std::atomic<bool> done{false};
    bool own = false;
    bool other = true;
    strand.post(morph::exec::detail::ModelId{1}, [&] {
        own = strand.runningHere(morph::exec::detail::ModelId{1});
        other = strand.runningHere(morph::exec::detail::ModelId{2});
        done = true;
    });
    REQUIRE(morph::testing::waitUntil([&] { return done.load(); }));
    REQUIRE(own);
    REQUIRE_FALSE(other);
}

TEST_CASE("dispatchesAsync tells a Task handler from an ordinary one, and dispatchAsync runs either",
          "[coroutine][remote]") {
    morph::model::detail::ActionDispatcher dispatcher;
    dispatcher.registerAction<CoroModel, CoroDouble>("Coro_Model", "Coro_Double");
    dispatcher.registerAction<CoroModel, CoroLog>("Coro_Model", "Coro_Log");
    REQUIRE(dispatcher.dispatchesAsync("Coro_Model", "Coro_Double"));
    REQUIRE_FALSE(dispatcher.dispatchesAsync("Coro_Model", "Coro_Log"));
    REQUIRE_FALSE(dispatcher.dispatchesAsync("Coro_Model", "Coro_Unknown"));

    // An ordinary handler through dispatchAsync reports at once, and needs no
    // strand executor.
    auto holder = morph::model::detail::ModelFactory::create<CoroModel>();
    std::optional<std::string> result;
    dispatcher.dispatchAsync("Coro_Model", "Coro_Log", *holder, "{}", nullptr, core::async::StopToken{},
                             [&](const std::string& json, const std::exception_ptr& error) {
                                 result = error ? std::string{"error"} : json;
                             });
    REQUIRE(result == "0");
}

TEST_CASE("a Task handler's action that fails its validator is rejected before the handler starts",
          "[coroutine][model]") {
    coro_test::SchedulerScope const timers;
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor exec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CoroModel> handler{bridge, &exec};

    std::exception_ptr failure;
    handler.execute(CoroValidated{.x = 0}).then([](int) {}).onError([&](const std::exception_ptr& error) {
        failure = error;
    });
    REQUIRE(pumpUntil(exec, [&] { return failure != nullptr; }));
    REQUIRE(holds<morph::model::ValidationError>(failure));

    std::optional<int> ready;
    handler.execute(CoroValidated{.x = 5})
        .then([&](int value) { ready = value; })
        .onError([](const std::exception_ptr&) {});
    REQUIRE(pumpUntil(exec, [&] { return ready.has_value(); }));
    REQUIRE(*ready == 5);
}

TEST_CASE("a Task handler that throws is journalled as Outcome::Failed, locally and through dispatchAsync",
          "[coroutine][model][action_log]") {
    coro_test::SchedulerScope const timers;
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor exec;
    auto log = std::make_shared<coro_test::SuccessRefusingLog>();
    {
        morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
        auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
        binding->typeId = "Coro_Model";
        binding->modelFactory = [log] {
            auto holder = morph::model::detail::ModelFactory::create<CoroModel>();
            holder->attachActionLog(log, "coro-throws-local");
            return holder;
        };
        morph::bridge::BridgeHandler<CoroModel> handler{bridge, &exec, binding};
        std::exception_ptr failure;
        handler.execute(CoroThrow{}).then([](int) {}).onError([&](const std::exception_ptr& error) {
            failure = error;
        });
        REQUIRE(pumpUntil(exec, [&] { return failure != nullptr; }));
    }

    morph::exec::detail::StrandExecutor strand{pool};
    auto link = std::make_shared<morph::exec::detail::StrandLink>(strand);
    morph::exec::detail::ModelId const key{11};
    auto executor = std::make_shared<morph::exec::StrandCoroExecutor>(link, key, morph::session::Context{});
    morph::model::detail::ActionDispatcher dispatcher;
    dispatcher.registerAction<CoroModel, CoroThrow>("Coro_Model", "Coro_Throw");
    auto holder = morph::model::detail::ModelFactory::create<CoroModel>();
    holder->attachActionLog(log, "coro-throws-remote");
    std::promise<std::exception_ptr> outcome;
    auto settled = outcome.get_future();
    REQUIRE(link->post(key, [&] {
        dispatcher.dispatchAsync(
            "Coro_Model", "Coro_Throw", *holder, "{}", executor, core::async::StopToken{},
            [&](const std::string&, const std::exception_ptr& error) { outcome.set_value(error); });
    }));
    REQUIRE(settled.wait_for(5s) == std::future_status::ready);
    REQUIRE(settled.get() != nullptr);
    link->close();

    auto const offered = log->offered();
    REQUIRE(offered.size() == 2);
    for (auto const& entry : offered) {
        REQUIRE(entry.outcome == morph::journal::Outcome::Failed);
        REQUIRE(entry.error == "handler failed after a suspension");
    }
}

TEST_CASE("a Task action queued behind a suspended one does not start once a backend switch failed it",
          "[coroutine][model][lifetime]") {
    coro_test::SchedulerScope const timers;
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor exec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CoroModel> handler{bridge, &exec};
    armHold(&exec);

    coro_test::SwitchedFlag holdSwitched;
    coro_test::SwitchedFlag queuedSwitched;
    handler.execute(CoroHold{.tag = 1}).then([](int) {}).onError(holdSwitched.handler());
    handler.execute(CoroDouble{.x = 2}).then([](int) {}).onError(queuedSwitched.handler());
    REQUIRE(pumpUntil(exec, [&] {
        std::scoped_lock const lock{probe().mtx};
        return !probe().order.empty();
    }));

    coro_test::switchWithin(bridge, pool);
    REQUIRE(pumpUntil(exec, [&] { return holdSwitched.seen.load() && queuedSwitched.seen.load(); }));
    probe().release->resolve(100);
    REQUIRE(pumpUntil(exec, [&] { return probe().holdFinished.load(); }));
    exec.runFor(20ms);
    // CoroDouble notes its strand first thing: it never started.
    std::scoped_lock const lock{probe().mtx};
    REQUIRE(probe().onOwnStrand.empty());
}

TEST_CASE("LocalBackend still stops a running Task handler after sweeping many finished ones",
          "[coroutine][model][lifetime]") {
    coro_test::SchedulerScope const timers;
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor exec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CoroModel> handler{bridge, &exec};

    // More finished Task runs than the backend keeps before sweeping them.
    constexpr int quick = 40;
    std::atomic<int> finished{0};
    for (int index = 0; index < quick; ++index) {
        handler.execute(CoroQuick{.x = index})
            .then([&](int) { finished.fetch_add(1); })
            .onError([](const std::exception_ptr&) {});
    }
    REQUIRE(pumpUntil(exec, [&] { return finished.load() == quick; }));

    // The sweep kept what is still running: a switch stops this one.
    armHold(&exec);
    coro_test::SwitchedFlag holdSwitched;
    handler.execute(CoroHold{.tag = 1}).then([](int) {}).onError(holdSwitched.handler());
    REQUIRE(pumpUntil(exec, [&] {
        std::scoped_lock const lock{probe().mtx};
        return !probe().order.empty();
    }));
    coro_test::switchWithin(bridge, pool);
    REQUIRE(pumpUntil(exec, [&] { return probe().holdCancelled.load() && holdSwitched.seen.load(); }));
}

TEST_CASE("RemoteServer queues an ordinary action behind a suspended Task handler", "[coroutine][model][remote]") {
    coro_test::SchedulerScope const timers;
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor exec;
    morph::model::detail::ModelRegistryFactory registry;
    morph::model::detail::ActionDispatcher dispatcher;
    registry.registerModel<CoroModel>("Coro_Model");
    dispatcher.registerAction<CoroModel, CoroHold>("Coro_Model", "Coro_Hold");
    dispatcher.registerAction<CoroModel, CoroLog>("Coro_Model", "Coro_Log");
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, dispatcher, registry);
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server)};
    morph::bridge::BridgeHandler<CoroModel> handler{bridge, &exec};
    armHold(&exec);

    std::optional<int> held;
    std::optional<int> logged;
    handler.execute(CoroHold{.tag = 1}).then([&](int value) { held = value; }).onError([](const std::exception_ptr&) {
    });
    handler.execute(CoroLog{.tag = 2}).then([&](int value) { logged = value; }).onError([](const std::exception_ptr&) {
    });
    REQUIRE(pumpUntil(exec, [&] {
        std::scoped_lock const lock{probe().mtx};
        return !probe().order.empty();
    }));
    exec.runFor(20ms);
    {
        std::scoped_lock const lock{probe().mtx};
        REQUIRE(probe().order == std::vector<std::string>{"hold-start"});
    }

    probe().release->resolve(100);
    REQUIRE(pumpUntil(exec, [&] { return held.has_value() && logged.has_value(); }));
    // The wire payloads decode to {}, so both tags are 0.
    std::scoped_lock const lock{probe().mtx};
    REQUIRE(probe().order == std::vector<std::string>{"hold-start", "hold-end", "log-0"});
}

TEST_CASE("LocalBackend rejects a call that carries neither localOp nor localOpAsync", "[coroutine][model]") {
    morph::exec::ThreadPoolExecutor pool{1};
    morph::exec::MainThreadExecutor exec;
    morph::backend::LocalBackend backend{pool};
    auto const mid = backend.registerModel("Coro_Model", morph::model::detail::ModelFactory::create<CoroModel>);

    morph::backend::detail::ActionCall call;
    call.modelTypeId = "Coro_Model";
    call.actionTypeId = "Coro_Log";
    std::string message;
    backend.execute(mid, std::move(call), &exec)
        .then([](const std::shared_ptr<void>&) {})
        .onError([&](const std::exception_ptr& error) {
            try {
                std::rethrow_exception(error);
            } catch (const std::exception& exc) {
                message = exc.what();
            }
        });
    REQUIRE(pumpUntil(exec, [&] { return !message.empty(); }));
    REQUIRE(message.contains("localOp is null"));
}

TEST_CASE("dispatchAsync reports an unknown action through done, not by throwing", "[coroutine][remote]") {
    morph::model::detail::ActionDispatcher dispatcher;
    auto holder = morph::model::detail::ModelFactory::create<CoroModel>();
    std::string message;
    dispatcher.dispatchAsync("Coro_Model", "Coro_Unknown", *holder, "{}", nullptr, core::async::StopToken{},
                             [&](const std::string&, const std::exception_ptr& error) {
                                 try {
                                     std::rethrow_exception(error);
                                 } catch (const std::exception& exc) {
                                     message = exc.what();
                                 }
                             });
    REQUIRE(message == "unknown action: Coro_Model/Coro_Unknown");
}

TEST_CASE("ActionGate counts a second thread inside it as an overlap", "[coroutine][gate]") {
    morph::model::detail::ActionGate gate;
    std::promise<void> inside;
    std::promise<void> release;
    auto released = release.get_future();
    std::thread holder{[&] {
        gate.enter([&] {
            inside.set_value();
            // Bounded: the test body releases it right after looking.
            static_cast<void>(released.wait_for(5s));
        });
    }};
    REQUIRE(inside.get_future().wait_for(5s) == std::future_status::ready);

    // The holder is still inside enter(); this thread touching the gate now is
    // exactly the misuse the counter exists to catch.
    auto const before = morph::model::detail::ActionGate::overlapsObserved();
    REQUIRE_FALSE(gate.tryEnter());
    REQUIRE(morph::model::detail::ActionGate::overlapsObserved() > before);

    release.set_value();
    holder.join();
}
