// SPDX-License-Identifier: Apache-2.0
//
// The offline queue over a real socket: a document's queued mutation made while the server is down is held by
// ui::QueueingDispatcher, and replayed in order once the backend has reconnected and the bridge has re-bound the
// model, against a server that kept its state or one that restarted fresh.

#include <QCoreApplication>
#include <QEventLoop>
#include <QUrl>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/qt/qt_executor.hpp>
#include <morph/qt/qt_websocket_backend.hpp>
#include <morph/qt/qt_websocket_server.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/queueing_dispatcher.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// At namespace scope, not in an anonymous namespace: glaze's reflection needs external linkage on the type.
struct OfflineQueueAppend {
    int value = 0;
};

// Records every value it executes, process-wide, so a test sees the order the server ran the replays in whichever
// RemoteServer instance ran them.
struct OfflineQueueLogModel {
    static std::mutex& mutex() {
        static std::mutex instance;
        return instance;
    }
    static std::vector<int>& log() {
        static std::vector<int> instance;
        return instance;
    }

    int execute(OfflineQueueAppend action) {
        std::scoped_lock const lock{mutex()};
        log().push_back(action.value);
        return action.value;
    }
};

BRIDGE_REGISTER_MODEL(OfflineQueueLogModel, "OfflineQueueLogModel")
BRIDGE_REGISTER_ACTION(OfflineQueueLogModel, OfflineQueueAppend, "OfflineQueueAppend")

namespace {

using namespace std::chrono_literals;

// Pumps the Qt event loop until @p done holds or @p budget elapses.
[[nodiscard]] bool pumpUntil(std::function<bool()> const& done, std::chrono::milliseconds budget = 10s) {
    auto const deadline = std::chrono::steady_clock::now() + budget;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

[[nodiscard]] std::vector<int> executed() {
    std::scoped_lock const lock{OfflineQueueLogModel::mutex()};
    return OfflineQueueLogModel::log();
}

// One call's outcome, filled on the owner.
struct Outcome {
    std::optional<std::string> reply;
    bool failed = false;
};

void track(morph::async::Completion<std::string> completion, std::shared_ptr<Outcome> const& outcome) {
    completion.then([outcome](std::string const& reply) { outcome->reply = reply; })
        .onError([outcome](std::exception_ptr const&) { outcome->failed = true; });
}

}  // namespace

TEST_CASE(
    "ui::QueueingDispatcher over a real socket: calls queued while the server is down replay in order once "
    "it is back",
    "[qt][ws][offline]") {
    REQUIRE(QCoreApplication::instance() != nullptr);
    bool const freshServer = GENERATE(false, true);
    CAPTURE(freshServer);
    {
        std::scoped_lock const lock{OfflineQueueLogModel::mutex()};
        OfflineQueueLogModel::log().clear();
    }

    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    auto wsServer = std::make_unique<morph::qt::QtWebSocketServer>(*server, quint16{0});
    REQUIRE(wsServer->listen());
    quint16 const port = wsServer->port();

    auto backend = std::make_unique<morph::qt::QtWebSocketBackend>(
        QUrl{QString("ws://127.0.0.1:%1").arg(port)},
        morph::qt::QtWebSocketBackend::Config{.initialReconnectDelay = 50ms, .maxReconnectDelay = 200ms});
    REQUIRE(backend->waitForConnected());
    morph::qt::QtExecutor owner;
    morph::bridge::Bridge bridge{std::move(backend), owner};

    morph::ui::BridgeDispatcher inner{bridge, &owner};
    morph::ui::QueueingDispatcher queue{inner, &owner};
    auto const channel = queue.bind("OfflineQueueLogModel", morph::bridge::BindSharing::Private, "");
    morph::ui::CallOptions const queued{.queueOffline = true};

    auto const online = std::make_shared<Outcome>();
    track(channel->execute("OfflineQueueAppend", R"({"value":1})", queued), online);
    REQUIRE(pumpUntil([&] { return online->reply.has_value() || online->failed; }));
    CHECK(online->reply == "1");
    CHECK(queue.held() == 0U);

    // The server goes away.
    wsServer->close();
    wsServer.reset();
    if (freshServer) {
        server.reset();
    }

    auto const first = std::make_shared<Outcome>();
    auto const second = std::make_shared<Outcome>();
    track(channel->execute("OfflineQueueAppend", R"({"value":2})", queued), first);
    REQUIRE(pumpUntil([&] { return queue.held() == 1U; }));
    track(channel->execute("OfflineQueueAppend", R"({"value":3})", queued), second);
    CHECK(queue.held() == 2U);
    CHECK_FALSE(first->reply.has_value());
    CHECK_FALSE(first->failed);

    // A replay while it is still down holds everything.
    queue.replay();
    static_cast<void>(pumpUntil([] { return false; }, 200ms));  // give the refused replay time to settle
    CHECK(queue.held() == 2U);
    CHECK_FALSE(first->failed);

    // The server comes back on the same port: the backend reconnects, the bridge re-binds, and replays land.
    if (freshServer) {
        server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    }
    wsServer = std::make_unique<morph::qt::QtWebSocketServer>(*server, port);
    REQUIRE(wsServer->listen());
    REQUIRE(pumpUntil([&] {
        queue.replay();
        return second->reply.has_value() || second->failed;
    }));

    CHECK(first->reply == "2");
    CHECK(second->reply == "3");
    CHECK(queue.held() == 0U);
    CHECK(executed() == std::vector<int>{1, 2, 3});

    // And the channel keeps working as an ordinary one.
    auto const after = std::make_shared<Outcome>();
    track(channel->execute("OfflineQueueAppend", R"({"value":4})", {}), after);
    REQUIRE(pumpUntil([&] { return after->reply.has_value() || after->failed; }));
    CHECK(after->reply == "4");
}
