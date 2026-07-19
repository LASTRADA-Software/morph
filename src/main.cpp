// SPDX-License-Identifier: Apache-2.0

#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <iostream>
#include <thread>

// NOLINTBEGIN(misc-use-internal-linkage)
struct IncrementAction {
    int by = 0;
};
struct FailingAction {};

struct CounterModel {
    int value = 0;
    int execute(IncrementAction action) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        value += action.by;
        return value;
    }
    int execute(FailingAction) { throw std::runtime_error("intentional failure from FailingAction"); }
};
// NOLINTEND(misc-use-internal-linkage)

BRIDGE_REGISTER_MODEL(CounterModel, "CounterModel")
BRIDGE_REGISTER_ACTION(CounterModel, IncrementAction, "IncrementAction")
BRIDGE_REGISTER_ACTION(CounterModel, FailingAction, "FailingAction")

namespace {
void runScenario(morph::bridge::Bridge& bridge, morph::exec::MainThreadExecutor& gui, const char* label) {
    std::cout << "\n===== " << label << " =====\n";
    morph::bridge::BridgeHandler<CounterModel> handler{bridge, &gui};
    std::atomic<int> successCount{0};

    for (int idx = 0; idx < 5; ++idx) {
        handler.execute(IncrementAction{idx + 1})
            .then([&](int val) {
                std::cout << "[gui] counter = " << val << "\n";
                successCount.fetch_add(1);
            })
            .onError([](const std::exception_ptr& exc) {
                try {
                    std::rethrow_exception(exc);
                } catch (const std::exception& ex) {
                    std::cout << "[gui] unexpected: " << ex.what() << "\n";
                }
            });
    }

    handler.execute(FailingAction{})
        .then([](int) { std::cout << "[gui] (should not fire)\n"; })
        .onError([](const std::exception_ptr& exc) {
            try {
                std::rethrow_exception(exc);
            } catch (const std::exception& ex) {
                std::cout << "[gui] caught: " << ex.what() << "\n";
            }
        });

    handler.execute(FailingAction{}).then([](int) noexcept {});

    gui.runFor(std::chrono::milliseconds(500));
    std::cout << "[main] " << successCount.load() << " increments completed\n";
}
}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
int main() {
    morph::exec::ThreadPoolExecutor clientPool{4};
    morph::exec::ThreadPoolExecutor serverPool{4};
    morph::exec::MainThreadExecutor gui;

    {
        morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(clientPool)};
        runScenario(bridge, gui, "morph::backend::LocalBackend");
    }
    {
        auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
        morph::bridge::Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server)};
        runScenario(bridge, gui, "morph::backend::SimulatedRemoteBackend");
    }
    return 0;
}
