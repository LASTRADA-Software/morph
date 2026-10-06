// SPDX-License-Identifier: Apache-2.0
//
// morph::exec::IoLoop, the loop every socket, timer and probe of an
// application shares: docs/spec/core/executor.md, "The I/O loop". A case that
// holds for both drivers runs once per driver: on an OwnThread loop the test
// waits for the loop's thread, on a Caller loop the test thread turns the loop.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/core/io_loop.hpp>
#include <morph/core/timeout_scheduler.hpp>
#include <stdexcept>
#include <string_view>
#include <thread>

#include "test_support.hpp"

namespace {

using morph::exec::IoLoop;
using morph::exec::IoLoopDriver;
using namespace std::chrono_literals;

bool onLoop(IoLoop& loop) { return morph::exec::runningOn(static_cast<core::async::IExecutor const&>(loop.loop())); }

/// Waits until @p done holds: by polling while an OwnThread loop's thread runs,
/// or by turning a Caller loop on this thread.
template <class Pred>
bool driveUntil(IoLoop& loop, Pred done, std::chrono::milliseconds budget = 2000ms) {
    if (loop.driver() == IoLoopDriver::OwnThread) {
        return morph::testing::waitUntil(done, morph::testing::WaitBudget{budget});
    }
    auto const deadline = std::chrono::steady_clock::now() + budget;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        static_cast<void>(loop.loop().runOnce(5ms));
    }
    return true;
}

/// What a thread that waits on a loop reports. Shared with that thread, so a
/// case that gives up on it can detach it, and the thread then touches nothing
/// on the case's stack.
struct Outcome {
    std::atomic<bool> returned{false};
    std::atomic<bool> rethrown{false};
    std::atomic<bool> otherRunningHere{true};
};

/// Joins @p thread if it @p finished; detaches it otherwise, so a case whose
/// thread waits on a task nobody runs fails instead of hanging in `join`.
/// @return @p finished.
bool joinOrDetach(std::thread& thread, bool finished) {
    if (finished) {
        thread.join();
    } else {
        thread.detach();
    }
    return finished;
}

/// The instances of `Counted` alive, and whether three ever were at once.
struct Counts {
    std::atomic<int> alive{0};
    std::atomic<bool> peaked{false};
    std::atomic<bool> ran{false};
};

/// A task that counts its own copies. After `runAndWait` from another thread
/// has posted it, the caller's argument and the queued copy are the only ones
/// left, and the temporaries in between are destroyed after the post returns:
/// "two alive, after three were" therefore means the post has returned, so the
/// loop may be destroyed without racing it.
class Counted {
public:
    explicit Counted(Counts& counts) noexcept : _counts{&counts} { arrive(); }
    Counted(Counted const& other) noexcept : _counts{other._counts} { arrive(); }
    Counted(Counted&& other) noexcept : _counts{other._counts} { arrive(); }
    Counted& operator=(Counted const&) = delete;
    Counted& operator=(Counted&&) = delete;
    ~Counted() { _counts->alive.fetch_sub(1); }

    void operator()() const { _counts->ran = true; }

private:
    void arrive() noexcept {
        if (_counts->alive.fetch_add(1) + 1 >= 3) {
            _counts->peaked = true;
        }
    }

    Counts* _counts;
};

}  // namespace

TEST_CASE("IoLoop: a posted task runs as a task of the loop, on the loop's thread", "[io_loop]") {
    auto const driver = GENERATE(IoLoopDriver::OwnThread, IoLoopDriver::Caller);
    IoLoop loop{driver};
    CHECK(loop.driver() == driver);
    REQUIRE_FALSE(onLoop(loop));
    // Outside a turn, only a Caller loop's driving thread is the loop's.
    CHECK(loop.runningHere() == (driver == IoLoopDriver::Caller));

    std::atomic<bool> ran{false};
    std::atomic<bool> runningHere{false};
    std::atomic<bool> scoped{false};
    std::thread::id where;
    loop.post([&] {
        runningHere = loop.runningHere();
        scoped = onLoop(loop);
        where = std::this_thread::get_id();
        ran = true;
    });
    REQUIRE(driveUntil(loop, [&] { return ran.load(); }));
    CHECK(runningHere.load());
    CHECK(scoped.load());
    CHECK((where == std::this_thread::get_id()) == (driver == IoLoopDriver::Caller));
}

TEST_CASE("IoLoop: runAndWait returns once the task has run, and nests inline", "[io_loop]") {
    auto const driver = GENERATE(IoLoopDriver::OwnThread, IoLoopDriver::Caller);
    IoLoop loop{driver};
    if (driver == IoLoopDriver::Caller) {
        // Otherwise the runAndWait below waits for a turn this thread never runs.
        REQUIRE(loop.runningHere());
    }
    bool ranHere = false;
    bool ranInScope = false;
    loop.runAndWait([&] {
        ranHere = loop.runningHere();
        ranInScope = onLoop(loop);
    });
    CHECK(ranHere);
    // A Caller loop runs it inline on the driving thread, outside any turn but
    // inside the loop's executor scope, so an owner check that reads the scope
    // agrees with runningHere().
    CHECK(ranInScope);

    // From inside a task of the loop, waiting on the loop would wait forever:
    // it runs the nested task inline instead.
    bool nestedRan = false;
    loop.runAndWait([&] { loop.runAndWait([&] { nestedRan = loop.runningHere(); }); });
    CHECK(nestedRan);
}

TEST_CASE("IoLoop: a task that throws is logged and later tasks still run", "[io_loop]") {
    auto const driver = GENERATE(IoLoopDriver::OwnThread, IoLoopDriver::Caller);
    IoLoop loop{driver};
    loop.post([] { throw std::runtime_error{"boom"}; });
    std::atomic<bool> later{false};
    loop.post([&] { later = true; });
    CHECK(driveUntil(loop, [&] { return later.load(); }));
}

TEST_CASE("IoLoop: runAndWait hands a throw out of its task to the caller", "[io_loop]") {
    auto const driver = GENERATE(IoLoopDriver::OwnThread, IoLoopDriver::Caller);
    IoLoop loop{driver};

    // From another thread: the task runs on the loop, the throw reaches the waiter.
    auto const outcome = std::make_shared<Outcome>();
    std::thread waiter{[&loop, outcome] {
        try {
            loop.runAndWait([] { throw std::runtime_error{"boom"}; });
        } catch (std::runtime_error const& error) {
            outcome->rethrown = std::string_view{error.what()} == "boom";
        } catch (...) {  // NOLINT(bugprone-empty-catch)
            // Any other type: `rethrown` stays false.
        }
        outcome->returned = true;
    }};
    bool const returned = joinOrDetach(waiter, driveUntil(loop, [&] { return outcome->returned.load(); }));
    REQUIRE(returned);
    CHECK(outcome->rethrown.load());

    // Inline, from a task of the loop or a Caller loop's driving thread: the
    // same throw reaches the same place.
    if (driver == IoLoopDriver::Caller) {
        // Otherwise the runAndWait below waits for a turn this thread never runs.
        REQUIRE(loop.runningHere());
    }
    bool nestedRethrown = false;
    loop.runAndWait([&] {
        try {
            loop.runAndWait([] { throw std::runtime_error{"nested"}; });
        } catch (std::runtime_error const&) {
            nestedRethrown = true;
        }
    });
    CHECK(nestedRethrown);

    // The loop goes on running tasks.
    std::atomic<bool> later{false};
    loop.post([&] { later = true; });
    CHECK(driveUntil(loop, [&] { return later.load(); }));
}

TEST_CASE("IoLoop: a weak handle stops posting once the loop is gone", "[io_loop]") {
    auto const driver = GENERATE(IoLoopDriver::OwnThread, IoLoopDriver::Caller);
    auto loop = std::make_unique<IoLoop>(driver);
    auto const weak = loop->weak();
    std::atomic<bool> ran{false};
    REQUIRE(weak.post([&] { ran = true; }));
    REQUIRE(driveUntil(*loop, [&] { return ran.load(); }));
    loop.reset();
    CHECK_FALSE(weak.post([] {}));
}

// OwnThread only: a Caller loop is destroyed on its driving thread outside a
// turn (the next cases), never from one of its own tasks.
TEST_CASE("IoLoop: destroyed from one of its own tasks, it lets the thread finish instead of joining it",
          "[io_loop]") {
    auto loop = std::make_unique<IoLoop>();
    auto const weak = loop->weak();
    std::atomic<bool> destroyed{false};
    loop->post([&] {
        loop.reset();
        destroyed = true;
    });
    REQUIRE(morph::testing::waitUntil([&] { return destroyed.load(); }));
    // The thread held the loop's last share and released it on the way out.
    CHECK(morph::testing::waitUntil([&] { return !weak.post([] {}); }));
}

TEST_CASE("IoLoop (Caller): nothing runs until the driving thread turns the loop", "[io_loop][caller]") {
    IoLoop loop{IoLoopDriver::Caller};
    std::atomic<bool> ran{false};
    loop.post([&] { ran = true; });
    std::this_thread::sleep_for(20ms);
    CHECK_FALSE(ran.load());
    static_cast<void>(loop.loop().runUntilIdle());
    CHECK(ran.load());
}

TEST_CASE("IoLoop (Caller): runAndWait on the driving thread outside a turn runs inline", "[io_loop][caller]") {
    IoLoop loop{IoLoopDriver::Caller};
    REQUIRE(loop.runningHere());
    bool ran = false;
    std::thread::id where;
    loop.runAndWait([&] {
        ran = true;
        where = std::this_thread::get_id();
    });
    CHECK(ran);
    CHECK(where == std::this_thread::get_id());
}

TEST_CASE("IoLoop (Caller): another thread's post and runAndWait run on the driving thread's next turn",
          "[io_loop][caller]") {
    IoLoop loop{IoLoopDriver::Caller};
    std::atomic<bool> posted{false};
    std::atomic<bool> scoped{false};
    std::thread::id postedOn;
    std::thread::id waitedOn;
    auto const outcome = std::make_shared<Outcome>();
    // The tasks run on this thread, in its turns; the thread itself records into
    // `outcome` only, so it can be detached if the wait never ends.
    std::thread other{[&loop, &posted, &scoped, &postedOn, &waitedOn, outcome] {
        // Recorded, not asserted: Catch2 assertions are not thread-safe.
        outcome->otherRunningHere = loop.runningHere();
        loop.post([&] {
            postedOn = std::this_thread::get_id();
            scoped = onLoop(loop);
            posted = true;
        });
        loop.runAndWait([&] { waitedOn = std::this_thread::get_id(); });
        outcome->returned = true;
    }};
    bool const done = joinOrDetach(other, driveUntil(loop, [&] { return outcome->returned.load(); }));
    REQUIRE(done);
    CHECK_FALSE(outcome->otherRunningHere.load());
    CHECK(posted.load());
    CHECK(scoped.load());
    CHECK(postedOn == std::this_thread::get_id());
    CHECK(waitedOn == std::this_thread::get_id());
}

TEST_CASE("IoLoop (Caller): destroyed outside a turn, it drops queued work unrun", "[io_loop][caller]") {
    auto loop = std::make_unique<IoLoop>(IoLoopDriver::Caller);
    auto const weak = loop->weak();
    auto sentinel = std::make_shared<int>(0);
    bool ran = false;
    loop->post([&ran, held = sentinel] {
        static_cast<void>(held);
        ran = true;
    });
    CHECK(sentinel.use_count() == 2);
    loop.reset();
    CHECK_FALSE(ran);
    CHECK(sentinel.use_count() == 1);
    CHECK_FALSE(weak.post([] {}));
}

TEST_CASE("IoLoop (Caller): destroyed unturned while another thread waits in runAndWait, it ends the wait",
          "[io_loop][caller]") {
    auto loop = std::make_unique<IoLoop>(IoLoopDriver::Caller);
    auto const counts = std::make_shared<Counts>();
    auto const outcome = std::make_shared<Outcome>();
    std::thread waiter{[&target = *loop, counts, outcome] {
        target.runAndWait(Counted{*counts});
        outcome->returned = true;
    }};
    bool const posted = morph::testing::waitUntil([&] { return counts->peaked.load() && counts->alive.load() == 2; });
    if (!posted) {
        // The post may still be in flight: destroying the loop would race it,
        // so the loop is leaked instead.
        [[maybe_unused]] IoLoop const* const leaked = loop.release();
        waiter.detach();
        FAIL("runAndWait's post did not return");
    }
    loop.reset();
    bool const returned = joinOrDetach(waiter, morph::testing::waitUntil([&] { return outcome->returned.load(); }));
    REQUIRE(returned);
    CHECK_FALSE(counts->ran.load());
    CHECK(counts->alive.load() == 0);
}

TEST_CASE("IoLoop (Caller): a component's close runs inline on the driving thread", "[io_loop][caller]") {
    IoLoop loop{IoLoopDriver::Caller};
    // Otherwise the destructor below waits for a turn this thread never runs.
    REQUIRE(loop.runningHere());
    std::atomic<bool> fired{false};
    {
        morph::async::detail::TimeoutScheduler scheduler{loop};
        static_cast<void>(scheduler.schedule(1ms, [&] { fired = true; }));
        // No turn has run, so the arming is still queued. The destructor's
        // close runs inline here; waiting for a turn would never return.
    }
    std::this_thread::sleep_for(5ms);
    static_cast<void>(loop.loop().runUntilIdle());
    CHECK_FALSE(fired.load());
    CHECK(loop.loop().pendingTimerCount() == 0U);
}
