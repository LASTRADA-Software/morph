// SPDX-License-Identifier: Apache-2.0
//
// Fixtures of the custom screens client's tests: a server without the model, a store that refuses every save, a
// client of a server's custom screens over a simulated connection, and helpers over the model's replies.

#pragma once
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <expected>
#include <glaze/glaze.hpp>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/owner_strand.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <morph/testing/step_executor.hpp>
#include <morph/ui/custom_screen_sync.hpp>
#include <morph/ui/custom_screens.hpp>
#include <morph/ui/interpreter.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "test_support.hpp"
#include "ui_custom_screens_support.hpp"

namespace custom_client {

/// A RemoteServer that never registered `morph.customScreens`.
struct BareServer {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    std::shared_ptr<uiservice::RuleAuthorizer> authorizer = std::make_shared<uiservice::RuleAuthorizer>();
    std::shared_ptr<morph::backend::RemoteServer> server =
        std::make_shared<morph::backend::RemoteServer>(pool, authorizer, dispatcher, registry);
};

/// A store that keeps what a memory store keeps but refuses every save, with a reason for the person.
struct RefusingStore final : morph::ui::ICustomScreenStore {
    explicit RefusingStore(morph::exec::IExecutor& base) : inner{base}, strand{base} {}
    morph::async::Completion<std::vector<morph::ui::CustomScreen>> list(std::string person, std::string app) override {
        return inner.list(std::move(person), std::move(app));
    }
    morph::async::Completion<std::expected<morph::ui::CustomScreen, morph::ui::CustomScreenRefusal>> get(
        morph::ui::CustomScreenKey key) override {
        return inner.get(std::move(key));
    }
    morph::async::Completion<std::expected<void, morph::ui::CustomScreenRefusal>> put(
        morph::ui::CustomScreenKey /*key*/, morph::ui::CustomScreen /*screen*/) override {
        auto [completion, promise] =
            morph::async::Completion<std::expected<void, morph::ui::CustomScreenRefusal>>::makeSettleable(&strand);
        promise.resolve(
            std::unexpected(morph::ui::CustomScreenRefusal{.reason = "over your limit of custom screens"}));
        return std::move(completion);
    }
    morph::async::Completion<std::expected<void, morph::ui::CustomScreenRefusal>> remove(
        morph::ui::CustomScreenKey key) override {
        return inner.remove(std::move(key));
    }
    morph::ui::MemoryCustomScreenStore inner;
    /// The serial owner the refusal is delivered on, declared last so it is destroyed first.
    morph::exec::OwnerStrand strand;
};

/// The id a `save` reply carries.
inline std::string savedIdOf(morph::wire::Envelope const& reply) {
    REQUIRE(reply.kind == "ok");
    morph::ui::CustomScreenSaved out;
    REQUIRE_FALSE(glz::read_json(out, reply.body));
    return out.id;
}

/// A `save` body: a screen of @p app based on @p basedOn, overwriting @p id when it is given.
inline std::string saveBody(std::string const& app, std::string const& title, std::string const& basedOn,
                            std::string const& document, std::string const& id = {}) {
    return R"({"app":)" + screens_test::quoted(app) +
           (id.empty() ? std::string{} : R"(,"id":)" + screens_test::quoted(id)) + R"(,"title":)" +
           screens_test::quoted(title) + R"(,"basedOn":)" + screens_test::quoted(basedOn) + R"(,"document":)" +
           screens_test::quoted(document) + "}";
}

/// One client of @p server, signed in as @p person, for @p app: a bridge over a simulated connection, the dispatcher
/// over it, and a sync. With @p recordWatches, the sync's watches are recorded and answered with nothing refused.
struct SyncClient {
    SyncClient(morph::backend::RemoteServer& server, std::string person, std::string app, bool recordWatches = false)
        : bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(server), owner},
          sync{dispatcher, std::move(app), [] { return uiservice::facts(); },
               recordWatches ? watcher() : morph::ui::Watcher{}} {
        morph::session::Context signedIn;
        signedIn.token = std::move(person);
        bridge.setDefaultSession(std::move(signedIn));
    }

    /// Runs the owner until @p done holds, for at most @p budget.
    template <typename Pred>
    bool pump(Pred done, std::chrono::milliseconds budget = morph::testing::kDefaultWaitBudget) {
        return morph::testing::waitUntil(
            [&] {
                owner.runAll();
                return done();
            },
            morph::testing::WaitBudget{budget});
    }

    /// Probes, and answers whether the server offers custom screens.
    bool probe() {
        std::optional<bool> offered;
        sync.probe([&offered](bool answer) { offered = answer; });
        REQUIRE(pump([&offered] { return offered.has_value(); }));
        return *offered;
    }

    morph::ui::Watcher watcher() {
        return [this](std::string const& kind, std::string const& body) {
            watches.push_back(kind + " " + body);
            auto [completion, promise] = morph::async::Completion<std::string>::makeSettleable(&owner);
            promise.resolve(std::string{R"({"refusedRecords":[],"refusedInstances":[]})"});
            return std::move(completion);
        };
    }

    std::vector<std::string> watches;
    morph::testing::StepExecutor owner;
    morph::bridge::Bridge bridge;
    morph::ui::BridgeDispatcher dispatcher{bridge, &owner};
    morph::ui::CustomScreenSync sync;
};

}  // namespace custom_client
