// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <morph/strand.hpp>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <stdexcept>
#include <thread>


TEST_CASE("morph::exec::detail::StrandExecutor: exception in task is swallowed and next task still runs", "[strand]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::detail::StrandExecutor strand{pool};
    morph::exec::detail::ModelId key{42};

    std::atomic<bool> afterRan{false};

    strand.post(key, [] { throw std::runtime_error("strand bomb"); });
    strand.post(key, [&] { afterRan.store(true); });

    for (int i = 0; i < 50 && !afterRan.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(afterRan.load());
}

TEST_CASE("morph::exec::detail::StrandExecutor: rapid post to running strand queues correctly", "[strand]") {
    // Post many tasks to same key without waiting — exercises the "already running" branch
    morph::exec::ThreadPoolExecutor pool{4};
    morph::exec::detail::StrandExecutor strand{pool};
    morph::exec::detail::ModelId key{7};

    constexpr int numTasks = 50;
    std::atomic<int> completed{0};
    std::vector<int> order;
    std::mutex orderMtx;

    for (int i = 0; i < numTasks; ++i) {
        strand.post(key, [&, i] {
            {
                std::scoped_lock lock{orderMtx};
                order.push_back(i);
            }
            completed.fetch_add(1);
        });
    }

    for (int i = 0; i < 200 && completed.load() < numTasks; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(completed.load() == numTasks);
    // Tasks for same key must arrive in submission order
    REQUIRE(order.size() == static_cast<std::size_t>(numTasks));
    for (int i = 0; i < numTasks; ++i) {
        REQUIRE(order[static_cast<std::size_t>(i)] == i);
    }
}

TEST_CASE("morph::exec::detail::StrandExecutor: independent keys each run exactly their own tasks", "[strand]") {
    morph::exec::ThreadPoolExecutor pool{4};
    morph::exec::detail::StrandExecutor strand{pool};

    constexpr int numKeys = 3;
    constexpr int tasksPerKey = 5;
    std::array<std::atomic<int>, numKeys> results{};

    constexpr auto numKeysZ = static_cast<std::size_t>(numKeys);
    constexpr auto tasksPerKeyZ = static_cast<std::size_t>(tasksPerKey);

    for (std::size_t key = 0; key < numKeysZ; ++key) {
        for (std::size_t task = 0; task < tasksPerKeyZ; ++task) {
            strand.post(morph::exec::detail::ModelId{key + 1}, [&results, key] { results[key].fetch_add(1); });
        }
    }

    for (int i = 0; i < 100; ++i) {
        bool done = true;
        for (std::size_t key = 0; key < numKeysZ; ++key) {
            if (results[key].load() < tasksPerKey) {
                done = false;
            }
        }
        if (done) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    for (std::size_t key = 0; key < numKeysZ; ++key) {
        REQUIRE(results[key].load() == tasksPerKey);
    }
}
