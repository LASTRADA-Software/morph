// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <functional>
#include <morph/core/executor.hpp>
#include <thread>

TEST_CASE("morph::exec::ThreadPoolExecutor runs posted tasks", "[executor]") {
    morph::exec::ThreadPoolExecutor pool{2};
    std::atomic<int> count{0};
    for (int i = 0; i < 10; ++i) {
        pool.post([&] { count.fetch_add(1); });
    }
    // Give workers time to drain
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(count.load() == 10);
}

TEST_CASE("morph::exec::ThreadPoolExecutor swallows exceptions from tasks", "[executor]") {
    morph::exec::ThreadPoolExecutor pool{1};
    std::atomic<bool> reached{false};
    pool.post([] { throw std::runtime_error("boom"); });
    pool.post([&] { reached.store(true); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(reached.load());
}

namespace {

using namespace std::chrono_literals;

// Polls @p pred until it holds or @p timeout passes; true when it held.
bool waitFor(std::function<bool()> const& pred, std::chrono::milliseconds timeout = 5000ms) {
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(50us);
    }
    return true;
}

}  // namespace

TEST_CASE("morph::exec::ThreadPoolExecutor loses no task to a post racing a worker going to sleep",
          "[executor][pool-spin]") {
    // Bursts separated by pauses longer than the spin, so each burst meets
    // workers that are asleep, spinning, or in between.
    morph::exec::PoolOptions options;
    options.spinBeforeSleep = 100us;
    morph::exec::ThreadPoolExecutor pool{4, options};
    std::atomic<int> ran{0};
    int posted = 0;
    for (int burst = 0; burst < 300; ++burst) {
        int const n = 1 + (burst % 3);
        for (int i = 0; i < n; ++i) {
            pool.post([&] { ran.fetch_add(1); });
            ++posted;
        }
        REQUIRE(waitFor([&] { return ran.load() == posted; }));
        std::this_thread::sleep_for(std::chrono::microseconds{20 + ((burst % 7) * 40)});
    }
}

TEST_CASE("morph::exec::ThreadPoolExecutor runs a task posted between the end of a spin and the lock",
          "[executor][pool-spin]") {
    std::atomic<bool> armed{true};
    std::atomic<bool> inGap{false};
    std::atomic<bool> release{false};
    morph::exec::PoolOptions options;
    options.spinBeforeSleep = 100us;
    options.afterSpin = [&] {
        if (armed.exchange(false)) {
            inGap.store(true);
            waitFor([&] { return release.load(); });
        }
    };
    std::atomic<bool> ran{false};
    {
        morph::exec::ThreadPoolExecutor pool{1, options};
        // The single worker is now parked between its spin and the lock, counted
        // as spinning, so the post below does not notify anyone.
        REQUIRE(waitFor([&] { return inGap.load(); }));
        pool.post([&] { ran.store(true); });
        release.store(true);
        REQUIRE(waitFor([&] { return ran.load(); }, 2000ms));
    }
}

TEST_CASE("morph::exec::ThreadPoolExecutor wakes a sleeper for the second queued task while one worker spins",
          "[executor][pool-spin]") {
    std::atomic<bool> armed{true};
    std::atomic<bool> inGap{false};
    std::atomic<bool> release{false};
    morph::exec::PoolOptions options;
    options.spinBeforeSleep = 100us;
    options.afterSpin = [&] {
        if (armed.exchange(false)) {
            inGap.store(true);
            waitFor([&] { return release.load(); });
        }
    };
    std::atomic<int> ran{0};
    {
        morph::exec::ThreadPoolExecutor pool{2, options};
        // One worker is held while counted as spinning; the other is asleep.
        REQUIRE(waitFor([&] { return inGap.load() && pool.sleepingWorkers() == 1; }));
        pool.post([&] { ran.fetch_add(1); });
        pool.post([&] { ran.fetch_add(1); });
        // The spinner is still held, so only the sleeper can have run them: the
        // second post had to wake it.
        REQUIRE(waitFor([&] { return ran.load() == 2; }, 2000ms));
        release.store(true);
    }
}

TEST_CASE("morph::exec::ThreadPoolExecutor with a spin runs blocking tasks posted together at once",
          "[executor][pool-spin]") {
    morph::exec::PoolOptions options;
    options.spinBeforeSleep = 20000us;
    morph::exec::ThreadPoolExecutor pool{2, options};
    for (int round = 0; round < 10; ++round) {
        // At least one worker sleeps; the other spins or, once its spin has run
        // out, sleeps too. Waiting for exactly one would race the end of the
        // spin, and a coarse sleep clock (Windows) can step over that window.
        REQUIRE(waitFor([&] { return pool.sleepingWorkers() >= 1; }));
        std::atomic<int> arrived{0};
        std::atomic<int> met{0};
        auto const task = [&] {
            arrived.fetch_add(1);
            if (waitFor([&] { return arrived.load() == 2; }, 2000ms)) {
                met.fetch_add(1);
            }
        };
        pool.post(task);
        pool.post(task);
        REQUIRE(waitFor([&] { return met.load() == 2; }, 3000ms));
        // Let the pool settle before the next round.
        REQUIRE(waitFor([&] { return pool.sleepingWorkers() >= 1; }));
    }
}

TEST_CASE("morph::exec::ThreadPoolExecutor with a zero spin sleeps at once and never spins", "[executor][pool-spin]") {
    std::atomic<int> spins{0};
    morph::exec::PoolOptions options;
    options.spinBeforeSleep = 0us;
    options.afterSpin = [&] { spins.fetch_add(1); };
    morph::exec::ThreadPoolExecutor pool{2, options};
    std::atomic<int> ran{0};
    for (int i = 0; i < 20; ++i) {
        pool.post([&] { ran.fetch_add(1); });
    }
    REQUIRE(waitFor([&] { return ran.load() == 20; }));
    REQUIRE(waitFor([&] { return pool.sleepingWorkers() == 2; }));
    REQUIRE(spins.load() == 0);
}

TEST_CASE("morph::exec::ThreadPoolExecutor with a spin spins after a task and then sleeps", "[executor][pool-spin]") {
    std::atomic<int> spins{0};
    morph::exec::PoolOptions options;
    options.spinBeforeSleep = 200us;
    options.afterSpin = [&] { spins.fetch_add(1); };
    morph::exec::ThreadPoolExecutor pool{2, options};
    std::atomic<bool> ran{false};
    pool.post([&] { ran.store(true); });
    REQUIRE(waitFor([&] { return ran.load(); }));
    REQUIRE(waitFor([&] { return pool.sleepingWorkers() == 2; }));
    REQUIRE(spins.load() > 0);
}

TEST_CASE("morph::exec::ThreadPoolExecutor lets at most maxSpinners workers spin", "[executor][pool-spin]") {
    std::atomic<int> inSpinGap{0};
    std::atomic<int> peak{0};
    std::atomic<bool> release{false};
    morph::exec::PoolOptions options;
    options.spinBeforeSleep = 50us;
    options.maxSpinners = 2;
    options.afterSpin = [&] {
        int const now = inSpinGap.fetch_add(1) + 1;
        int seen = peak.load();
        while (now > seen && !peak.compare_exchange_weak(seen, now)) {
        }
        waitFor([&] { return release.load(); });
        inSpinGap.fetch_sub(1);
    };
    {
        morph::exec::ThreadPoolExecutor pool{4, options};
        REQUIRE(waitFor([&] { return pool.sleepingWorkers() == 2 && inSpinGap.load() == 2; }));
        // Two are spinners parked in the hook; the other two went to sleep at once.
        REQUIRE(peak.load() == 2);
        release.store(true);
    }
}

TEST_CASE("morph::exec::ThreadPoolExecutor destructor runs every queued task and does not wait out a long spin",
          "[executor][pool-spin]") {
    std::atomic<int> ran{0};
    auto const begin = std::chrono::steady_clock::now();
    {
        morph::exec::PoolOptions options;
        options.spinBeforeSleep = std::chrono::seconds{30};
        morph::exec::ThreadPoolExecutor pool{3, options};
        for (int i = 0; i < 200; ++i) {
            pool.post([&] { ran.fetch_add(1); });
        }
    }
    REQUIRE(ran.load() == 200);
    REQUIRE(std::chrono::steady_clock::now() - begin < std::chrono::seconds{5});
}

TEST_CASE("morph::exec::MainThreadExecutor drains tasks via runFor", "[executor]") {
    morph::exec::MainThreadExecutor exec;
    std::atomic<int> count{0};
    for (int i = 0; i < 5; ++i) {
        exec.post([&] { count.fetch_add(1); });
    }
    exec.runFor(std::chrono::milliseconds(200));
    REQUIRE(count.load() == 5);
}

TEST_CASE("morph::exec::MainThreadExecutor times out when queue is empty", "[executor]") {
    morph::exec::MainThreadExecutor exec;
    auto startTime = std::chrono::steady_clock::now();
    exec.runFor(std::chrono::milliseconds(50));
    auto elapsed = std::chrono::steady_clock::now() - startTime;
    // Should return roughly on time (within 2x)
    REQUIRE(elapsed < std::chrono::milliseconds(200));
}

TEST_CASE("morph::exec::MainThreadExecutor runOnce runs exactly one queued task", "[executor]") {
    morph::exec::MainThreadExecutor exec;
    std::atomic<int> count{0};
    exec.post([&] { count.fetch_add(1); });
    exec.post([&] { count.fetch_add(1); });

    bool const ranFirst = exec.runOnce();
    REQUIRE(ranFirst);
    REQUIRE(count.load() == 1);

    bool const ranSecond = exec.runOnce();
    REQUIRE(ranSecond);
    REQUIRE(count.load() == 2);
}

TEST_CASE("morph::exec::MainThreadExecutor runOnce returns immediately and false on an empty queue", "[executor]") {
    morph::exec::MainThreadExecutor exec;
    auto const startTime = std::chrono::steady_clock::now();
    bool const ran = exec.runOnce();
    auto const elapsed = std::chrono::steady_clock::now() - startTime;
    REQUIRE_FALSE(ran);
    // Must not block waiting for a task — this is the whole point of runOnce().
    REQUIRE(elapsed < std::chrono::milliseconds(50));
}

TEST_CASE("morph::exec::MainThreadExecutor runOnce logs and swallows std::exception, still consuming the task",
          "[executor]") {
    morph::exec::MainThreadExecutor exec;
    std::atomic<int> count{0};
    exec.post([] { throw std::runtime_error("boom"); });
    exec.post([&] { count.fetch_add(1); });

    REQUIRE(exec.runOnce());  // consumes the throwing task
    REQUIRE(count.load() == 0);
    REQUIRE(exec.runOnce());  // consumes the second task
    REQUIRE(count.load() == 1);
}

TEST_CASE("morph::exec::MainThreadExecutor drain runs all queued tasks without blocking for new ones", "[executor]") {
    morph::exec::MainThreadExecutor exec;
    constexpr int numTasks = 25;
    std::atomic<int> count{0};
    for (int i = 0; i < numTasks; ++i) {
        exec.post([&] { count.fetch_add(1); });
    }

    auto const startTime = std::chrono::steady_clock::now();
    exec.drain();
    auto const elapsed = std::chrono::steady_clock::now() - startTime;

    REQUIRE(count.load() == numTasks);
    // Must return as soon as the queue empties, not wait around for a timeout.
    REQUIRE(elapsed < std::chrono::milliseconds(200));
}

TEST_CASE("morph::exec::MainThreadExecutor drain returns immediately when the queue is already empty", "[executor]") {
    morph::exec::MainThreadExecutor exec;
    auto const startTime = std::chrono::steady_clock::now();
    exec.drain();
    auto const elapsed = std::chrono::steady_clock::now() - startTime;
    REQUIRE(elapsed < std::chrono::milliseconds(50));
}

TEST_CASE("morph::exec::MainThreadExecutor drain runs a bounded chain of tasks that each repost once more",
          "[executor]") {
    // A task that posts one more piece of work while running must not make
    // drain() spin forever, but the freshly-posted task lands back in the
    // (still non-empty) queue and must still run before drain() returns.
    morph::exec::MainThreadExecutor exec;
    std::atomic<int> count{0};
    constexpr int chainLength = 3;

    std::function<void(int)> postChain = [&](int remaining) {
        if (remaining <= 0) {
            return;
        }
        count.fetch_add(1);
        exec.post([&postChain, remaining] { postChain(remaining - 1); });
    };
    exec.post([&] { postChain(chainLength); });

    exec.drain();
    REQUIRE(count.load() == chainLength);
}

// ── A non-std::exception must not escape the main-thread pump ──
//
// runTask() caught only `const std::exception&`, while ThreadPoolExecutor::loop
// has caught `...` as well all along. Three doc claims on this class depended on
// the missing arm: runFor "execution continues with the next task", runOnce
// "returns `true` whether or not that task threw" (it did not return at all),
// and drain, which left the queue undrained.
TEST_CASE("MainThreadExecutor: a task throwing a non-std::exception is contained", "[executor][morph501]") {
    morph::exec::MainThreadExecutor exec;
    bool ranAfter = false;
    exec.post([] { throw 42; });  // not derived from std::exception
    exec.post([&ranAfter] { ranAfter = true; });

    // Before the fix this call propagated the `int` instead of returning.
    REQUIRE(exec.runOnce());
    // And the queue must still be pumpable afterwards.
    REQUIRE(exec.runOnce());
    REQUIRE(ranAfter);
}
