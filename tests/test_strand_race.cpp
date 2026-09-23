// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <morph/core/executor.hpp>
#include <morph/core/strand.hpp>
#include <mutex>
#include <thread>
#include <vector>

// Regression test for the per-key serialisation race in StrandExecutor.
//
// The bug: post() captured the strand under _mapMtx, released _mapMtx, then
// re-armed the strand under only strand->mtx. A concurrent drain in
// scheduleNext (holding {_mapMtx, strand->mtx}) could see the pending queue
// empty, clear running, and erase the strand from the map in the window after
// post() released _mapMtx but before it re-armed. The re-armed strand was then
// orphaned: a later post(key) created a *second* strand for the same key, and
// both strands dispatched tasks for that key concurrently — breaking the
// per-model serialisation guarantee.
//
// This file holds three cases, and they cover different things. This first one is
// a *load* test: it hammers post() on a single key from many threads, and every
// task bumps a per-key in-flight counter on entry and drops it on exit, so if
// two tasks for the same key ever run concurrently the counter exceeds 1 and the
// test fails.
//
// What it does **not** cover is the drain-and-erase boundary the bug above lives
// on. The erase only fires when a drain finds the pending queue empty, and eight
// threads posting 3200 tasks back-to-back onto one key keep it non-empty almost
// throughout: the quiet moment the defect needs never arrives. Measured, not
// assumed -- with the pre-fix two-step drain restored in `scheduleNext` (flip
// `running` under `strand->mtx`, release it, then erase under `_mapMtx` in a
// separate critical section), this case passed 10/10 under ThreadSanitizer on
// x86-64 Linux / clang 22.1.8. Short tasks maximise *re-arm*; they do
// not produce a drain. Turning the thread or post counts up makes that worse,
// not better.
//
// The second case below produces the shape this one cannot, and is the one that
// fails against that mutant. The third covers the node the drain now recycles
// which neither of the first two can be wrong about. Keep all
// three: saturation, the drain boundary, and the recycled node's key are
// different failure modes of the same invariant.
namespace {

// ── The drain's diagnostic, and why it is a watchdog and not a deadline ──────
//
// This case has been observed hanging under ThreadSanitizer and killed by
// ctest's 120 s TIMEOUT having printed nothing but the Catch2 banner: no
// assertion, no TSan report, no reason. That observation is weak and stays
// weak -- 1 of 3 full-suite runs, 0 of 40 isolated, and this lane did not
// reproduce it.
//
// The *structural* half of the issue is checkable by reading, and it holds.
// `~StrandExecutor` waits on
// `_cv.wait(lock, [this] { return _inFlight == 0; })`
// (`include/morph/core/strand.hpp:64-66`) with no timeout, and the strand is
// scoped so that this wait *is* the drain.
//
// One correction to the issue's framing, because it decides the remedy. The
// pre-#374 `2000 x 1 ms` budget was never a bound on the hang:
// `~StrandExecutor` was unbounded then too and ran at the end of every
// iteration regardless, so a lost wakeup hung the pre-#374 binary just as
// thoroughly. What that budget bounded was the time to the *first diagnostic*
// -- a failed `REQUIRE` naming `completed` against `kExpected`, printed before
// the same unbounded wait was entered. A deadline there does not create the
// hang; it is the only thing that speaks before it.
//
// So restoring a deadline would be the wrong remedy twice over: it would not
// bound the hang, and it would re-introduce
// a `REQUIRE` about how fast the host is, evaluated before the invariant this
// file exists for. What is restored below is the diagnostic with no verdict
// attached: a watchdog thread that says where the case is and whether it is
// still moving, and that fails nothing. A slow host prints a few lines and
// still passes. A wedged one prints the same lines with `completed` frozen,
// and the ctest timeout that follows carries the evidence it used to lack.
//
// The `+N since the last report` field is the whole point: it separates "this
// host is slow" from "this strand is stuck", which is the one thing a bare
// timeout cannot determine about an observed hang.
//
// The period is `MORPH_STRAND_DRAIN_WATCHDOG_MS`, default 10000. Ten seconds
// against a 0.4 s median for the whole case leaves about eleven reports inside
// ctest's 120 s TIMEOUT, and the override is how the diagnostic is
// demonstrated without waiting for a hang that may never come back.
class DrainWatchdog {
public:
    /// @brief What the watchdog reads. Every field is written by the test
    ///        thread and read by the watchdog thread, so all of it is atomic.
    struct State {
        std::atomic<int>* completed;
        std::atomic<int>* inFlight;
        std::atomic<int>* maxInFlight;
        std::atomic<const char*> phase{"starting"};
        int iteration{0};
        int expected{0};
    };

    /// @brief Arms the watchdog. @p state must outlive it.
    explicit DrainWatchdog(State& state) : _state{&state}, _thread{[this] { run(); }} {}

    DrainWatchdog(const DrainWatchdog&) = delete;
    DrainWatchdog(DrainWatchdog&&) = delete;
    DrainWatchdog& operator=(const DrainWatchdog&) = delete;
    DrainWatchdog& operator=(DrainWatchdog&&) = delete;

    /// @brief Disarms and joins. Declared *before* the strand in the scope
    ///        below, so reverse destruction order keeps it running across
    ///        `~StrandExecutor` -- which is the wait it exists to report on.
    ~DrainWatchdog() {
        {
            const std::scoped_lock lock{_mtx};
            _stop = true;
        }
        _cv.notify_all();
        _thread.join();
    }

private:
    static std::chrono::milliseconds period() {
        constexpr std::chrono::milliseconds kDefault{10000};
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        const char* raw = std::getenv("MORPH_STRAND_DRAIN_WATCHDOG_MS");
        if (raw == nullptr) {
            return kDefault;
        }
        const long parsed = std::strtol(raw, nullptr, 10);
        return parsed > 0 ? std::chrono::milliseconds{parsed} : kDefault;
    }

    void run() {
        const auto tick = period();
        const auto armed = std::chrono::steady_clock::now();
        int previous = _state->completed->load();
        std::unique_lock lock{_mtx};
        while (!_cv.wait_for(lock, tick, [this] { return _stop; })) {
            const int current = _state->completed->load();
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - armed).count();
            // Straight to `std::cerr`, never through Catch2: this runs on a
            // second thread, where Catch2's macros are not safe to call, and
            // the point is to emit something even when the process is about to
            // be killed. Flushed per line so a SIGKILL cannot eat half a report.
            std::cerr << "strand-race watchdog: iteration " << _state->iteration << ", phase '" << _state->phase.load()
                      << "', " << elapsed << " s into the iteration, completed " << current << "/" << _state->expected
                      << " (+" << (current - previous) << " since the last report), inFlight "
                      << _state->inFlight->load() << ", maxInFlight " << _state->maxInFlight->load() << '\n'
                      << std::flush;
            previous = current;
        }
    }

    State* _state;
    std::mutex _mtx;
    std::condition_variable _cv;
    bool _stop{false};
    std::thread _thread;
};

}  // namespace

// `[slow]` is what gives this case its own ctest `TIMEOUT`; see
// tests/CMakeLists.txt, where the tag is excluded from the blanket 120 s and
// registered again with a budget sized from this case's measured loaded
// runtime. It is a *scheduling* budget, not a performance one -- see the note
// on `kIterations` below.
TEST_CASE("StrandExecutor never runs two tasks for one key concurrently under contention", "[strand][race][slow]") {
    constexpr int kThreads = 8;
    constexpr int kPostsPerThread = 400;
    // Detection power, not a duration. Each iteration is one fresh
    // pool/strand pair sampling the drain-and-re-arm interleaving once; twenty
    // of them is how often this case gets to observe it. Cutting this number
    // is the cheap way to fit a timeout and it makes the case worse at the one
    // thing it exists for, so the budget lives on the ctest entry instead.
    //
    // What the case actually costs is set by the *scheduler*, not by the work:
    // the strand serialises `kThreads * kPostsPerThread` tasks, and each
    // handoff is a wakeup that has to wait its turn on the run queue. Measured
    // here, 12 cores, clang 22.1.8 Release, synthetic spin-loop load, whole
    // case wall clock:
    //
    //     run queue  1 (idle)  ->    0.14 s
    //     run queue 14         ->   23.8 s
    //     run queue 27         ->  170.5 s
    //     run queue 38         ->  396.4 s
    //
    // Steeply superlinear in the oversubscription ratio, and the serialisation
    // invariant held in every one of those runs -- `inFlight 1, maxInFlight 1`
    // throughout, 40 assertions passed. A host busy enough will still exceed
    // any fixed ceiling; that is a property of the measurement, not a defect
    // this case can assert its way out of.
    constexpr int kIterations = 20;

    morph::exec::detail::ModelId const key{42};

    constexpr int kExpected = kThreads * kPostsPerThread;

    for (int iter = 0; iter < kIterations; ++iter) {
        // Declared outside the strand's scope so it is destroyed *after* the
        // strand -- docs/spec/concurrency_and_lifetimes.md, "base IExecutor
        // must outlive its StrandExecutor". The reverse order deadlocks.
        morph::exec::ThreadPoolExecutor pool{4};

        // Outlive the strand too: the drain below runs their final updates.
        std::atomic<int> inFlight{0};
        std::atomic<int> maxInFlight{0};
        std::atomic<int> completed{0};

        auto task = [&] {
            int const cur = inFlight.fetch_add(1) + 1;
            int prev = maxInFlight.load();
            while (cur > prev && !maxInFlight.compare_exchange_weak(prev, cur)) {
            }
            // Tiny window so drains and re-arms interleave heavily.
            std::this_thread::yield();
            inFlight.fetch_sub(1);
            completed.fetch_add(1);
        };

        DrainWatchdog::State watched{.completed = &completed,
                                     .inFlight = &inFlight,
                                     .maxInFlight = &maxInFlight,
                                     .phase = {"posting"},
                                     .iteration = iter,
                                     .expected = kExpected};

        {
            // Declared before the strand, so reverse destruction order keeps
            // it alive across `~StrandExecutor` -- the unbounded wait it
            // exists to report on. See the note above `DrainWatchdog`.
            const DrainWatchdog watchdog{watched};

            morph::exec::detail::StrandExecutor strand{pool};

            std::vector<std::thread> producers;
            producers.reserve(kThreads);
            for (int t = 0; t < kThreads; ++t) {
                producers.emplace_back([&] {
                    for (int i = 0; i < kPostsPerThread; ++i) {
                        strand.post(key, task);
                    }
                });
            }
            watched.phase.store("joining producers");
            for (auto& producer : producers) {
                producer.join();
            }
            watched.phase.store("draining (~StrandExecutor)");
            // Every producer has joined, so nothing else will post -- which is
            // also what the spec's "no post() may race or follow
            // ~StrandExecutor" corollary requires. Closing this scope runs
            // `~StrandExecutor`, which blocks until `_inFlight == 0`; because
            // the re-arm in `scheduleNext` increments `_inFlight` for the next
            // dispatch *before* the current one decrements, the count never
            // dips to zero across a handoff, so `_inFlight == 0` with no
            // producer left means every queued task has run.
            //
            // Not a fixed budget of 2000 x 1 ms sleeps.
            // That budget is ~2 s of wall clock for 3200 strand-serialised
            // tasks, 20 times over, and could expire with work still queued on
            // a loaded machine. Worse, the deficit was a `REQUIRE` and came
            // first, so Catch2 aborted the case before `maxInFlight` -- the
            // only reason this test exists -- was ever evaluated: a busy host
            // turned "the strand serialisation test" into "no strand
            // serialisation check ran", reported as a strand failure. The
            // drain is now a synchronisation point rather than a deadline, so
            // it does not depend on how fast the host is.
            //
            // The budget was never the runtime bound it looked like, either:
            // `~StrandExecutor` ran at the end of every iteration regardless
            // and blocked for the same drain, so on a green run the polling
            // loop waited for something the destructor was about to wait for
            // anyway. All the loop ever added was a way to fail first. What
            // remains is CTest's own per-test `TIMEOUT 120`
            // (`tests/CMakeLists.txt`), which is the right place for "this
            // host was too slow" to be reported: as a timeout, not as an
            // invariant that did not hold.
        }

        INFO("iteration " << iter << ": completed " << completed.load() << " of " << kExpected);
        // A `CHECK`, and deliberately not a `REQUIRE`: the two questions are
        // independent and both must be answered. `~StrandExecutor` has already
        // returned, so a deficit here is a task the strand lost, never a slow
        // host -- and reporting it must not stop `maxInFlight` below from
        // being evaluated over the tasks that did run.
        CHECK(completed.load() == kExpected);
        // The core invariant: at most one task for this key ever runs at once.
        // This one is a `REQUIRE` -- a value above 1 means two strands ran the
        // same key's tasks concurrently, which is the race this file regresses
        // and not something to keep iterating past.
        REQUIRE(maxInFlight.load() == 1);
    }
}

// The drain-and-re-arm boundary, which the case above never reaches.
//
// Shape, not volume. The defect needs a strand to reach *empty* while a post is
// arriving, so this case manufactures that rendezvous instead of hoping for it:
//
//   1. A pilot task is posted alone on the key. Its last act is to publish the
//      round number, so the chaser threads learn the strand is about to drain.
//   2. `kChasers` threads spin on that publication and post the instant it
//      flips -- that is, while the drain block following the pilot's body is
//      deciding "keep running vs. erase". A per-thread stagger walks each post
//      across the handful of instructions that decision spans, so the window is
//      sampled at many offsets rather than one.
//   3. The round ends only once every one of its tasks has run, so the strand
//      really does empty before the next pilot. The gap is the point of the
//      test, and is exactly what sustained saturation destroys.
//
// Three detectors, because the defect and its symptom are not the same event:
//
//   * `maxInFlight` -- the *symptom*, as in the case above: two tasks for one
//     key running at the same wall-clock moment.
//   * plain, non-atomic state touched by every task -- the *defect*. Two strands
//     for one key leave those accesses unordered by any happens-before edge,
//     which ThreadSanitizer reports whether or not the two tasks ever overlap in
//     wall clock. Lost updates to the same state are visible without a sanitizer
//     at all, which is why the cells are checked as well as raced on.
//   * per-producer FIFO -- a strand orphaned mid-burst can run one producer's
//     later task before its earlier one, and an ordinary build sees that too.
//
// The tasks deliberately do a little plain work rather than none: an empty task
// gives an overlap a window a few instructions wide, which is why the case above
// can be wrong about serialisation and still report `maxInFlight == 1`.
TEST_CASE("StrandExecutor keeps one strand per key when a post races the drain", "[strand][race]") {
    constexpr int kChasers = 4;
    constexpr int kBurst = 3;
    constexpr int kRounds = 600;
    constexpr int kIterations = 6;
    constexpr int kCells = 24;
    constexpr int kPerRound = 1 + (kChasers * kBurst);

    morph::exec::detail::ModelId const key{7};

    for (int iter = 0; iter < kIterations; ++iter) {
        // Same ordering rule as the case above: the pool outlives the strand.
        morph::exec::ThreadPoolExecutor pool{4};

        // Deliberately plain -- no atomic, no mutex. Under the invariant this
        // file exists for, the strand *is* the synchronisation: every handoff
        // between two tasks for one key passes through `_mapMtx`/`strand->mtx`,
        // so each task's writes happen-before the next task's reads and these
        // are data-race-free. A second strand for the same key breaks that
        // chain, and then they are not.
        std::vector<int> cells(static_cast<std::size_t>(kCells), 0);
        std::vector<int> lastSeq(static_cast<std::size_t>(kChasers) + 1, -1);
        long long executedPlain = 0;

        std::atomic<int> inFlight{0};
        std::atomic<int> maxInFlight{0};
        std::atomic<int> outOfOrder{0};
        std::atomic<int> completed{0};
        // Round number whose pilot task has finished its body. Release/acquire:
        // the chasers must not start posting for round r before it is set.
        std::atomic<int> gate{0};

        auto body = [&](int producer, int seq) {
            int const cur = inFlight.fetch_add(1) + 1;
            int prev = maxInFlight.load();
            while (cur > prev && !maxInFlight.compare_exchange_weak(prev, cur)) {
            }
            auto const slot = static_cast<std::size_t>(producer);
            if (seq <= lastSeq[slot]) {
                outOfOrder.fetch_add(1);
            }
            lastSeq[slot] = seq;
            for (auto& cell : cells) {
                cell += 1;
            }
            ++executedPlain;
            inFlight.fetch_sub(1);
            completed.fetch_add(1, std::memory_order_release);
        };

        {
            morph::exec::detail::StrandExecutor strand{pool};

            std::vector<std::thread> chasers;
            chasers.reserve(kChasers);
            for (int chaser = 0; chaser < kChasers; ++chaser) {
                chasers.emplace_back([&, chaser] {
                    int const producer = chaser + 1;
                    // Per-thread LCG, so the stagger below differs per thread
                    // and per round without pulling in <random> or a shared
                    // engine that would itself synchronise the threads.
                    auto rng = (static_cast<unsigned>(chaser) * 2654435761U) + 1U;
                    for (int round = 0; round < kRounds; ++round) {
                        // Spin rather than yield: the window this case aims at
                        // is a few instructions wide, and a yield overshoots it
                        // by orders of magnitude. The periodic yield is only a
                        // starvation guard for hosts with fewer cores than this
                        // case has threads (a CI runner has four); it fires once
                        // per 4096 spins, so it costs the rendezvous nothing.
                        for (unsigned spins = 0; gate.load(std::memory_order_acquire) <= round; ++spins) {
                            if ((spins & 0xFFFU) == 0xFFFU) {
                                std::this_thread::yield();
                            }
                        }
                        for (int post = 0; post < kBurst; ++post) {
                            rng = (rng * 1664525U) + 1013904223U;
                            int const stagger = static_cast<int>((rng >> 16U) & 0x3FU);
                            for (int step = 0; step < stagger; ++step) {
                                // Busy work, folded back into `rng` so it cannot
                                // be optimised away, walking this post to a
                                // different offset inside the drain window.
                                rng = (rng * 1103515245U) + 12345U;
                            }
                            int const seq = (round * kBurst) + post;
                            strand.post(key, [&body, producer, seq] { body(producer, seq); });
                        }
                    }
                });
            }

            for (int round = 0; round < kRounds; ++round) {
                strand.post(key, [&body, &gate, round] {
                    body(0, round);
                    // Published last: the chasers' posts have to arrive while
                    // the drain that follows this body is running, not before.
                    gate.store(round + 1, std::memory_order_release);
                });
                // Wait out the round rather than pipelining it. This is the
                // quiet moment -- the strand drains to empty here, which is the
                // only state from which the erase can fire at all.
                while (completed.load(std::memory_order_acquire) < (round + 1) * kPerRound) {
                    std::this_thread::yield();
                }
            }

            for (auto& chaser : chasers) {
                chaser.join();
            }
            // Closing this scope runs `~StrandExecutor`, which blocks until
            // `_inFlight == 0`; see the case above for why that is a complete
            // drain and not a deadline.
        }

        constexpr int kExpected = kRounds * kPerRound;
        INFO("iteration " << iter << ": completed " << completed.load() << " of " << kExpected);
        // `CHECK`, not `REQUIRE`, for everything but the last line: each of
        // these answers a different question about the same run and stopping at
        // the first one would hide the others (see the case above).
        CHECK(completed.load() == kExpected);
        // Lost updates to the plain state: the sanitizer-free reading of the
        // same defect the TSan legs see as a data race on it.
        CHECK(executedPlain == static_cast<long long>(kExpected));
        auto const [lowest, highest] = std::ranges::minmax_element(cells);
        CHECK(*lowest == kExpected);
        CHECK(*highest == kExpected);
        // FIFO per key is part of the contract, and an orphaned strand breaks it
        // without any two tasks having to overlap.
        CHECK(outOfOrder.load() == 0);
        REQUIRE(maxInFlight.load() == 1);
    }
}

// The recycled map node, which neither case above can be wrong
// about.
//
// When a strand drains, `scheduleNext` no longer `erase`s the map entry: it
// `extract`s it into a single-slot `_spare`, and the next `post()` that misses
// re-keys that node and inserts it back. Re-keying is the new step, and it is
// the one a functional test does not see. An entry left under the *previous*
// key still serialises every task that reaches it, still runs them in order,
// and still completes them all; what it corrupts is which key the map answers
// for, and that only becomes a serialisation failure two posts later:
//
//   1. Key A drains, parking a node still keyed A.
//   2. Key B misses and takes that node -- which, unkeyed, goes back into the
//      map under A. B's first task starts running on a strand the map calls A,
//      so `find(B)` still misses.
//   3. B's *next* post therefore misses too, and installs a second strand for
//      B while the first is still running its task. Two strands for one key:
//      the invariant the two cases above exist for, reached through a door
//      neither of them opens.
//
// Step 3 is what the shape below is for, and it is why this case is not simply
// "post to several keys and let them drain". The mis-key is only observable
// while a task is *still running* on the mis-keyed strand, so each round posts
// a short burst back-to-back -- the second and third posts of a burst arrive
// while the first is running, which on the correct code is an ordinary re-arm
// and on the mutant is a second strand. Between rounds the key is allowed to
// go quiet, which is what produces the drain step 1 needs; several keys
// running this cycle out of phase is what carries a parked node from one key
// to another. Measured, not assumed: with `_spare.key() = key;` deleted, this
// case fails 10/10 under ThreadSanitizer, while a variant that drained between
// every single post (no burst) passed 10/10 against the same mutant.
//
// Same three detectors as the case above, kept per key: an in-flight counter
// for the symptom, plain (non-atomic) per-key state for the data race a
// sanitizer sees whether or not the two tasks overlap in wall clock, and a
// per-key FIFO check for an ordering break that needs no overlap at all.
TEST_CASE("StrandExecutor recycles a drained strand under the key that asked for it", "[strand][race]") {
    constexpr std::size_t kKeys = 3;
    constexpr int kBurst = 4;
    constexpr int kRounds = 900;
    constexpr int kIterations = 6;
    constexpr std::size_t kCells = 24;

    for (int iter = 0; iter < kIterations; ++iter) {
        // Same ordering rule as the cases above: the pool outlives the strand.
        morph::exec::ThreadPoolExecutor pool{4};

        // Declared outside the strand's scope so the drain in `~StrandExecutor`
        // runs their final updates against live objects.
        std::array<std::atomic<int>, kKeys> inFlight{};
        std::array<std::atomic<int>, kKeys> maxInFlight{};
        std::array<std::atomic<int>, kKeys> completed{};
        std::array<std::atomic<int>, kKeys> outOfOrder{};
        // Plain, per key, and touched only by that key's tasks: distinct
        // objects, so a sanitizer report here means two tasks for the *same*
        // key raced, never two keys sharing a cache line. Under the invariant
        // this file exists for, the strand is the synchronisation and these
        // accesses are data-race-free.
        std::array<std::array<int, kCells>, kKeys> cells{};
        std::array<int, kKeys> lastSeq{};

        {
            morph::exec::detail::StrandExecutor strand{pool};

            std::vector<std::thread> producers;
            producers.reserve(kKeys);
            for (std::size_t slot = 0; slot < kKeys; ++slot) {
                lastSeq.at(slot) = -1;
                producers.emplace_back([&, slot] {
                    // Distinct, non-zero ids: 0 is `ModelId`'s reserved
                    // "unbound" sentinel.
                    morph::exec::detail::ModelId const key{100 + slot};
                    for (int round = 0; round < kRounds; ++round) {
                        for (int post = 0; post < kBurst; ++post) {
                            int const seq = (round * kBurst) + post;
                            strand.post(key, [&, slot, seq] {
                                int const cur = inFlight.at(slot).fetch_add(1) + 1;
                                int prev = maxInFlight.at(slot).load();
                                while (cur > prev && !maxInFlight.at(slot).compare_exchange_weak(prev, cur)) {
                                }
                                if (seq <= lastSeq.at(slot)) {
                                    outOfOrder.at(slot).fetch_add(1);
                                }
                                lastSeq.at(slot) = seq;
                                // A little plain work rather than none: an
                                // empty task gives an overlap a window a few
                                // instructions wide, which is how a broken
                                // strand can still report `maxInFlight == 1`.
                                for (auto& cell : cells.at(slot)) {
                                    cell += 1;
                                }
                                inFlight.at(slot).fetch_sub(1);
                                completed.at(slot).fetch_add(1, std::memory_order_release);
                            });
                        }
                        // Let this key go quiet before the next burst: the
                        // drain that parks a node in `_spare` only fires from
                        // an empty pending queue, and without this gap every
                        // post after the first re-arms a strand that is
                        // already in the map and the install path is never
                        // reached at all.
                        while (completed.at(slot).load(std::memory_order_acquire) < (round + 1) * kBurst) {
                            std::this_thread::yield();
                        }
                    }
                });
            }
            for (auto& producer : producers) {
                producer.join();
            }
            // Closing this scope runs `~StrandExecutor`, which blocks until
            // `_inFlight == 0`; see the first case for why that is a complete
            // drain and not a deadline.
        }

        constexpr int kPerKey = kRounds * kBurst;
        for (std::size_t slot = 0; slot < kKeys; ++slot) {
            INFO("iteration " << iter << ", key slot " << slot << ": completed " << completed.at(slot).load() << " of "
                              << kPerKey);
            // `CHECK` for the counts, `REQUIRE` for the invariant: they answer
            // different questions and stopping at the first would hide the
            // others (see the first case).
            CHECK(completed.at(slot).load() == kPerKey);
            auto const [lowest, highest] = std::ranges::minmax_element(cells.at(slot));
            CHECK(*lowest == kPerKey);
            CHECK(*highest == kPerKey);
            CHECK(outOfOrder.at(slot).load() == 0);
            REQUIRE(maxInFlight.at(slot).load() == 1);
        }
    }
}

// Regression test for ThreadPoolExecutor(0): a zero-worker pool used to accept
// tasks that could never run, hanging every post() forever. The constructor now
// clamps the worker count to at least 1, so a pool built with 0 is still usable.
TEST_CASE("ThreadPoolExecutor(0) yields a usable pool", "[executor][race]") {
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
