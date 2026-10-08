// SPDX-License-Identifier: Apache-2.0
//
// What test_bridge_raw.cpp observes of the Task-handler fixture, without
// naming any model type.
#pragma once

#include <atomic>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/timeout_scheduler.hpp>
#include <string>

namespace rawprobe {

/// Where `Raw_Spawn` builds a raw handler, from inside a running action.
struct SpawnSlot {
    morph::bridge::Bridge* bridge = nullptr;
    morph::exec::IExecutor* gui = nullptr;
    std::unique_ptr<morph::bridge::RawHandler> handler;
};

SpawnSlot& spawnSlot();

struct Sleeper {
    std::atomic<int> started{0};
    std::atomic<int> cancelled{0};
    std::atomic<int> finished{0};
    morph::async::detail::TimeoutScheduler* scheduler = nullptr;
};

Sleeper& sleeper();

/// Owns the scheduler `Raw_Sleep` waits on for one test.
class SleeperScope {
public:
    SleeperScope() {
        sleeper().started = 0;
        sleeper().cancelled = 0;
        sleeper().finished = 0;
        sleeper().scheduler = &_scheduler;
    }
    SleeperScope(const SleeperScope&) = delete;
    SleeperScope& operator=(const SleeperScope&) = delete;
    SleeperScope(SleeperScope&&) = delete;
    SleeperScope& operator=(SleeperScope&&) = delete;
    ~SleeperScope() { sleeper().scheduler = nullptr; }

private:
    morph::async::detail::TimeoutScheduler _scheduler;
};

}  // namespace rawprobe
