// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <morph/core/strand.hpp>
#include <stdexcept>
#include <vector>

#include "testkit/strand_interleaver.hpp"

TEST_CASE("DeterministicExecutor runs same-key strand tasks in FIFO order under a scripted interleaving",
          "[ladder][testkit][strand-interleaver]") {
    morph::ladder::testkit::DeterministicExecutor det;
    morph::exec::detail::StrandExecutor strand{det};

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
    // (StrandExecutor's own guarantee); otherKey's task may interleave
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

TEST_CASE("DeterministicExecutor::runSchedule forces a non-default interleaving across two StrandExecutor keys",
          "[ladder][testkit][strand-interleaver]") {
    // Plain FIFO draining (the previous test case) happens to run `key`'s
    // two tasks with `otherKey`'s task landing *between* them, because
    // StrandExecutor::post appends a same-key continuation to the *back* of
    // the base executor's queue rather than re-running it immediately: after
    // posting key/otherKey/key, the DeterministicExecutor's queue holds only
    // two entries — [keyTask1, otherKeyTask] — since the second `key` post
    // finds the strand already running and just enqueues onto the strand's
    // own pending list rather than posting a third entry to `det`. Stepping
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
    // and StrandExecutor re-arms the strand).
    morph::ladder::testkit::DeterministicExecutor det;
    morph::exec::detail::StrandExecutor strand{det};

    std::vector<int> order;
    morph::exec::detail::ModelId key{1};
    morph::exec::detail::ModelId otherKey{2};

    strand.post(key, [&] { order.push_back(1); });
    strand.post(otherKey, [&] { order.push_back(100); });
    strand.post(key, [&] { order.push_back(2); });

    // det's queue right now: [0] = key's first-task dispatch, [1] = otherKey's
    // dispatch. key's second task is not queued on `det` yet — it is sitting
    // in the strand's own pending list, waiting for the strand to be re-armed.
    REQUIRE(det.pending() == 2);

    // Step 1: run index 0 (key's first task). This both runs task 1 *and*
    // causes StrandExecutor to re-arm the key strand, appending a new
    // dispatch to the back of det's queue — so afterwards det's queue is
    // [otherKey's dispatch, key's second-task dispatch].
    //
    // Step 2: run index 1 — *not* index 0 — to run key's second-task
    // dispatch (the one that only just appeared) ahead of otherKey's,
    // deliberately keeping key's two tasks contiguous.
    //
    // Step 3: only otherKey's dispatch is left, at index 0.
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
