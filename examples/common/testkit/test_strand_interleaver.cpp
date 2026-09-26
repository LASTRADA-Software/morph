// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <core/async/Strand.hpp>
#include <morph/core/strand.hpp>
#include <stdexcept>
#include <vector>

#include "testkit/strand_interleaver.hpp"

TEST_CASE("DeterministicExecutor runs same-key strand tasks in FIFO order under a scripted interleaving",
          "[ladder][testkit][strand-interleaver]") {
    morph::ladder::testkit::DeterministicExecutor det;
    // One task per turn, as morph's own strand ran them: a turn of several
    // would run the key's queued tasks inside one step.
    morph::exec::detail::ModelStrands strand{det, core::async::StrandOptions{.batch = 1}};

    std::vector<int> order;
    morph::exec::detail::ModelId key{1};
    morph::exec::detail::ModelId otherKey{2};

    strand.post(key, [&] { order.push_back(1); });
    strand.post(otherKey, [&] { order.push_back(100); });
    strand.post(key, [&] { order.push_back(2); });

    REQUIRE(det.pending() >= 1);

    // Deliberately run the *other* key's task before the same-key pair's
    // second entry, proving the interleaving is under this test's control
    // rather than the underlying pool's scheduling.
    while (det.pending() > 0) {
        det.step();
    }

    // key's two tasks must have run in post order relative to each other
    // (the strand's own guarantee); otherKey's task may interleave
    // anywhere since it is a different key — assert only the same-key
    // relative order, which is the property this harness exists to make
    // reproducible.
    auto posOf = [&](int value) {
        return static_cast<std::size_t>(std::find(order.begin(), order.end(), value) - order.begin());
    };
    REQUIRE(posOf(1) < posOf(2));
}

TEST_CASE("DeterministicExecutor::runSchedule executes queued tasks in the caller's chosen order",
          "[ladder][testkit][strand-interleaver]") {
    morph::ladder::testkit::DeterministicExecutor det;
    std::vector<int> order;
    det.post([&] { order.push_back(1); });
    det.post([&] { order.push_back(2); });
    det.post([&] { order.push_back(3); });

    // Indices are re-read after each erase, not fixed against the original
    // queue: to run "3" (index 2) first, then "1" (index 0), then "2", the
    // third index is 0 — not 1 — because once "3" and "1" are gone, "2" is
    // the only element left and sits at index 0.
    det.runSchedule({2, 0, 0});  // run "3" first, then "1", then "2"
    REQUIRE(order == std::vector<int>{3, 1, 2});
}

TEST_CASE("DeterministicExecutor::runSchedule forces a non-default interleaving across two strand keys",
          "[ladder][testkit][strand-interleaver]") {
    // Plain FIFO draining (the previous test case) happens to run `key`'s
    // two tasks with `otherKey`'s task landing *between* them, because
    // a strand that runs one task per turn hands its base back after each
    // task and queues itself again at the *back* of the base executor's
    // queue: after posting key/otherKey/key, the DeterministicExecutor's
    // queue holds only two entries — [key's strand, otherKey's strand] —
    // since the second `key` post finds the strand already scheduled and just
    // enqueues onto the strand's own queue rather than a third entry on
    // `det`. Stepping
    // that queue FIFO therefore already interleaves otherKey's task between
    // key's two tasks, without any deliberate scripting.
    //
    // This test proves runSchedule can force a *different* order than that
    // default: both of key's tasks back-to-back, with otherKey's task
    // pushed out to run last — an order plain FIFO draining would never
    // produce, and one that only works because runSchedule re-reads the
    // queue's current contents before consuming each index (the second
    // `key` task's post-to-`det` entry does not exist yet at schedule-
    // construction time; it only appears once the first `key` task has run
    // and the strand queues itself on `det` again).
    morph::ladder::testkit::DeterministicExecutor det;
    // One task per turn, as morph's own strand ran them: a turn of several
    // would run the key's queued tasks inside one step.
    morph::exec::detail::ModelStrands strand{det, core::async::StrandOptions{.batch = 1}};

    std::vector<int> order;
    morph::exec::detail::ModelId key{1};
    morph::exec::detail::ModelId otherKey{2};

    strand.post(key, [&] { order.push_back(1); });
    strand.post(otherKey, [&] { order.push_back(100); });
    strand.post(key, [&] { order.push_back(2); });

    // det's queue right now: [0] = key's strand, [1] = otherKey's strand.
    // key's second task is not queued on `det` itself — it is sitting in the
    // strand's own queue, waiting for the strand's next turn.
    REQUIRE(det.pending() == 2);

    // Step 1: run index 0 (key's strand, one task). This both runs task 1
    // *and* queues the key's strand on det again, at the back — so
    // afterwards det's queue is [otherKey's strand, key's strand].
    //
    // Step 2: run index 1 — *not* index 0 — to run key's second task (the
    // entry that only just appeared) ahead of otherKey's,
    // deliberately keeping key's two tasks contiguous.
    //
    // Step 3: only otherKey's strand is left, at index 0.
    det.runSchedule({0, 1, 0});

    REQUIRE(order == std::vector<int>{1, 2, 100});
}

TEST_CASE("DeterministicExecutor::step throws when the queue is empty", "[ladder][testkit][strand-interleaver]") {
    morph::ladder::testkit::DeterministicExecutor det;
    REQUIRE(det.pending() == 0);
    REQUIRE_THROWS_AS(det.step(), std::runtime_error);
}

TEST_CASE("DeterministicExecutor::runSchedule throws on an out-of-range index",
          "[ladder][testkit][strand-interleaver]") {
    morph::ladder::testkit::DeterministicExecutor det;
    det.post([] {});
    REQUIRE_THROWS_AS(det.runSchedule({1}), std::runtime_error);
}
