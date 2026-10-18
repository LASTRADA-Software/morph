// SPDX-License-Identifier: Apache-2.0

#pragma once

// One channel conformance case, instantiated by each remote backend's suite
// (simulated, `net`, Qt). It states what the server-to-client channel promises
// a backend, independent of the transport that carries it: a posted event
// reaches the handler, is never taken for a reply, and arrives in the order it
// was posted.
//
// Include this from one translation unit per test executable: it registers the
// model its pending-call check drives.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// External linkage: glaze reflection and the BRIDGE_REGISTER_* macros need it.
// NOLINTBEGIN(misc-use-internal-linkage)
struct PcfGateAction {
    int value = 0;
};
/// Open while the model's action may finish; closed, the action holds, so a
/// call stays pending for as long as a test needs it.
inline std::atomic<bool> pcfGateOpen{true};
struct PcfGateModel {
    int execute(PcfGateAction /*action*/) {
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!pcfGateOpen.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return 7;
    }
};
// NOLINTEND(misc-use-internal-linkage)

BRIDGE_REGISTER_MODEL(PcfGateModel, "PcfGateModel")
BRIDGE_REGISTER_ACTION(PcfGateModel, PcfGateAction, "PcfGateAction")

namespace morph::testing::push {

/// What a suite hands the conformance case: a server, a backend connected to
/// it, and the id of the connection the server posts to.
struct ChannelFixture {
    /// The server that posts. It serves `PcfGateModel` from the global registry.
    ::morph::backend::RemoteServer* server = nullptr;
    /// The connection @p backend is on, as `server` numbers it.
    ::morph::backend::ConnectionId cid = 0;
    /// The client backend under test, already connected.
    ::morph::backend::detail::IBackend* backend = nullptr;
    /// Runs the transport until @p done holds or the suite's deadline passes.
    /// @return Whether @p done held.
    std::function<bool(const std::function<bool()>& done)> waitFor;
};

/// Builds a fixture, runs the body it is given, then tears the fixture down.
using FixtureFactory = std::function<void(const std::function<void(ChannelFixture&)>& body)>;

namespace detail {

/// Runs each task inline on the posting thread.
struct InlineExec final : ::morph::exec::IExecutor {
    void post(std::function<void()> task) override { task(); }
};

/// What an event handler saw, from whichever thread delivered it.
struct Events {
    std::mutex mtx;
    std::vector<::morph::wire::EventBody> seen;

    std::function<void(::morph::wire::EventBody)> handler() {
        return [this](::morph::wire::EventBody body) {
            std::scoped_lock const lock{mtx};
            seen.push_back(std::move(body));
        };
    }
    std::size_t size() {
        std::scoped_lock const lock{mtx};
        return seen.size();
    }
    std::vector<::morph::wire::EventBody> snapshot() {
        std::scoped_lock const lock{mtx};
        return seen;
    }
};

/// Closes the gate on construction and opens it again on destruction, so a
/// failing check cannot leave a server action holding.
struct GateClosed {
    GateClosed() { pcfGateOpen.store(false); }
    ~GateClosed() { pcfGateOpen.store(true); }
    GateClosed(const GateClosed&) = delete;
    GateClosed& operator=(const GateClosed&) = delete;
};

inline std::string eventJson(std::size_t n) { return "{\"n\":" + std::to_string(n) + "}"; }

}  // namespace detail

/// @brief The channel promises every remote backend keeps, checked through
///        the transport @p make builds.
///
/// Three checks, each on a fresh fixture: a posted event reaches the handler;
/// an event carrying the call id of a call still in flight reaches the handler
/// and does not settle that call, whose real reply still does; and the events
/// of one connection arrive in the order they were posted.
/// @param make Builds the transport and runs the body it is handed.
inline void channelConformance(const FixtureFactory& make) {
    SECTION("a posted event reaches the handler") {
        make([](ChannelFixture& fx) {
            detail::InlineExec exec;
            detail::Events log;
            fx.backend->setEventHandler(log.handler(), &exec);

            fx.server->post(fx.cid, ::morph::wire::encode(::morph::wire::makeEvent("changes", R"({"n":1})")));
            REQUIRE(fx.waitFor([&] { return log.size() == 1; }));
            auto const got = log.snapshot();
            CHECK(got[0].topic == "changes");
            CHECK(got[0].payload.str == R"({"n":1})");
        });
    }

    SECTION("an event is not taken for a reply") {
        make([](ChannelFixture& fx) {
            detail::InlineExec exec;
            detail::Events log;
            fx.backend->setEventHandler(log.handler(), &exec);

            // Completions settle on their owner, which is drained from the
            // thread that waits.
            ::morph::exec::MainThreadExecutor owner;
            auto const waitFor = [&fx, &owner](const std::function<bool()>& done) {
                return fx.waitFor([&] {
                    owner.drain();
                    return done();
                });
            };

            std::atomic<std::uint64_t> modelId{0};
            fx.backend->bindModel({.typeId = "PcfGateModel"}, owner).then([&](::morph::exec::detail::ModelId mid) {
                modelId.store(mid.v);
            });
            REQUIRE(waitFor([&] { return modelId.load() != 0; }));

            detail::GateClosed const held;
            ::morph::backend::detail::ActionCall call;
            call.modelTypeId = "PcfGateModel";
            call.actionTypeId = "PcfGateAction";
            call.serializeAction = [](const void*) { return std::string{"{}"}; };
            call.deserializeResult = [](std::string_view body) -> std::shared_ptr<void> {
                return std::make_shared<int>(std::stoi(std::string{body}));
            };
            std::atomic<int> result{-1};
            std::atomic<bool> failed{false};
            fx.backend->execute(::morph::exec::detail::ModelId{modelId.load()}, std::move(call), &owner)
                .then([&](const std::shared_ptr<void>& value) { result.store(*std::static_pointer_cast<int>(value)); })
                .onError([&](const std::exception_ptr&) { failed.store(true); });

            // The call's id is the transport's to choose, so the events carry
            // every id a first call could have been filed under.
            constexpr std::size_t kIds = 8;
            for (std::size_t id = 0; id <= kIds; ++id) {
                auto env = ::morph::wire::makeEvent("changes", detail::eventJson(id));
                env.callId = id;
                fx.server->post(fx.cid, ::morph::wire::encode(env));
            }
            REQUIRE(waitFor([&] { return log.size() == kIds + 1; }));
            CHECK(result.load() == -1);
            CHECK_FALSE(failed.load());

            // The call is still pending, and its own reply settles it.
            pcfGateOpen.store(true);
            REQUIRE(waitFor([&] { return result.load() != -1 || failed.load(); }));
            CHECK(result.load() == 7);
            CHECK_FALSE(failed.load());
        });
    }

    SECTION("events on one connection arrive in posting order") {
        make([](ChannelFixture& fx) {
            detail::InlineExec exec;
            detail::Events log;
            fx.backend->setEventHandler(log.handler(), &exec);

            constexpr std::size_t kEvents = 50;
            for (std::size_t n = 0; n < kEvents; ++n) {
                fx.server->post(fx.cid, ::morph::wire::encode(::morph::wire::makeEvent("changes", detail::eventJson(n))));
            }
            REQUIRE(fx.waitFor([&] { return log.size() == kEvents; }));
            auto const got = log.snapshot();
            for (std::size_t n = 0; n < kEvents; ++n) {
                CHECK(got[n].payload.str == detail::eventJson(n));
            }
        });
    }
}

}  // namespace morph::testing::push
