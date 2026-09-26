// SPDX-License-Identifier: Apache-2.0
//
// Catch2 (own main, controls QCoreApplication lifetime like
// tests/qt/test_qt_websocket.cpp) coverage of MultiModelBridgeCore and
// MultiModelFormsControllerCore: routing an action-type id to whichever of
// several registered models serves it, generically over servesAction/
// executeJson, proving the core that examples/bookmarks' FormsBridge now
// composes directly.

#include <QCoreApplication>
#include <QEventLoop>
#include <atomic>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <morph/core/registry.hpp>
#include <morph/qt/bridge/multi_model_bridge_core.hpp>
#include <morph/qt/forms/multi_model_forms_controller_core.hpp>
#include <string>
#include <testkit/log_level.hpp>
#include <thread>
#include <utility>

// Deliberately at file scope, NOT inside an anonymous namespace: glaze's
// reflection forms an `extern const T external;` declaration for each
// registered type, which requires external linkage -- same constraint
// test_forms_controller_core.cpp's own file-scope models exist for. This is
// its own standalone executable (morph_multi_model_bridge_core_tests), never
// linked into the shared morph_tests binary, so there is no ODR risk from
// other translation units reusing these names.

struct PingAction {
    std::string text;
};

class PingModel {
public:
    std::string execute(const PingAction& action) { return "ping: " + action.text; }
};

BRIDGE_REGISTER_MODEL(PingModel, "MultiCore_PingModel")
BRIDGE_REGISTER_ACTION(PingModel, PingAction, "MultiCore_PingAction")

struct PongAction {
    std::string text;
};

struct PongOptions {};

struct PongOptionsResult {
    std::string value;
};

class PongModel {
public:
    std::string execute(const PongAction& action) { return "pong: " + action.text; }
    PongOptionsResult execute(const PongOptions&) { return PongOptionsResult{.value = "pong-option"}; }
};

BRIDGE_REGISTER_MODEL(PongModel, "MultiCore_PongModel")
BRIDGE_REGISTER_ACTION(PongModel, PongAction, "MultiCore_PongAction")
BRIDGE_REGISTER_ACTION(PongModel, PongOptions, "MultiCore_PongOptions")

// A third model, used only by the three-model routing case below, so that
// case is not indistinguishable from the two-model ones.
struct BuzzAction {
    std::string text;
};

class BuzzModel {
public:
    std::string execute(const BuzzAction& action) { return "buzz: " + action.text; }
};

BRIDGE_REGISTER_MODEL(BuzzModel, "MultiCore_BuzzModel")
BRIDGE_REGISTER_ACTION(BuzzModel, BuzzAction, "MultiCore_BuzzAction")

namespace {

void pumpUntil(const std::function<bool()>& done, int maxIterations = 300) {
    for (int idx = 0; idx < maxIterations && !done(); ++idx) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

/// @brief One dispatch round trip, run to completion.
struct RunResult {
    std::string reply;         ///< The success payload, empty on failure.
    std::exception_ptr error;  ///< Set on failure, null on success.

    /// @return `error`'s `what()`, or `""` if this run succeeded.
    [[nodiscard]] std::string errorText() const {
        if (!error) {
            return {};
        }
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& e) {
            return e.what();
        }
    }
};

/// @brief Invokes @p dispatch with a matching `(onReply, onError)` pair, pumps
///        the event loop until one of them settles it, and returns the
///        outcome -- the `atomic<bool>`/`pumpUntil`/capture boilerplate every
///        case below would otherwise repeat.
/// @param dispatch Callable taking `(onReply, onError)`, e.g.
///        `[&](auto onReply, auto onError){ core.execute("Action", "{}", onReply, onError); }`.
template <typename Dispatch>
[[nodiscard]] RunResult runSync(Dispatch&& dispatch) {
    std::atomic<bool> done{false};
    RunResult result;
    dispatch(
        [&](std::string resultJson) {
            result.reply = std::move(resultJson);
            done.store(true);
        },
        [&](const std::exception_ptr& err) {
            result.error = err;
            done.store(true);
        });
    pumpUntil([&] { return done.load(); });
    return result;
}

}  // namespace

TEST_CASE("MultiModelBridgeCore routes to whichever Model serves the action, in either order",
          "[multi_model_bridge_core]") {
    morph::qt::bridge::MultiModelBridgeCore<morph::bridge::NoSharing, PingModel, PongModel> core;

    CHECK(runSync([&](auto onReply, auto onError) {
              core.execute("MultiCore_PongAction", R"({"text":"hi"})", onReply, onError);
          }).reply == R"("pong: hi")");

    CHECK(runSync([&](auto onReply, auto onError) {
              core.execute("MultiCore_PingAction", R"({"text":"there"})", onReply, onError);
          }).reply == R"("ping: there")");
}

TEST_CASE("MultiModelBridgeCore routes across three Models, not just the first two tried",
          "[multi_model_bridge_core]") {
    morph::qt::bridge::MultiModelBridgeCore<morph::bridge::NoSharing, PingModel, PongModel, BuzzModel> core;

    CHECK(runSync([&](auto onReply, auto onError) {
              core.execute("MultiCore_BuzzAction", R"({"text":"last"})", onReply, onError);
          }).reply == R"("buzz: last")");
}

TEST_CASE("MultiModelBridgeCore reports an unrouted action type via onError, not a thrown exception",
          "[multi_model_bridge_core]") {
    morph::qt::bridge::MultiModelBridgeCore<morph::bridge::NoSharing, PingModel, PongModel> core;

    // Delivered synchronously, on the same call frame -- runSync's pumpUntil
    // returns on its very first check, since an unrouted action never reaches
    // a Completion.
    const RunResult result =
        runSync([&](auto onReply, auto onError) { core.execute("NoSuchAction", "{}", onReply, onError); });
    CHECK(result.error != nullptr);
    CHECK(result.errorText() == "no model in this client serves action 'NoSuchAction'");
}

TEST_CASE("MultiModelBridgeCore composes over a caller-supplied Bridge/executor", "[multi_model_bridge_core]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::qt::QtExecutor gui;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};

    morph::qt::bridge::MultiModelBridgeCore<morph::bridge::NoSharing, PingModel, PongModel> core{bridge, &gui};

    CHECK(runSync([&](auto onReply, auto onError) {
              core.execute("MultiCore_PingAction", R"({"text":"composed"})", onReply, onError);
          }).reply == R"("ping: composed")");
}

TEST_CASE("MultiModelFormsControllerCore submits and fetches options through the routed dispatch",
          "[multi_model_bridge_core]") {
    morph::qt::forms::MultiModelFormsControllerCore<morph::bridge::NoSharing, PingModel, PongModel> core{
        R"({"MultiCore_PingAction":{},"MultiCore_PongAction":{}})"};
    CHECK(core.schemasJson() == R"({"MultiCore_PingAction":{},"MultiCore_PongAction":{}})");

    CHECK(runSync([&](auto onReply, auto onError) {
              core.submitIfValid("MultiCore_PingAction", R"({"text":"submit"})", onReply, onError);
          }).reply == R"("ping: submit")");

    const RunResult options = runSync(
        [&](auto onReply, auto onError) { core.fetchOptions("MultiCore_PongOptions", "{}", onReply, onError); });
    CHECK(options.reply.contains("pong-option"));
}

int main(int argc, char** argv) {
    const QCoreApplication app{argc, argv};
    Catch::Session session;
    return morph::testkit::runSession(session, argc, argv);
}
