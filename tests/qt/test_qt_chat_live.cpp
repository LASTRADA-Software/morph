// SPDX-License-Identifier: Apache-2.0
//
// A chat against a real model, which costs money and needs a key, so it runs only when asked for by name:
//
//     MORPH_CHAT_LIVE_CONFIG=chat.json morph_qt_tests "[.live]"
//
// The file is a chat configuration (docs/spec/ui/chat.md). The check is on what was stored, since a model's words
// vary.

#include <QCoreApplication>
#include <QEventLoop>
#include <QObject>
#include <atomic>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/records.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/qt/qt_chat_provider.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/author.hpp>
#include <morph/ui/chat/config.hpp>
#include <morph/ui/chat/protocol.hpp>
#include <morph/ui/chat/service.hpp>
#include <morph/ui/service.hpp>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// NOLINTBEGIN(misc-use-internal-linkage): registered types must have external linkage.
namespace chat_live {

struct TestView {
    std::int64_t id = 0;
    std::optional<double> container;
    std::optional<double> before;
    std::optional<double> after;
};
struct ListTests {};
struct ListTestsResult {
    std::vector<TestView> tests;
};
struct AddTest {};
struct RecordValues {
    std::int64_t id = 0;
    std::optional<double> container;
    std::optional<double> before;
    std::optional<double> after;
};

struct Store {
    std::mutex mutex;
    std::vector<TestView> tests;
};
inline Store& store() {
    static Store shared;
    return shared;
}

struct IgnitionModel {
    ListTestsResult execute(ListTests const& /*action*/) const {
        std::scoped_lock const lock{store().mutex};
        return ListTestsResult{.tests = store().tests};
    }
    TestView execute(AddTest const& /*action*/) const {
        std::scoped_lock const lock{store().mutex};
        store().tests.push_back(TestView{.id = static_cast<std::int64_t>(store().tests.size()) + 1});
        return store().tests.back();
    }
    TestView execute(RecordValues const& action) const {
        std::scoped_lock const lock{store().mutex};
        for (TestView& test : store().tests) {
            if (test.id == action.id) {
                test.container = action.container ? action.container : test.container;
                test.before = action.before ? action.before : test.before;
                test.after = action.after ? action.after : test.after;
                return test;
            }
        }
        throw std::runtime_error{"no such test"};
    }
};

}  // namespace chat_live
// NOLINTEND(misc-use-internal-linkage)

BRIDGE_REGISTER_MODEL(chat_live::IgnitionModel, "Live_Ignition")
BRIDGE_REGISTER_ACTION(chat_live::IgnitionModel, chat_live::ListTests, "ListIgnitionTests")
BRIDGE_REGISTER_ACTION(chat_live::IgnitionModel, chat_live::AddTest, "AddIgnitionTest")
BRIDGE_REGISTER_ACTION(chat_live::IgnitionModel, chat_live::RecordValues, "RecordIgnitionValues")
BRIDGE_REGISTER_RECORD(chat_live::TestView, "Live_IgnitionTest")
BRIDGE_REGISTER_EFFECTS(chat_live::AddTest, morph::effects::changes<chat_live::TestView>)
BRIDGE_REGISTER_EFFECTS(chat_live::RecordValues, morph::effects::updates<chat_live::TestView>)

namespace {

using namespace std::chrono_literals;
namespace chat = morph::ui::chat;
namespace ui = morph::ui;
namespace wire = morph::wire;

constexpr std::string_view kScreen = R"({
  "vocab": "ui/1", "id": "live.ignition",
  "models": { "ignition": { "type": "Live_Ignition" } },
  "queries": { "tests": { "model": "ignition", "action": "ListIgnitionTests" } },
  "mutations": { "add": { "model": "ignition", "action": "AddIgnitionTest" },
                 "record": { "model": "ignition", "action": "RecordIgnitionValues" } },
  "view": { "kind": "text", "text": "Ignition tests" } })";

struct Authorizer final : morph::session::IAuthorizer {
    [[nodiscard]] bool authorize(morph::session::Context const& /*ctx*/, std::string_view /*modelType*/,
                                 std::string_view /*actionType*/) const override {
        return true;
    }
    [[nodiscard]] std::optional<std::string> authenticate(morph::session::Context const& ctx) const override {
        return ctx.token.empty() ? std::nullopt : std::optional{ctx.token};
    }
};

bool pumpUntil(std::function<bool()> const& done, std::chrono::milliseconds limit) {
    auto const deadline = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return true;
}

struct Rig {
    QObject owner;
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::ThreadPoolExecutor worker{2};
    std::shared_ptr<morph::backend::RemoteServer> server;
    std::uint64_t nextCall = 1;

    explicit Rig(chat::ChatConfig const& config) {
        ui::author::CatalogFacts facts;
        facts.action<chat_live::IgnitionModel, chat_live::ListTests>();
        facts.action<chat_live::IgnitionModel, chat_live::AddTest>();
        facts.action<chat_live::IgnitionModel, chat_live::RecordValues>();
        ui::UiRegistry registry{ui::ApplicationInfo{.id = "live", .version = "1"}, facts.catalog()};
        ui::CatalogEntry entry;
        entry.id = "live.ignition";
        REQUIRE(registry.addScreen(entry, std::string{kScreen}).has_value());
        auto built = registry.build();
        REQUIRE(built.has_value());
        auto provider = morph::qt::makeQtChatProvider(config.provider, &owner);
        INFO((provider ? std::string{} : provider.error()));
        REQUIRE(provider.has_value());
        chat::ChatOptions options;
        options.configure(config);
        options.provider = *provider;
        options.worker = &worker;
        options.bundle = *built;
        morph::backend::ServerConfig serverConfig;
        serverConfig.services.push_back(std::make_shared<ui::UiService>(*built));
        serverConfig.services.push_back(std::make_shared<chat::ChatService>(std::move(options)));
        server = std::make_shared<morph::backend::RemoteServer>(pool, std::make_shared<Authorizer>(),
                                                                std::move(serverConfig));
    }

    wire::Envelope call(wire::Envelope env, morph::backend::ConnectionId cid) {
        env.callId = nextCall++;
        env.session.token = "tech";
        env.session.principal = "tech";
        auto replied = std::make_shared<std::optional<std::string>>();
        auto mutex = std::make_shared<std::mutex>();
        server->handle(
            wire::encode(env),
            [replied, mutex](std::string const& text) {
                std::scoped_lock const lock{*mutex};
                *replied = text;
            },
            cid);
        REQUIRE(pumpUntil(
            [&] {
                std::scoped_lock const lock{*mutex};
                return replied->has_value();
            },
            10s));
        return wire::decode(**replied);
    }

    chat::TurnState say(std::string text, std::uint64_t instance, morph::backend::ConnectionId cid) {
        chat::TurnRequest request;
        request.screenId = "live.ignition";
        request.models.push_back({.path = "/models/ignition",
                                  .alias = "ignition",
                                  .modelType = "Live_Ignition",
                                  .instanceId = instance,
                                  .primary = ""});
        request.text = std::move(text);
        wire::Envelope turn;
        turn.kind = std::string{chat::kTurnKind};
        turn.body = chat::writeMessage(request);
        auto const accepted = call(std::move(turn), cid);
        REQUIRE(accepted.kind == "ok");
        auto const turnId = chat::readTurnAccepted(accepted.body)->turnId;
        chat::TurnState state;
        REQUIRE(pumpUntil(
            [&] {
                wire::Envelope read;
                read.kind = std::string{chat::kStateKind};
                read.body = chat::writeMessage(chat::StateRequest{.turnId = turnId});
                state = *chat::readTurnState(call(std::move(read), cid).body);
                return state.status != chat::TurnStatus::Running;
            },
            180s));
        return state;
    }
};

}  // namespace

TEST_CASE("a real model turns the two phrases into stored values", "[.live][qt][chat]") {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts.
    char const* const path = std::getenv("MORPH_CHAT_LIVE_CONFIG");
    if (path == nullptr) {
        SKIP("MORPH_CHAT_LIVE_CONFIG is not set");
    }
    auto const config = chat::loadChatConfig(path);
    INFO((config ? std::string{} : config.error()));
    REQUIRE(config.has_value());
    {
        std::scoped_lock const lock{chat_live::store().mutex};
        chat_live::store().tests.clear();
    }
    Rig rig{*config};
    auto const cid = rig.server->openConnection();
    auto const registered = rig.call(wire::makeRegister("Live_Ignition"), cid);
    REQUIRE(registered.kind == "ok");

    auto const first = rig.say("add ignition test", registered.modelId, cid);
    INFO("first: " << first.error << " | " << first.text);
    REQUIRE(first.status == chat::TurnStatus::Done);
    {
        std::scoped_lock const lock{chat_live::store().mutex};
        REQUIRE(chat_live::store().tests.size() == 1);
    }

    auto const second = rig.say("container 100, before 299, after 223", registered.modelId, cid);
    INFO("second: " << second.error << " | " << second.text);
    REQUIRE(second.status == chat::TurnStatus::Done);
    std::scoped_lock const lock{chat_live::store().mutex};
    REQUIRE(chat_live::store().tests.size() == 1);
    CHECK(chat_live::store().tests.front().container == 100.0);
    CHECK(chat_live::store().tests.front().before == 299.0);
    CHECK(chat_live::store().tests.front().after == 223.0);
}
