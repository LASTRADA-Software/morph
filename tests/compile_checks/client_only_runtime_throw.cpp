// SPDX-License-Identifier: Apache-2.0
//
// Runtime-check fixture for the MORPH_CLIENT_ONLY guard's other half: proves
// that Bridge::executeVia's localOp actually throws std::logic_error under
// MORPH_CLIENT_ONLY when misused against LocalBackend, rather than silently
// calling Model::execute (see MORPH_DETAIL_REGISTER_MODEL_LOCAL's @warning in
// registry.hpp -- this is exactly the misuse scenario it warns against). Run
// via try_run() in tests/CMakeLists.txt (compiles AND executes, unlike
// try_compile()), since this needs to observe runtime behavior, not just
// link success.
//
// Unlike client_only_no_model_link.cpp, ClientOnlyRuntimeModel is fully
// defined here -- the localOp guard is a runtime throw independent of
// whether Model::execute has a definition; the point of this probe is to
// prove that throw actually fires, with the process's exit code as the
// observable result try_run() checks.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

struct InlineExecutor : morph::exec::IExecutor {
    void post(std::function<void()> fn) override { fn(); }
};

}  // namespace

struct ClientOnlyRuntimeAction {
    int x = 0;
};

struct ClientOnlyRuntimeModel {
    int execute(const ClientOnlyRuntimeAction& action) { return action.x * 2; }
};

BRIDGE_REGISTER_MODEL(ClientOnlyRuntimeModel, "ClientOnlyRuntimeModel")
BRIDGE_REGISTER_ACTION_4(ClientOnlyRuntimeModel, ClientOnlyRuntimeAction, "ClientOnlyRuntimeAction",
                         ::morph::model::Loggable::Yes)

int main() {
    morph::exec::ThreadPoolExecutor pool{2};
    InlineExecutor cbExec;
    // Deliberate misuse: MORPH_DETAIL_REGISTER_MODEL_LOCAL's @warning says
    // MORPH_CLIENT_ONLY must never be paired with LocalBackend. Exercising it
    // anyway is exactly how this probe proves the throw fires instead of
    // silently calling execute.
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<ClientOnlyRuntimeModel> handler{bridge, &cbExec};

    std::atomic<bool> gotExpectedError{false};
    std::atomic<bool> completed{false};
    handler.execute(ClientOnlyRuntimeAction{21})
        .then([&](int) { completed.store(true); })
        .onError([&](const std::exception_ptr& eptr) {
            try {
                std::rethrow_exception(eptr);
            } catch (const std::logic_error& exc) {
                std::string const what{exc.what()};
                gotExpectedError.store(what.find("MORPH_CLIENT_ONLY") != std::string::npos);
            } catch (...) {
            }
            completed.store(true);
        });

    for (int idx = 0; idx < 200 && !completed.load(); ++idx) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!completed.load()) {
        std::fputs("client_only_runtime_throw: timed out waiting for completion\n", stderr);
        return 1;
    }
    if (!gotExpectedError.load()) {
        std::fputs("client_only_runtime_throw: did not observe the expected std::logic_error\n", stderr);
        return 1;
    }
    return 0;
}
