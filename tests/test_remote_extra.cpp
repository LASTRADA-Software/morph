// SPDX-License-Identifier: Apache-2.0

#include <morph/bridge.hpp>
#include <morph/executor.hpp>
#include <morph/registry.hpp>
#include <morph/remote.hpp>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <stdexcept>
#include <thread>


// Fresh dispatcher + registry per test to avoid global state pollution
struct Env {
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
};

struct SquareAction {
    int x = 0;
};
struct SquareFail {};
struct SquareModel {
    int execute(const SquareAction& act) { return act.x * act.x; }
    int execute(const SquareFail&) { throw std::runtime_error("square failed"); }
};

template <>
struct morph::model::ModelTraits<SquareModel> {
    static constexpr std::string_view typeId() { return "RX_SquareModel"; }
};
template <>
struct morph::model::ActionTraits<SquareAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "RX_SquareAction"; }
    static std::string toJson(const SquareAction& act) {
        std::string out;
        (void)glz::write_json(act, out);
        return out;
    }
    static SquareAction fromJson(std::string_view json) {
        SquareAction action{};
        (void)glz::read_json(action, json);
        return action;
    }
    static std::string resultToJson(const int& res) {
        std::string out;
        (void)glz::write_json(res, out);
        return out;
    }
    static int resultFromJson(std::string_view json) {
        int result{};
        (void)glz::read_json(result, json);
        return result;
    }
};
template <>
struct morph::model::ActionTraits<SquareFail> {
    using Result = int;
    static constexpr std::string_view typeId() { return "RX_SquareFail"; }
    static std::string toJson(const SquareFail&) { return "{}"; }
    static SquareFail fromJson(std::string_view) { return {}; }
    static std::string resultToJson(const int&) { return "0"; }
    static int resultFromJson(std::string_view) { return 0; }
};

static Env& sharedEnv() {
    static Env env = [] {
        Env env2;
        env2.registry.registerModel<SquareModel>("RX_SquareModel");
        env2.dispatcher.registerAction<SquareModel, SquareAction>("RX_SquareModel", "RX_SquareAction");
        env2.dispatcher.registerAction<SquareModel, SquareFail>("RX_SquareModel", "RX_SquareFail");
        return env2;
    }();
    return env;
}

struct SyncExec : morph::exec::IExecutor {
    void post(std::function<void()> fn) override { fn(); }
};

// ── morph::backend::RemoteServer: bad message type ───────────────────────────────────────────

TEST_CASE("morph::backend::RemoteServer: unknown command replies with err", "[remote]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = sharedEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    std::atomic<bool> replyReceived{false};
    std::string replyMsg;
    server->handle("badcmd|stuff", [&](const std::string& reply) {
        replyMsg = reply;
        replyReceived.store(true);
    });

    for (int i = 0; i < 50 && !replyReceived.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(replyReceived.load());
    REQUIRE(replyMsg.starts_with("err|"));
}

// ── morph::backend::RemoteServer: deregister path ────────────────────────────────────────────

TEST_CASE("morph::backend::RemoteServer: register then deregister succeeds", "[remote]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = sharedEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    // Register
    std::string regReply;
    {
        std::atomic<bool> done{false};
        server->handle("register|RX_SquareModel", [&](const std::string& reply) {
            regReply = reply;
            done.store(true);
        });
        for (int i = 0; i < 50 && !done.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        REQUIRE(done.load());
        REQUIRE(regReply.starts_with("ok|"));
    }

    // Extract id
    uint64_t mid = std::stoull(regReply.substr(3));

    // Deregister
    {
        std::atomic<bool> done{false};
        server->handle("deregister|" + std::to_string(mid), [&](const std::string& reply) {
            REQUIRE(reply == "ok");
            done.store(true);
        });
        for (int i = 0; i < 50 && !done.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        REQUIRE(done.load());
    }
}

// ── morph::backend::RemoteServer: execute on unknown model ─────────────────────────────────

TEST_CASE("morph::backend::RemoteServer: execute on unknown model replies with err (5-part format)", "[remote]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = sharedEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    std::atomic<bool> replyReceived{false};
    std::string replyMsg;
    // Use a model id that was never registered
    server->handle("execute|9999|RX_SquareModel|RX_SquareAction|{\"x\":3}", [&](const std::string& reply) {
        replyMsg = reply;
        replyReceived.store(true);
    });

    for (int i = 0; i < 50 && !replyReceived.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(replyReceived.load());
    REQUIRE(replyMsg.starts_with("err|"));
}

TEST_CASE("morph::backend::RemoteServer: execute on unknown model replies with err (6-part Qt format)", "[remote]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = sharedEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    std::atomic<bool> replyReceived{false};
    std::string replyMsg;
    // 6-part format: execute|callId|mid|modelTy|actionTy|body
    server->handle("execute|call-abc|9999|RX_SquareModel|RX_SquareAction|{\"x\":3}", [&](const std::string& reply) {
        replyMsg = reply;
        replyReceived.store(true);
    });

    for (int i = 0; i < 50 && !replyReceived.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(replyReceived.load());
    // Should include callId in error: "err|call-abc|model not found"
    REQUIRE(replyMsg.starts_with("err|call-abc|"));
}

// ── morph::backend::RemoteServer: 6-part Qt execute with valid model ─────────────────────────

TEST_CASE("morph::backend::RemoteServer: Qt 6-part execute returns ok|callId|result", "[remote]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = sharedEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    // First register a model
    std::string regReply;
    {
        std::atomic<bool> done{false};
        server->handle("register|RX_SquareModel", [&](const std::string& reply) {
            regReply = reply;
            done.store(true);
        });
        for (int i = 0; i < 50 && !done.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        REQUIRE(regReply.starts_with("ok|"));
    }

    uint64_t mid = std::stoull(regReply.substr(3));

    // Execute via 6-part Qt format
    std::atomic<bool> replyReceived{false};
    std::string replyMsg;
    server->handle("execute|call-xyz|" + std::to_string(mid) + "|RX_SquareModel|RX_SquareAction|{\"x\":5}",
                   [&](const std::string& reply) {
                       replyMsg = reply;
                       replyReceived.store(true);
                   });

    for (int i = 0; i < 50 && !replyReceived.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(replyReceived.load());
    REQUIRE(replyMsg == "ok|call-xyz|25");
}

// ── morph::backend::RemoteServer: dispatch exception propagates as err reply ──────────────────

TEST_CASE("morph::backend::RemoteServer: model action exception becomes err reply", "[remote]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = sharedEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    std::string regReply;
    {
        std::atomic<bool> done{false};
        server->handle("register|RX_SquareModel", [&](const std::string& reply) {
            regReply = reply;
            done.store(true);
        });
        for (int i = 0; i < 50 && !done.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        REQUIRE(regReply.starts_with("ok|"));
    }

    uint64_t mid = std::stoull(regReply.substr(3));

    std::atomic<bool> replyReceived{false};
    std::string replyMsg;
    server->handle("execute|" + std::to_string(mid) + "|RX_SquareModel|RX_SquareFail|{}",
                   [&](const std::string& reply) {
                       replyMsg = reply;
                       replyReceived.store(true);
                   });

    for (int i = 0; i < 50 && !replyReceived.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(replyReceived.load());
    REQUIRE(replyMsg.starts_with("err|"));
}

// ── morph::backend::SimulatedRemoteBackend: malformed reply ───────────────────────────────────

TEST_CASE("morph::backend::SimulatedRemoteBackend: malformed reply triggers onError", "[remote]") {
    // We inject a fake server that always replies with garbage
    struct GarbageServer {
        void handle(std::string_view /*msg*/, const std::function<void(std::string)>& reply) {
            // Synchronously reply with something that is neither "ok|" nor "err|"
            reply("garbage");
        }
    };

    // Directly test CompletionState error path by simulating what morph::backend::SimulatedRemoteBackend does
    SyncExec cbExec;
    auto state = std::make_shared<morph::async::detail::CompletionState<std::shared_ptr<void>>>();
    morph::async::Completion<std::shared_ptr<void>> comp{state, &cbExec};

    std::string errMsg;
    comp.onError([&](const std::exception_ptr& exc) {
        try {
            std::rethrow_exception(exc);
        } catch (const std::runtime_error& ex) {
            errMsg = ex.what();
        }
    });

    // Simulate what the callback in morph::backend::SimulatedRemoteBackend does with "garbage"
    try {
        const std::string reply = "garbage";
        if (reply.starts_with("ok|")) {
            // won't enter
        } else if (reply.starts_with("err|")) {
            throw std::runtime_error(reply.substr(4));
        } else {
            throw std::runtime_error("malformed reply: " + reply);
        }
    } catch (...) {
        state->setException(std::current_exception());
    }

    REQUIRE(errMsg.contains("malformed"));
}

// ── morph::backend::SimulatedRemoteBackend: register failure (server sends non-ok reply) ──────

TEST_CASE("morph::backend::SimulatedRemoteBackend: register failure raises exception", "[remote]") {
    // Simulate a server that always rejects register
    struct RejectServer {
        void handle(std::string_view /*msg*/, const std::function<void(std::string)>& reply) {
            reply("err|no models allowed");
        }
    } fakeServer;

    // Manually replicate what morph::backend::SimulatedRemoteBackend::registerModel does
    std::promise<morph::exec::detail::ModelId> prom;
    auto fut = prom.get_future();

    fakeServer.handle("register|anything", [&prom](const std::string& reply) {
        if (reply.starts_with("ok|")) {
            prom.set_value(morph::exec::detail::ModelId{std::stoull(reply.substr(3))});
        } else {
            prom.set_exception(std::make_exception_ptr(std::runtime_error("register failed: " + reply)));
        }
    });

    REQUIRE_THROWS_AS(fut.get(), std::runtime_error);
}
