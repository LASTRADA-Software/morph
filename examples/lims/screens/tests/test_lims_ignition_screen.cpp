// SPDX-License-Identifier: Apache-2.0
//
// The whole chain, through the real shell: what a person types in the chat panel runs on the server, reaches the
// journal with a mark, and appears on the open screen by push.

#include <atomic>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <morph/client/shell.hpp>
#include <morph/client/source.hpp>
#include <morph/core/backend.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/chat/protocol.hpp>
#include <morph/ui/chat/service.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/testing/fake_chat_provider.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <testkit/db_fixture.hpp>
#include <testkit/fake_app_context.hpp>
#include <testkit/wait.hpp>
#include <utility>
#include <vector>

#include "lims/models/ignition_model.hpp"
#include "lims_screens.hpp"

namespace {

namespace chat = morph::ui::chat;
namespace ui = morph::ui;
using morph::ladder::testkit::DbFixture;

// Everyone is the one technician the session says, and every action is allowed: this is about the chain and not its
// checks, which the chat service's own tests cover.
struct Authorizer final : morph::session::IAuthorizer {
    [[nodiscard]] bool authorize(morph::session::Context const& /*ctx*/, std::string_view /*modelType*/,
                                 std::string_view /*actionType*/) const override {
        return true;
    }
    [[nodiscard]] std::optional<std::string> authenticate(morph::session::Context const& /*ctx*/) const override {
        return std::string{"tech"};
    }
};

// LIMS behind a server on one connection, as a client over a socket would have it, with a chat on a scripted provider.
class ChatSource final : public morph::client::Source {
public:
    explicit ChatSource(std::shared_ptr<chat::IProvider> provider) : _provider{std::move(provider)} {}
    ~ChatSource() override { close(); }
    ChatSource(ChatSource const&) = delete;
    ChatSource& operator=(ChatSource const&) = delete;
    ChatSource(ChatSource&&) = delete;
    ChatSource& operator=(ChatSource&&) = delete;

    // The last chat turn the client sent, as it left the client.
    // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes): the test reads it.
    std::optional<chat::TurnRequest> lastTurn;

protected:
    [[nodiscard]] std::expected<Transport, ui::ConnectError> connect(ui::AppContext& /*context*/) override {
        auto bundle = lims::screens::bundle();
        REQUIRE(bundle.has_value());
        _workers = std::make_unique<morph::exec::ThreadPoolExecutor>(2);
        _chatWorker = std::make_unique<morph::exec::ThreadPoolExecutor>(2);
        morph::backend::ServerConfig config;
        config.services.push_back(std::make_shared<ui::UiService>(*bundle));
        chat::ChatOptions options;
        options.provider = _provider;
        options.worker = _chatWorker.get();
        options.bundle = *bundle;
        config.services.push_back(std::make_shared<chat::ChatService>(std::move(options)));
        _server = std::make_shared<morph::backend::RemoteServer>(*_workers, std::make_shared<Authorizer>(),
                                                                 std::move(config));
        auto const cid = _server->openConnection();
        ui::Exchange exchange = [this, server = _server, cid](morph::wire::Envelope const& request,
                                                              std::function<void(ui::ExchangeReply)> const& done) {
            if (request.kind == chat::kTurnKind) {
                if (auto turn = chat::readTurnRequest(request.body)) {
                    lastTurn = std::move(*turn);
                }
            }
            done(morph::wire::decode(server->handleInline(morph::wire::encode(request), cid)));
        };
        return Transport{.backend = std::make_unique<morph::backend::SimulatedRemoteBackend>(*_server, cid),
                         .exchange = std::move(exchange),
                         .serverEvents = true};
    }

    void disconnect() override {
        _server.reset();
        _chatWorker.reset();
        _workers.reset();
    }

private:
    std::shared_ptr<chat::IProvider> _provider;
    std::unique_ptr<morph::exec::ThreadPoolExecutor> _workers;
    std::unique_ptr<morph::exec::ThreadPoolExecutor> _chatWorker;
    std::shared_ptr<morph::backend::RemoteServer> _server;
};

// The log every ignition instance in this binary writes to. It belongs to whichever rig is pumping.
struct SharedLog {
    morph::exec::MainThreadExecutor owner;
    std::shared_ptr<morph::journal::InMemoryActionLog> log =
        std::make_shared<morph::journal::InMemoryActionLog>(owner);
    SharedLog() { lims::attachIgnitionLog(log); }
};

SharedLog& sharedLog() {
    static SharedLog shared;
    return shared;
}

struct Rig {
    DbFixture db;
    SharedLog& shared = sharedLog();
    std::size_t logBefore = shared.log->entries().size();
    std::shared_ptr<chat::testing::FakeChatProvider> fake = std::make_shared<chat::testing::FakeChatProvider>();
    morph::examples::testing::FakeAppContext context;
    ui::testing::RecordingBackend backend;
    ChatSource source{fake};
    std::unique_ptr<ui::MountedShell> shell;
    std::vector<std::string> problems;

    Rig() {
        backend.setWidthClass("expanded");
        std::optional<std::expected<ui::Bundle, ui::ConnectError>> opened;
        source.open(context,
                    [&opened](std::expected<ui::Bundle, ui::ConnectError> result) { opened = std::move(result); });
        REQUIRE(pump([&opened] { return opened.has_value(); }));
        REQUIRE(opened->has_value());
        shell = morph::client::mountShell(
            context, backend, std::move(*opened), source,
            morph::client::ShellOptions{.startScreen = std::string{lims::screens::kIgnition},
                                        .onProblem = [this](std::string const& text) { problems.push_back(text); }});
        REQUIRE(pump([this] { return backend.find("Button", "testId", "ignition_add").has_value(); }));
        REQUIRE(pump([this] { return backend.find("TextInput", "testId", "chat_input").has_value(); }));
    }

    ~Rig() {
        shell.reset();
        source.close();
    }
    Rig(Rig const&) = delete;
    Rig& operator=(Rig const&) = delete;
    Rig(Rig&&) = delete;
    Rig& operator=(Rig&&) = delete;

    template <typename Pred>
    [[nodiscard]] bool pump(Pred done, std::chrono::milliseconds budget = std::chrono::seconds{10}) {
        return morph::examples::testing::pumpUntil(
            context.owner(),
            [&] {
                shared.owner.runFor(std::chrono::milliseconds{1});
                return done();
            },
            budget);
    }

    void say(std::string text) { backend.submit(*backend.find("TextInput", "testId", "chat_input"), std::move(text)); }

    [[nodiscard]] bool showsText(std::string_view text) const { return backend.dump().contains(text); }

    [[nodiscard]] std::vector<morph::journal::LogEntry> journal(std::size_t expected) {
        static_cast<void>(pump([&] { return shared.log->entries().size() >= logBefore + expected; }));
        auto const all = shared.log->entries();
        return {all.begin() + static_cast<std::ptrdiff_t>(logBefore), all.end()};
    }

    void scriptCall(std::string name, std::string arguments, std::string reply) const {
        fake->script(chat::Response{
            .text = "",
            .calls = {chat::ToolCall{.id = "1", .name = std::move(name), .argumentsJson = std::move(arguments)}},
            .opaque = {}});
        fake->script(chat::Response{.text = std::move(reply), .calls = {}, .opaque = {}});
    }
};

}  // namespace

// Checked by mutation: with RecordIgnitionValues declaring no effect, the screen keeps showing no values and this fails.
TEST_CASE("the_chat_utterances_appear_on_the_mounted_screen", "[lims][ignition][chat]") {
    Rig rig;
    CHECK_FALSE(rig.showsText("ignition_test_row"));

    rig.scriptCall("ignition__AddIgnitionTest", "{}", "Added.");
    rig.say("add ignition test");
    REQUIRE(rig.pump([&] { return rig.backend.find("Column", "testId", "ignition_test_row").has_value(); }));

    rig.scriptCall("ignition__RecordIgnitionValues", R"({"id":1,"container":100,"before":299,"after":223})",
                   "Recorded.");
    REQUIRE(rig.pump([&] { return rig.showsText("Added."); }));
    rig.say("container 100, before 299, after 223");
    // The screen's own lines: the numbers alone would also match what was typed in the chat.
    REQUIRE(rig.pump([&] {
        return rig.showsText("Container: 100") && rig.showsText("Before: 299") && rig.showsText("After: 223");
    }));
    CHECK(rig.pump([&] { return rig.showsText("Recorded."); }));
    CHECK(rig.problems.empty());

    auto const entries = rig.journal(2);
    REQUIRE(entries.size() == 2);
    CHECK(entries.at(0).actionType == "AddIgnitionTest");
    CHECK(entries.at(0).principal == "tech");
    CHECK(entries.at(0).originKind == "assistant");
    CHECK(entries.at(1).actionType == "RecordIgnitionValues");
    CHECK(entries.at(1).originText == "container 100, before 299, after 223");

    // The second turn saw the first: the same conversation, so "this test" has something to refer to.
    auto const asked = rig.fake->requests();
    REQUIRE(asked.size() == 4);
    CHECK(asked.at(2).messages.front().text == "add ignition test");
    CHECK(asked.at(0).system.contains("lims.ignition"));
}

// Mutation: mark every entry as the assistant's.
TEST_CASE("the_same_action_from_the_button_is_journaled_once_with_no_mark", "[lims][ignition][chat]") {
    Rig rig;
    rig.backend.click(*rig.backend.find("Button", "testId", "ignition_add"));
    REQUIRE(rig.pump([&] { return rig.backend.find("Column", "testId", "ignition_test_row").has_value(); }));
    auto const entries = rig.journal(1);
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().actionType == "AddIgnitionTest");
    CHECK(entries.front().originKind.empty());
}

// Mutation: leave the screen's instances out of the turn (the assistant's calls have nothing to run on).
TEST_CASE("a_turn_names_the_screens_own_instance", "[lims][ignition][chat]") {
    Rig rig;
    rig.scriptCall("ignition__AddIgnitionTest", "{}", "Added.");
    rig.say("add ignition test");
    REQUIRE(rig.pump([&] { return rig.showsText("Added."); }));
    REQUIRE(rig.source.lastTurn.has_value());
    REQUIRE(rig.source.lastTurn->models.size() == 1);
    CHECK(rig.source.lastTurn->models.front().alias == "ignition");
    CHECK(rig.source.lastTurn->models.front().instanceId != 0);
    CHECK(rig.source.lastTurn->screenId == lims::screens::kIgnition);
    bool offered = false;
    auto const asked = rig.fake->requests();
    for (chat::ToolSpec const& tool : asked.at(0).tools) {
        offered = offered || tool.name == "ignition__RecordIgnitionValues";
    }
    CHECK(offered);
}
