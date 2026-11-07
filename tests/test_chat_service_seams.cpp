// SPDX-License-Identifier: Apache-2.0
//
// What an envelope service gets from the server: the connection each envelope arrived on, a handle to the server, and
// word of a connection closing.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace wire = morph::wire;
using morph::backend::ConnectionId;
using morph::backend::RemoteServer;

struct Recorder final : morph::backend::IEnvelopeService {
    std::mutex mutex;
    int boundCount = 0;
    std::weak_ptr<RemoteServer> weak;
    bool serverAtFirstHandle = false;
    bool handled = false;
    ConnectionId seenCid = 12345;
    std::vector<ConnectionId> closed;
    bool throwOnClose = false;

    [[nodiscard]] bool serves(std::string_view kind) const override { return kind == "seam"; }
    [[nodiscard]] std::string handle(wire::Envelope const& request,
                                     morph::session::IAuthorizer const& /*authorizer*/) override {
        return wire::encode(wire::makeOk(request.callId));
    }
    [[nodiscard]] std::string handle(wire::Envelope const& request, morph::session::IAuthorizer const& authorizer,
                                     morph::backend::ConnectionFacts& facts) override {
        {
            std::scoped_lock const lock{mutex};
            seenCid = facts.cid;
            if (!handled) {
                handled = true;
                serverAtFirstHandle = weak.lock() != nullptr;
            }
        }
        return handle(request, authorizer);
    }
    void bound(std::weak_ptr<RemoteServer> const& server) override {
        std::scoped_lock const lock{mutex};
        ++boundCount;
        weak = server;
    }
    void connectionClosed(ConnectionId cid) override {
        std::scoped_lock const lock{mutex};
        closed.push_back(cid);
        if (throwOnClose) {
            throw std::runtime_error{"a service failed while a connection closed"};
        }
    }
};

struct Rig {
    morph::exec::ThreadPoolExecutor pool{2};
    std::shared_ptr<Recorder> service = std::make_shared<Recorder>();
    std::shared_ptr<RemoteServer> server = make();

    std::shared_ptr<RemoteServer> make() {
        morph::backend::ServerConfig config;
        config.services.push_back(service);
        return std::make_shared<RemoteServer>(pool, morph::session::allowAllAuthorizer(), std::move(config));
    }

    void ask(ConnectionId cid) {
        wire::Envelope env;
        env.kind = "seam";
        env.callId = 1;
        REQUIRE(wire::decode(server->handleInline(wire::encode(env), cid)).kind == "ok");
    }

    bool closedCount(std::size_t expected) {
        auto const deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::scoped_lock const lock{service->mutex};
                if (service->closed.size() >= expected) {
                    return true;
                }
            }
            std::this_thread::sleep_for(5ms);
        }
        return false;
    }
};

}  // namespace

TEST_CASE("a_service_sees_the_connection_id_of_its_envelope", "[chat][seams]") {
    Rig rig;
    auto const cid = rig.server->openConnection();
    rig.ask(cid);
    CHECK(rig.service->seenCid == cid);
}

TEST_CASE("an_unscoped_envelope_has_connection_id_zero", "[chat][seams]") {
    Rig rig;
    rig.ask(0);
    CHECK(rig.service->seenCid == 0);
}

TEST_CASE("a_service_is_bound_before_its_first_envelope_and_only_once", "[chat][seams]") {
    Rig rig;
    rig.ask(0);
    rig.ask(0);
    CHECK(rig.service->boundCount == 1);
    CHECK(rig.service->serverAtFirstHandle);
}

TEST_CASE("a_service_handle_does_not_keep_the_server_alive", "[chat][seams]") {
    Rig rig;
    rig.ask(0);
    REQUIRE_FALSE(rig.service->weak.expired());
    rig.server.reset();
    auto const deadline = std::chrono::steady_clock::now() + 5s;
    while (!rig.service->weak.expired() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    CHECK(rig.service->weak.expired());
}

TEST_CASE("closing_a_connection_tells_each_service_once", "[chat][seams]") {
    Rig rig;
    auto const cid = rig.server->openConnection();
    rig.ask(cid);
    rig.server->closeConnection(cid);
    rig.server->closeConnection(cid);
    REQUIRE(rig.closedCount(1));
    rig.ask(0);
    std::scoped_lock const lock{rig.service->mutex};
    REQUIRE(rig.service->closed.size() == 1);
    CHECK(rig.service->closed.front() == cid);
}

TEST_CASE("a_throwing_connectionClosed_does_not_stop_the_close", "[chat][seams]") {
    Rig rig;
    rig.service->throwOnClose = true;
    auto const first = rig.server->openConnection();
    rig.server->closeConnection(first);
    REQUIRE(rig.closedCount(1));
    auto const second = rig.server->openConnection();
    rig.ask(second);
    CHECK(rig.service->seenCid == second);
}
