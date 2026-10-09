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

/// A typed shared handler on one instance, subscribed to `std::string`
/// results; counts what it receives. Opaque here: the handler's type names
/// the model.
class TypedRenameWatch;

/// Attaches a typed shared handler to the instance for @p key and subscribes it.
std::shared_ptr<TypedRenameWatch> watchRenames(morph::bridge::Bridge& bridge, morph::exec::IExecutor& owner,
                                               const std::string& key);

/// How many `std::string` results @p watch has received.
int renamesSeen(const TypedRenameWatch& watch);

/// Dispatches a typed `Raw_Rename` through @p watch's own handler.
void renameTyped(TypedRenameWatch& watch);

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
