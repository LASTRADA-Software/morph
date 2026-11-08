// SPDX-License-Identifier: Apache-2.0
//
// A chat turn on a real server: the screen's tools, called through the same checks as the GUI's own calls, reaching
// the GUI by push and the journal with a mark.

// clang-format off
#include <morph/ui/value.hpp>
// clang-format on

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/logger.hpp>
#include <morph/core/model.hpp>
#include <morph/core/records.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/author.hpp>
#include <morph/ui/chat/protocol.hpp>
#include <morph/ui/chat/service.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/testing/fake_chat_provider.hpp>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "test_support.hpp"

// NOLINTBEGIN(misc-use-internal-linkage): registered types must have external linkage.
namespace chat_service_test {

struct Note {
    std::int64_t id = 0;
    std::string text;
};
struct ListNotes {};
struct AddNote {
    std::string text;
};
struct WipeNotes {};
struct QuietNote {
    std::string text;
};
struct SilentNote {
    std::string text;
};
struct SecretRead {};

struct Store {
    std::mutex mutex;
    std::vector<Note> notes;
};
inline Store& store() {
    static Store shared;
    return shared;
}

struct NotesModel {
    [[nodiscard]] std::vector<Note> execute(ListNotes const& /*action*/) const {
        std::scoped_lock const lock{store().mutex};
        return store().notes;
    }
    [[nodiscard]] Note execute(AddNote const& action) const {
        std::scoped_lock const lock{store().mutex};
        store().notes.push_back(Note{.id = static_cast<std::int64_t>(store().notes.size()) + 1, .text = action.text});
        return store().notes.back();
    }
    [[nodiscard]] bool execute(WipeNotes const& /*action*/) const {
        std::scoped_lock const lock{store().mutex};
        store().notes.clear();
        return true;
    }
    [[nodiscard]] Note execute(QuietNote const& action) const { return Note{.id = 1, .text = action.text}; }
    [[nodiscard]] Note execute(SilentNote const& action) const { return Note{.id = 1, .text = action.text}; }
    [[nodiscard]] bool execute(SecretRead const& /*action*/) const { return true; }
};

}  // namespace chat_service_test
// NOLINTEND(misc-use-internal-linkage)

using chat_service_test::AddNote;
using chat_service_test::ListNotes;
using chat_service_test::NotesModel;
using chat_service_test::QuietNote;
using chat_service_test::SecretRead;
using chat_service_test::SilentNote;
using chat_service_test::WipeNotes;

BRIDGE_REGISTER_MODEL(NotesModel, "Chat_Notes")
BRIDGE_REGISTER_ACTION(NotesModel, ListNotes, "Chat_List")
BRIDGE_REGISTER_ACTION(NotesModel, AddNote, "Chat_Add")
BRIDGE_REGISTER_ACTION(NotesModel, WipeNotes, "Chat_Wipe")
BRIDGE_REGISTER_ACTION(NotesModel, QuietNote, "Chat_Quiet", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(NotesModel, SilentNote, "Chat_Silent")
BRIDGE_REGISTER_ACTION(NotesModel, SecretRead, "Chat_Secret")
BRIDGE_REGISTER_RECORD(chat_service_test::Note, "Chat_Note")
BRIDGE_REGISTER_EFFECTS(AddNote, morph::effects::changes<chat_service_test::Note>)
BRIDGE_REGISTER_EFFECTS(WipeNotes, morph::effects::changes<chat_service_test::Note>)
BRIDGE_REGISTER_EFFECTS(QuietNote, morph::effects::changes<chat_service_test::Note>)
BRIDGE_REGISTER_EFFECTS(SilentNote, morph::effects::none)

namespace {

using namespace std::chrono_literals;
namespace ui = morph::ui;
namespace chat = morph::ui::chat;
namespace wire = morph::wire;
using morph::backend::ConnectionId;

constexpr std::string_view kNotesScreen = R"({
  "vocab": "ui/1", "id": "notes",
  "models": { "notes": { "type": "Chat_Notes" } },
  "queries": { "list": { "model": "notes", "action": "Chat_List" } },
  "mutations": { "add": { "model": "notes", "action": "Chat_Add" }, "wipe": { "model": "notes", "action": "Chat_Wipe" },
                 "quiet": { "model": "notes", "action": "Chat_Quiet" },
                 "silent": { "model": "notes", "action": "Chat_Silent" } },
  "view": { "kind": "text", "text": "Notes" } })";

constexpr std::string_view kSecretScreen = R"({
  "vocab": "ui/1", "id": "secret",
  "models": { "notes": { "type": "Chat_Notes" } },
  "queries": { "read": { "model": "notes", "action": "Chat_Secret" } },
  "mutations": { "add": { "model": "notes", "action": "Chat_Add" } },
  "view": { "kind": "text", "text": "Secret" } })";

// The token is the principal. `muted` may not chat, `nosecret` may not read the secret screen's query, `visitor` may
// not wipe, and every instance belongs to whoever registered it.
struct Authorizer final : morph::session::IAuthorizer {
    [[nodiscard]] bool authorize(morph::session::Context const& ctx, std::string_view modelType,
                                 std::string_view actionType) const override {
        if (ctx.principal == "muted" && modelType == "ui" && actionType == "chat") {
            return false;
        }
        if (ctx.principal == "nosecret" && actionType == "Chat_Secret") {
            return false;
        }
        return !(ctx.principal == "visitor" && actionType == "Chat_Wipe");
    }
    [[nodiscard]] std::optional<std::string> authenticate(morph::session::Context const& ctx) const override {
        return ctx.token.empty() ? std::nullopt : std::optional{ctx.token};
    }
    [[nodiscard]] bool authorizeInstance(morph::session::Context const& ctx, std::string_view /*modelType*/,
                                         std::string_view /*actionType*/, std::uint64_t /*modelId*/,
                                         std::string_view owner) const override {
        return owner.empty() || owner == ctx.principal;
    }
};

struct EventWire {
    std::mutex mutex;
    std::vector<std::string> sent;
    std::atomic<std::size_t> backlog{0};

    morph::backend::ConnectionWriter writer() {
        return {.write =
                    [this](std::string frame) {
                        std::scoped_lock const lock{mutex};
                        sent.push_back(std::move(frame));
                    },
                .backlog = [this] { return backlog.load(); }};
    }

    [[nodiscard]] std::vector<wire::EventBody> events() {
        std::scoped_lock const lock{mutex};
        std::vector<wire::EventBody> out;
        for (std::string const& frame : sent) {
            auto const env = wire::decode(frame);
            wire::EventBody body;
            REQUIRE_FALSE(glz::read<glz::opts{.error_on_unknown_keys = false}>(body, env.body));
            out.push_back(std::move(body));
        }
        return out;
    }

    [[nodiscard]] bool changed(std::string_view record) {
        return std::ranges::any_of(events(), [&](wire::EventBody const& event) {
            return event.topic == wire::kChangesTopic && event.payload.str.contains(record);
        });
    }
};

bool waitFor(auto done, std::chrono::milliseconds limit = 5s) {
    auto const deadline = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        morph::testing::storageOwner().runFor(2ms);
    }
    return true;
}

std::shared_ptr<morph::journal::InMemoryActionLog> sharedLog() {
    static auto const shared = [] {
        auto log = std::make_shared<morph::journal::InMemoryActionLog>(morph::testing::storageOwner());
        morph::model::detail::defaultRegistry().registerModel<NotesModel>("Chat_Notes", [log] {
            auto holder = std::make_unique<morph::model::detail::ModelHolder<NotesModel>>();
            holder->attachActionLog(log, {});
            return holder;
        });
        return log;
    }();
    return shared;
}

struct Rig {
    std::shared_ptr<morph::journal::InMemoryActionLog> log = sharedLog();
    std::size_t logBefore = log->entries().size();
    // Declared ahead of the pools, so it outlives the threads that write frames into it.
    EventWire eventWire;
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::ThreadPoolExecutor worker{2};
    std::shared_ptr<chat::testing::FakeChatProvider> fake = std::make_shared<chat::testing::FakeChatProvider>();
    chat::ChatLimits limits;
    std::shared_ptr<morph::backend::RemoteServer> server;
    std::shared_ptr<ui::ServedBundle const> bundle;
    std::atomic<std::uint64_t> nextCall{1};

    explicit Rig(chat::ChatLimits given = {}, bool withProvider = true,
                 std::shared_ptr<chat::IProvider> const& other = nullptr)
        : limits{given} {
        {
            std::scoped_lock const lock{chat_service_test::store().mutex};
            chat_service_test::store().notes.clear();
        }
        ui::author::CatalogFacts facts;
        facts.action<NotesModel, ListNotes>();
        facts.action<NotesModel, AddNote>();
        facts.action<NotesModel, WipeNotes>();
        facts.action<NotesModel, QuietNote>();
        facts.action<NotesModel, SilentNote>();
        facts.action<NotesModel, SecretRead>();
        ui::UiRegistry registry{ui::ApplicationInfo{.id = "notes", .version = "1"}, facts.catalog()};
        for (auto [id, text] : {std::pair{"notes", kNotesScreen}, std::pair{"secret", kSecretScreen}}) {
            ui::CatalogEntry entry;
            entry.id = id;
            REQUIRE(registry.addScreen(entry, std::string{text}).has_value());
        }
        auto built = registry.build();
        REQUIRE(built.has_value());
        bundle = *built;
        morph::backend::ServerConfig config;
        config.services.push_back(std::make_shared<ui::UiService>(bundle));
        chat::ChatOptions options;
        if (other != nullptr) {
            options.provider = other;
        } else if (withProvider) {
            options.provider = fake;
        }
        options.worker = &worker;
        options.bundle = bundle;
        options.limits = limits;
        chatService = std::make_shared<chat::ChatService>(std::move(options));
        config.services.push_back(chatService);
        server =
            std::make_shared<morph::backend::RemoteServer>(pool, std::make_shared<Authorizer>(), std::move(config));
    }

    std::shared_ptr<chat::ChatService> chatService;

    wire::Envelope call(wire::Envelope env, ConnectionId cid, std::string const& token = "alice") {
        env.callId = nextCall.fetch_add(1);
        env.session.token = token;
        env.session.principal = token;
        auto replied = std::make_shared<std::promise<std::string>>();
        auto answer = replied->get_future();
        server->handle(wire::encode(env), [replied](std::string const& text) { replied->set_value(text); }, cid);
        REQUIRE(waitFor([&] { return answer.wait_for(0s) == std::future_status::ready; }));
        return wire::decode(answer.get());
    }

    ConnectionId gui(std::string const& token = "alice") {
        auto const cid = server->openConnection();
        server->setConnectionWriter(cid, eventWire.writer());
        wire::Envelope hello;
        hello.kind = std::string{ui::kHelloKind};
        hello.body = R"({"vocabularies":["ui/1","expr/1"],"events":true})";
        auto const helloReply = call(std::move(hello), cid, token);
        REQUIRE(helloReply.kind == "ok");
        wire::Envelope watch;
        watch.kind = std::string{wire::kWatchKind};
        watch.body = R"({"records":["Chat_Note"],"instances":[]})";
        auto const watchReply = call(std::move(watch), cid, token);
        REQUIRE(watchReply.kind == "ok");
        return cid;
    }

    std::uint64_t instance(ConnectionId cid, std::string const& token = "alice") {
        auto const reply = call(wire::makeRegister("Chat_Notes"), cid, token);
        REQUIRE(reply.kind == "ok");
        return reply.modelId;
    }

    [[nodiscard]] static chat::TurnRequest turnOn(std::uint64_t modelId, std::string text,
                                                  std::string screen = "notes") {
        chat::TurnRequest request;
        request.screenId = std::move(screen);
        request.models.push_back({.path = "/models/notes",
                                  .alias = "notes",
                                  .modelType = "Chat_Notes",
                                  .instanceId = modelId,
                                  .primary = ""});
        request.text = std::move(text);
        return request;
    }

    wire::Envelope startRaw(chat::TurnRequest const& request, ConnectionId cid, std::string const& token = "alice") {
        wire::Envelope env;
        env.kind = std::string{chat::kTurnKind};
        env.body = chat::writeMessage(request);
        return call(std::move(env), cid, token);
    }

    std::uint64_t start(chat::TurnRequest const& request, ConnectionId cid, std::string const& token = "alice") {
        auto const reply = startRaw(request, cid, token);
        REQUIRE(reply.kind == "ok");
        auto const accepted = chat::readTurnAccepted(reply.body);
        REQUIRE(accepted.has_value());
        return accepted->turnId;
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a test helper; the token is almost always left out.
    chat::TurnState state(std::uint64_t turnId, ConnectionId cid, std::string const& token = "alice") {
        wire::Envelope env;
        env.kind = std::string{chat::kStateKind};
        env.body = chat::writeMessage(chat::StateRequest{.turnId = turnId});
        auto const reply = call(std::move(env), cid, token);
        REQUIRE(reply.kind == "ok");
        auto const read = chat::readTurnState(reply.body);
        REQUIRE(read.has_value());
        return *read;
    }

    chat::TurnState finished(std::uint64_t turnId, ConnectionId cid, std::string const& token = "alice") {
        chat::TurnState last;
        REQUIRE(waitFor([&] {
            last = state(turnId, cid, token);
            return last.status != chat::TurnStatus::Running;
        }));
        return last;
    }

    std::vector<morph::journal::LogEntry> journal(std::size_t expected) {
        waitFor([&] { return log->entries().size() >= logBefore + expected; });
        morph::testing::storageOwner().runFor(20ms);
        auto const all = log->entries();
        return {all.begin() + static_cast<std::ptrdiff_t>(logBefore), all.end()};
    }

    void scriptAdd(std::string const& text) {
        fake->script(chat::Response{
            .text = "",
            .calls = {chat::ToolCall{
                .id = "1", .name = "notes__Chat_Add", .argumentsJson = R"({"text":")" + text + R"("})"}}});
        fake->script(chat::Response{.text = "done", .calls = {}, .opaque = {}});
    }
};

// A provider the test answers by hand, whenever and however a real one might. `called` hands the sink over rather
// than keeping it: the sink holds the service, which holds this provider, so a kept one would never be freed.
struct ManualProvider final : chat::IProvider {
    struct Call final : chat::IProviderCall {
        void cancel() override {}
    };

    [[nodiscard]] std::shared_ptr<chat::IProviderCall> stream(chat::Request /*request*/,
                                                              chat::ProviderSink given) override {
        std::scoped_lock const lock{mutex};
        sink = std::move(given);
        return std::make_shared<Call>();
    }

    [[nodiscard]] chat::ProviderSink called() {
        REQUIRE(waitFor([&] {
            std::scoped_lock const lock{mutex};
            return sink.has_value();
        }));
        std::scoped_lock const lock{mutex};
        return *std::exchange(sink, std::nullopt);
    }

    std::mutex mutex;
    std::optional<chat::ProviderSink> sink;
};

std::size_t noteCount() {
    std::scoped_lock const lock{chat_service_test::store().mutex};
    return chat_service_test::store().notes.size();
}

}  // namespace

TEST_CASE("a_turn_runs_the_tool_and_finishes", "[chat][service]") {
    Rig rig;
    auto const cid = rig.gui();
    auto const id = rig.instance(cid);
    rig.scriptAdd("hi");
    auto const state = rig.finished(rig.start(Rig::turnOn(id, "add hi"), cid), cid);
    CHECK(state.status == chat::TurnStatus::Done);
    CHECK(state.text == "done");
    REQUIRE(state.steps.size() == 1);
    CHECK(state.steps.front().name == "notes__Chat_Add");
    CHECK(state.steps.front().ok);
    REQUIRE(noteCount() == 1);
    CHECK(chat_service_test::store().notes.at(0).text == "hi");
}

TEST_CASE("the_provider_is_given_the_screens_tools_and_context", "[chat][service]") {
    Rig rig;
    auto const cid = rig.gui();
    auto const id = rig.instance(cid);
    rig.fake->script(chat::Response{.text = "hello", .calls = {}, .opaque = {}});
    auto request = Rig::turnOn(id, "hi there");
    request.paramsJson = R"({"sampleId":42})";
    request.models.front().primary = "the-primary";
    static_cast<void>(rig.finished(rig.start(request, cid), cid));
    auto const asked = rig.fake->requests().at(0);
    CHECK(asked.system.contains("notes"));
    CHECK(asked.system.contains("sampleId"));
    CHECK(asked.system.contains("the-primary"));
    CHECK(asked.tools.size() == 5);
    REQUIRE(asked.messages.size() == 1);
    CHECK(asked.messages.front().role == "user");
    CHECK(asked.messages.front().text == "hi there");
}

TEST_CASE("a_turns_change_reaches_the_watching_gui", "[chat][service]") {
    Rig rig;
    auto const cid = rig.gui();
    auto const id = rig.instance(cid);
    rig.scriptAdd("hi");
    static_cast<void>(rig.finished(rig.start(Rig::turnOn(id, "add hi"), cid), cid));
    CHECK(waitFor([&] { return rig.eventWire.changed("Chat_Note"); }));
}

TEST_CASE("removing_the_effect_stops_the_gui_event", "[chat][service]") {
    Rig rig;
    auto const cid = rig.gui();
    auto const id = rig.instance(cid);
    rig.fake->script(chat::Response{
        .text = "",
        .calls = {chat::ToolCall{.id = "1", .name = "notes__Chat_Silent", .argumentsJson = R"({"text":"x"})"}}});
    rig.fake->script(chat::Response{.text = "done", .calls = {}, .opaque = {}});
    auto const state = rig.finished(rig.start(Rig::turnOn(id, "note it"), cid), cid);
    REQUIRE(state.status == chat::TurnStatus::Done);
    REQUIRE(state.steps.at(0).ok);
    morph::testing::storageOwner().runFor(200ms);
    CHECK_FALSE(rig.eventWire.changed("Chat_Note"));
}

TEST_CASE("a_turn_writes_one_marked_journal_entry", "[chat][service]") {
    Rig rig;
    auto const cid = rig.gui();
    auto const id = rig.instance(cid);
    rig.scriptAdd("hi");
    static_cast<void>(rig.finished(rig.start(Rig::turnOn(id, "add hi"), cid), cid));
    auto const entries = rig.journal(1);
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().actionType == "Chat_Add");
    CHECK(entries.front().principal == "alice");
    CHECK(entries.front().originKind == "assistant");
    CHECK(entries.front().originText == "add hi");
}

TEST_CASE("a_destructive_action_takes_effect", "[chat][service]") {
    Rig rig;
    auto const cid = rig.gui();
    auto const id = rig.instance(cid);
    {
        std::scoped_lock const lock{chat_service_test::store().mutex};
        chat_service_test::store().notes.push_back({.id = 1, .text = "old"});
    }
    rig.fake->script(chat::Response{
        .text = "", .calls = {chat::ToolCall{.id = "1", .name = "notes__Chat_Wipe", .argumentsJson = "{}"}}});
    rig.fake->script(chat::Response{.text = "wiped", .calls = {}, .opaque = {}});
    auto const state = rig.finished(rig.start(Rig::turnOn(id, "delete everything"), cid), cid);
    REQUIRE(state.status == chat::TurnStatus::Done);
    CHECK(noteCount() == 0);
    auto const entries = rig.journal(1);
    REQUIRE(entries.size() == 1);
    CHECK(entries.back().actionType == "Chat_Wipe");
    CHECK(entries.back().originKind == "assistant");
    CHECK(waitFor([&] { return rig.eventWire.changed("Chat_Note"); }));
}

TEST_CASE("a_refused_action_is_reported_and_changes_nothing", "[chat][service]") {
    Rig rig;
    auto const cid = rig.gui("visitor");
    auto const id = rig.instance(cid, "visitor");
    // `visitor` may not wipe, so the screen offers no such tool: a model that calls it anyway gets a tool error.
    rig.fake->script(chat::Response{
        .text = "", .calls = {chat::ToolCall{.id = "1", .name = "notes__Chat_Wipe", .argumentsJson = "{}"}}});
    rig.fake->script(chat::Response{.text = "could not", .calls = {}, .opaque = {}});
    {
        std::scoped_lock const lock{chat_service_test::store().mutex};
        chat_service_test::store().notes.push_back({.id = 1, .text = "old"});
    }
    auto const state = rig.finished(rig.start(Rig::turnOn(id, "delete"), cid, "visitor"), cid, "visitor");
    REQUIRE(state.steps.size() == 1);
    CHECK_FALSE(state.steps.at(0).ok);
    CHECK(noteCount() == 1);
    CHECK(rig.journal(0).empty());
}

TEST_CASE("the_chat_gate_refuses_an_unauthorised_principal", "[chat][service]") {
    Rig rig;
    auto const cid = rig.gui("muted");
    auto const id = rig.instance(cid, "muted");
    auto const reply = rig.startRaw(Rig::turnOn(id, "hi"), cid, "muted");
    CHECK(reply.kind == "err");
    CHECK(rig.fake->calls() == 0);
    wire::Envelope probe;
    probe.kind = std::string{chat::kStateKind};
    auto const probeReply = rig.call(std::move(probe), cid, "muted");
    CHECK(probeReply.kind == "err");
}

TEST_CASE("a_screen_the_principal_may_not_fetch_gets_no_tools", "[chat][service]") {
    Rig rig;
    auto const cid = rig.gui("nosecret");
    auto const id = rig.instance(cid, "nosecret");
    auto const state = rig.finished(rig.start(Rig::turnOn(id, "hi", "secret"), cid, "nosecret"), cid, "nosecret");
    CHECK(state.status == chat::TurnStatus::Failed);
    CHECK(rig.fake->calls() == 0);
}

TEST_CASE("an_unknown_screen_fails_the_turn", "[chat][service]") {
    Rig rig;
    auto const cid = rig.gui();
    auto const id = rig.instance(cid);
    auto const state = rig.finished(rig.start(Rig::turnOn(id, "hi", "nosuch"), cid), cid);
    CHECK(state.status == chat::TurnStatus::Failed);
    CHECK(rig.fake->calls() == 0);
}

TEST_CASE("a_context_naming_anothers_instance_is_refused_by_an_ownership_authorizer", "[chat][service]") {
    Rig rig;
    auto const bobsConnection = rig.gui("bob");
    auto const bobsInstance = rig.instance(bobsConnection, "bob");
    auto const cid = rig.gui();
    rig.scriptAdd("stolen");
    auto const state = rig.finished(rig.start(Rig::turnOn(bobsInstance, "add"), cid), cid);
    REQUIRE(state.steps.size() == 1);
    CHECK_FALSE(state.steps.at(0).ok);
    CHECK(noteCount() == 0);
}

TEST_CASE("another_principal_cannot_read_a_turn", "[chat][service]") {
    Rig rig;
    auto const cid = rig.gui();
    auto const id = rig.instance(cid);
    rig.fake->script(chat::Response{.text = "private reply", .calls = {}, .opaque = {}});
    auto const turnId = rig.start(Rig::turnOn(id, "hi"), cid);
    static_cast<void>(rig.finished(turnId, cid));
    auto const other = rig.gui("bob");
    auto const seen = rig.state(turnId, other, "bob");
    CHECK(seen.turnId == 0);
    CHECK(seen.text.empty());
}

TEST_CASE("a_provider_calling_tools_forever_stops_at_the_cap", "[chat][service]") {
    chat::ChatLimits limits;
    limits.maxToolCallsPerTurn = 3;
    Rig rig{limits};
    auto const cid = rig.gui();
    auto const id = rig.instance(cid);
    rig.fake->repeat(chat::Response{
        .text = "",
        .calls = {chat::ToolCall{.id = "1", .name = "notes__Chat_Add", .argumentsJson = R"({"text":"again"})"}}});
    auto const state = rig.finished(rig.start(Rig::turnOn(id, "go"), cid), cid);
    CHECK(noteCount() == 3);
    CHECK(state.status == chat::TurnStatus::Failed);
    CHECK(state.error.contains("limit"));
}

TEST_CASE("a_malformed_tool_call_returns_as_a_tool_error_and_counts", "[chat][service]") {
    Rig rig;
    auto const cid = rig.gui();
    auto const id = rig.instance(cid);
    rig.fake->script(
        chat::Response{.text = "",
                       .calls = {chat::ToolCall{.id = "1", .name = "notes__Chat_Add", .argumentsJson = "[1]"},
                                 chat::ToolCall{.id = "2", .name = "notes__Chat_Add", .argumentsJson = "not json"},
                                 chat::ToolCall{.id = "3", .name = "no_such_tool", .argumentsJson = "{}"}}});
    rig.fake->script(chat::Response{.text = "sorry", .calls = {}, .opaque = {}});
    auto const state = rig.finished(rig.start(Rig::turnOn(id, "add"), cid), cid);
    CHECK(state.status == chat::TurnStatus::Done);
    REQUIRE(state.steps.size() == 3);
    CHECK_FALSE(state.steps.at(0).ok);
    CHECK_FALSE(state.steps.at(1).ok);
    CHECK_FALSE(state.steps.at(2).ok);
    // Refused here with a reason the model can act on, before the server's decoder would have refused it less clearly.
    CHECK(state.steps.at(0).message.contains("JSON object"));
    CHECK(state.steps.at(1).message.contains("JSON object"));
    CHECK(state.steps.at(2).message.contains("no tool"));
    CHECK(noteCount() == 0);
    auto const second = rig.fake->requests().at(1);
    REQUIRE(second.messages.size() == 3);
    REQUIRE(second.messages.at(2).results.size() == 3);
    CHECK(second.messages.at(2).results.at(2).isError);

    chat::ChatLimits one;
    one.maxToolCallsPerTurn = 1;
    Rig capped{one};
    auto const cappedCid = capped.gui();
    auto const cappedId = capped.instance(cappedCid);
    capped.fake->repeat(chat::Response{
        .text = "", .calls = {chat::ToolCall{.id = "1", .name = "no_such_tool", .argumentsJson = "{}"}}});
    CHECK(capped.finished(capped.start(Rig::turnOn(cappedId, "add"), cappedCid), cappedCid).status ==
          chat::TurnStatus::Failed);
}

TEST_CASE("cancelling_a_turn_cancels_its_provider_call", "[chat][service]") {
    Rig rig;
    auto const cid = rig.gui();
    auto const id = rig.instance(cid);
    rig.fake->hold(true);
    rig.fake->script(chat::Response{.text = "too late", .calls = {}, .opaque = {}});
    auto const turnId = rig.start(Rig::turnOn(id, "go"), cid);
    REQUIRE(waitFor([&] { return rig.fake->calls() == 1; }));
    wire::Envelope cancel;
    cancel.kind = std::string{chat::kCancelKind};
    auto const cancelReply = rig.call(std::move(cancel), cid);
    REQUIRE(cancelReply.kind == "ok");
    CHECK(rig.fake->cancelled() == 1);
    auto const state = rig.finished(turnId, cid);
    CHECK(state.status == chat::TurnStatus::Cancelled);
    rig.fake->release();
    morph::testing::storageOwner().runFor(20ms);
    CHECK(rig.state(turnId, cid).text.empty());
}

TEST_CASE("closing_the_connection_stops_its_turn", "[chat][service]") {
    Rig rig;
    auto const cid = rig.gui();
    auto const id = rig.instance(cid);
    rig.fake->hold(true);
    rig.fake->script(chat::Response{.text = "too late", .calls = {}, .opaque = {}});
    static_cast<void>(rig.start(Rig::turnOn(id, "go"), cid));
    REQUIRE(waitFor([&] { return rig.fake->calls() == 1; }));
    rig.server->closeConnection(cid);
    CHECK(waitFor([&] { return rig.fake->cancelled() == 1; }));
}

TEST_CASE("destroying_the_server_during_a_call_is_safe", "[chat][service]") {
    Rig rig;
    auto const cid = rig.gui();
    auto const id = rig.instance(cid);
    rig.fake->hold(true);
    rig.scriptAdd("late");
    static_cast<void>(rig.start(Rig::turnOn(id, "go"), cid));
    REQUIRE(waitFor([&] { return rig.fake->calls() == 1; }));
    rig.chatService.reset();
    rig.server.reset();
    CHECK(rig.fake->cancelled() == 1);
    rig.fake->release();
    morph::testing::storageOwner().runFor(50ms);
    CHECK(noteCount() == 0);
}

TEST_CASE("a_closed_tab_fails_the_tool_and_ends_the_turn", "[chat][service]") {
    Rig rig;
    auto const cid = rig.gui();
    auto const id = rig.instance(cid);
    wire::Envelope gone;
    gone.kind = "deregister";
    gone.modelId = id;
    auto const goneReply = rig.call(std::move(gone), cid);
    REQUIRE(goneReply.kind == "ok");
    rig.scriptAdd("hi");
    auto const state = rig.finished(rig.start(Rig::turnOn(id, "add"), cid), cid);
    REQUIRE(state.steps.size() == 1);
    CHECK_FALSE(state.steps.at(0).ok);
    CHECK(state.steps.at(0).message.contains("model not found"));
    CHECK(state.status == chat::TurnStatus::Done);
}

TEST_CASE("text_over_the_limit_is_refused_and_odd_text_survives", "[chat][service]") {
    chat::ChatLimits limits;
    limits.maxTextBytes = 64;
    Rig rig{limits};
    auto const cid = rig.gui();
    auto const id = rig.instance(cid);
    CHECK(rig.startRaw(Rig::turnOn(id, std::string(65, 'x')), cid).kind == "err");
    CHECK(rig.startRaw(Rig::turnOn(id, ""), cid).kind == "err");
    CHECK(rig.fake->calls() == 0);

    std::string const odd = "line\none \"quoted\" \x01 caf\xC3\xA9";
    rig.fake->script(chat::Response{.text = "ok", .calls = {}, .opaque = {}});
    static_cast<void>(rig.finished(rig.start(Rig::turnOn(id, odd), cid), cid));
    CHECK(rig.fake->requests().at(0).messages.at(0).text == odd);
}

TEST_CASE("a_malformed_turn_body_is_refused", "[chat][service]") {
    Rig rig;
    auto const cid = rig.gui();
    wire::Envelope env;
    env.kind = std::string{chat::kTurnKind};
    env.body = "[1]";
    auto const envReply = rig.call(std::move(env), cid);
    CHECK(envReply.kind == "err");
}

TEST_CASE("a_turn_needs_a_connection", "[chat][service]") {
    Rig rig;
    auto const cid = rig.gui();
    auto const id = rig.instance(cid);
    CHECK(rig.startRaw(Rig::turnOn(id, "hi"), 0).kind == "err");
}

TEST_CASE("a_dropped_final_event_is_recovered_by_reading_state", "[chat][service]") {
    Rig rig;
    auto const cid = rig.gui();
    auto const id = rig.instance(cid);
    rig.eventWire.backlog = morph::backend::kEventBacklogHigh + 1;
    rig.fake->script(chat::Response{.text = "the answer", .calls = {}, .opaque = {}});
    auto const state = rig.finished(rig.start(Rig::turnOn(id, "go"), cid), cid);
    CHECK(state.status == chat::TurnStatus::Done);
    CHECK(state.text == "the answer");
    for (wire::EventBody const& event : rig.eventWire.events()) {
        CHECK(event.topic != chat::kChatTopic);
    }
}

TEST_CASE("events_report_a_turns_progress_in_order", "[chat][service]") {
    Rig rig;
    auto const cid = rig.gui();
    auto const id = rig.instance(cid);
    rig.scriptAdd("hi");
    auto const turnId = rig.start(Rig::turnOn(id, "add"), cid);
    static_cast<void>(rig.finished(turnId, cid));
    std::vector<chat::TurnEvent> seen;
    REQUIRE(waitFor([&] {
        seen.clear();
        for (wire::EventBody const& event : rig.eventWire.events()) {
            if (event.topic == chat::kChatTopic) {
                auto read = chat::readTurnEvent(event.payload.str);
                REQUIRE(read.has_value());
                seen.push_back(*read);
            }
        }
        return !seen.empty() && seen.back().kind == "done";
    }));
    REQUIRE(seen.size() == 3);
    CHECK(seen.at(0).kind == "tool");
    CHECK(seen.at(1).kind == "text");
    CHECK(seen.at(1).text == "done");
    for (std::size_t index = 0; index < seen.size(); ++index) {
        CHECK(seen.at(index).turnId == turnId);
        CHECK(seen.at(index).seq == index + 1);
    }
}

TEST_CASE("no_provider_serves_no_kind", "[chat][service]") {
    Rig rig{chat::ChatLimits{}, false};
    CHECK_FALSE(rig.chatService->serves(chat::kTurnKind));
    CHECK_FALSE(rig.chatService->serves(chat::kStateKind));
    auto const cid = rig.gui();
    wire::Envelope probe;
    probe.kind = std::string{chat::kStateKind};
    auto const probeReply = rig.call(std::move(probe), cid);
    CHECK(probeReply.kind == "err");
}

TEST_CASE("the_opt_out_warning_fires_and_the_tool_result_says_so", "[chat][service]") {
    std::mutex logged;
    std::vector<std::string> warnings;
    morph::log::ScopedLoggerOverride const guard{[&](morph::log::LogLevel level, std::string_view message) {
                                                     if (level == morph::log::LogLevel::warn) {
                                                         std::scoped_lock const lock{logged};
                                                         warnings.emplace_back(message);
                                                     }
                                                 },
                                                 morph::log::LogLevel::warn};
    {
        Rig rig;
        auto const cid = rig.gui();
        auto const id = rig.instance(cid);
        rig.fake->script(chat::Response{
            .text = "",
            .calls = {chat::ToolCall{.id = "1", .name = "notes__Chat_Quiet", .argumentsJson = R"({"text":"x"})"}}});
        rig.fake->script(chat::Response{.text = "done", .calls = {}, .opaque = {}});
        auto const state = rig.finished(rig.start(Rig::turnOn(id, "note"), cid), cid);
        REQUIRE(state.steps.size() == 1);
        CHECK(state.steps.at(0).ok);
        auto const second = rig.fake->requests().at(1);
        CHECK(second.messages.at(2).results.at(0).text.contains("not journaled"));
    }
    std::scoped_lock const lock{logged};
    bool warned = false;
    for (std::string const& warning : warnings) {
        warned = warned || (warning.contains("Chat_Quiet") && warning.contains("journal"));
    }
    CAPTURE(warnings);
    CHECK(warned);
}

TEST_CASE("returning_to_a_record_continues_its_conversation", "[chat][conversation]") {
    Rig rig;
    auto const cid = rig.gui();
    auto const id = rig.instance(cid);
    auto onRecord = [&](std::string identity, std::string text) {
        auto request = Rig::turnOn(id, std::move(text));
        request.identity = std::move(identity);
        return request;
    };
    rig.scriptAdd("first");
    static_cast<void>(rig.finished(rig.start(onRecord("1", "add first"), cid), cid));
    std::size_t const asked = rig.fake->calls();
    rig.fake->script(chat::Response{.text = "elsewhere", .calls = {}, .opaque = {}});
    static_cast<void>(rig.finished(rig.start(onRecord("2", "on another"), cid), cid));
    CHECK(rig.fake->requests().at(asked).messages.size() == 1);
    rig.fake->script(chat::Response{.text = "again", .calls = {}, .opaque = {}});
    static_cast<void>(rig.finished(rig.start(onRecord("1", "and now"), cid), cid));
    // The first turn's words, its call and result, and its reply, then these words.
    auto const messages = rig.fake->requests().at(asked + 1).messages;
    REQUIRE(messages.size() == 5);
    CHECK(messages.front().text == "add first");
    CHECK(messages.at(1).calls.size() == 1);
    CHECK(messages.at(2).results.size() == 1);
    CHECK(messages.at(3).text == "done");
    CHECK(messages.back().text == "and now");
}

TEST_CASE("a_cancelled_request_is_not_carried_into_the_next_turn", "[chat][conversation]") {
    Rig rig;
    auto const cid = rig.gui();
    auto const id = rig.instance(cid);
    rig.fake->hold(true);
    rig.fake->script(chat::Response{.text = "late", .calls = {}, .opaque = {}});
    auto const turnId = rig.start(Rig::turnOn(id, "first"), cid);
    REQUIRE(waitFor([&] { return rig.fake->calls() == 1; }));
    wire::Envelope cancel;
    cancel.kind = std::string{chat::kCancelKind};
    auto const cancelReply = rig.call(std::move(cancel), cid);
    REQUIRE(cancelReply.kind == "ok");
    static_cast<void>(rig.finished(turnId, cid));
    rig.fake->release();
    rig.fake->script(chat::Response{.text = "ok", .calls = {}, .opaque = {}});
    static_cast<void>(rig.finished(rig.start(Rig::turnOn(id, "second"), cid), cid));
    // The person stopped it, so the next turn must not read it as still wanted.
    auto const messages = rig.fake->requests().at(1).messages;
    REQUIRE(messages.size() == 1);
    CHECK(messages.at(0).text == "second");
}

TEST_CASE("end_of_chat_stops_the_turn_and_ends_the_conversations", "[chat][conversation]") {
    Rig rig;
    auto const cid = rig.gui();
    auto const id = rig.instance(cid);
    rig.fake->script(chat::Response{.text = "hello", .calls = {}, .opaque = {}});
    static_cast<void>(rig.finished(rig.start(Rig::turnOn(id, "first"), cid), cid));
    rig.fake->hold(true);
    rig.fake->script(chat::Response{.text = "late", .calls = {}, .opaque = {}});
    auto const running = rig.start(Rig::turnOn(id, "second"), cid);
    REQUIRE(waitFor([&] { return rig.fake->calls() == 2; }));
    wire::Envelope end;
    end.kind = std::string{chat::kEndKind};
    auto const endReply = rig.call(std::move(end), cid);
    REQUIRE(endReply.kind == "ok");
    CHECK(rig.fake->cancelled() == 1);
    CHECK(rig.finished(running, cid).status == chat::TurnStatus::Cancelled);
    rig.fake->release();
    rig.fake->script(chat::Response{.text = "fresh", .calls = {}, .opaque = {}});
    static_cast<void>(rig.finished(rig.start(Rig::turnOn(id, "third"), cid), cid));
    CHECK(rig.fake->requests().at(2).messages.size() == 1);
}

TEST_CASE("end_of_chat_leaves_another_connections_turn_running", "[chat][conversation]") {
    Rig rig;
    auto const other = rig.gui();
    auto const otherInstance = rig.instance(other);
    auto const cid = rig.gui();
    rig.fake->hold(true);
    rig.fake->script(chat::Response{.text = "still here", .calls = {}, .opaque = {}});
    auto const elsewhere = rig.start(Rig::turnOn(otherInstance, "go"), other);
    REQUIRE(waitFor([&] { return rig.fake->calls() == 1; }));
    wire::Envelope end;
    end.kind = std::string{chat::kEndKind};
    auto const endReply = rig.call(std::move(end), cid);
    REQUIRE(endReply.kind == "ok");
    CHECK(rig.fake->cancelled() == 0);
    rig.fake->release();
    CHECK(rig.finished(elsewhere, other).status == chat::TurnStatus::Done);
}

TEST_CASE("the_turn_caps_hold", "[chat][conversation]") {
    chat::ChatLimits limits;
    limits.maxTurnsPerPrincipal = 2;
    limits.maxTurnsPerConnection = 1;
    Rig rig{limits};
    auto const one = rig.gui();
    auto const oneInstance = rig.instance(one);
    auto const two = rig.gui();
    auto const twoInstance = rig.instance(two);
    auto const three = rig.gui();
    auto const threeInstance = rig.instance(three);
    rig.fake->hold(true);
    rig.fake->repeat(chat::Response{.text = "held", .calls = {}, .opaque = {}});
    auto const first = rig.start(Rig::turnOn(oneInstance, "go"), one);
    CHECK(rig.startRaw(Rig::turnOn(oneInstance, "again"), one).kind == "err");
    auto const second = rig.start(Rig::turnOn(twoInstance, "go"), two);
    CHECK(rig.startRaw(Rig::turnOn(threeInstance, "go"), three).kind == "err");
    // A cap counts running turns, so a finished one frees its place.
    REQUIRE(waitFor([&] { return rig.fake->calls() == 2; }));
    rig.fake->release();
    static_cast<void>(rig.finished(first, one));
    static_cast<void>(rig.finished(second, two));
    auto const third = rig.start(Rig::turnOn(threeInstance, "go"), three);
    // The worker must outlive every turn, so the rig is torn down only once this one is over.
    CHECK(rig.finished(third, three).status == chat::TurnStatus::Done);
}

// Mutation: leave a stopped turn's completed rounds out, so the model no longer knows what already happened.
TEST_CASE("a_turn_stopped_after_a_tool_ran_keeps_what_happened", "[chat][conversation]") {
    chat::ChatLimits limits;
    limits.maxToolCallsPerTurn = 1;
    Rig rig{limits};
    auto const cid = rig.gui();
    auto const id = rig.instance(cid);
    // Two rounds: the first call runs, and the second hits the limit, which fails the turn.
    rig.fake->script(chat::Response{
        .text = "",
        .calls = {chat::ToolCall{.id = "1", .name = "notes__Chat_Add", .argumentsJson = R"({"text":"a"})"}},
        .opaque = {}});
    rig.fake->script(chat::Response{
        .text = "",
        .calls = {chat::ToolCall{.id = "2", .name = "notes__Chat_Add", .argumentsJson = R"({"text":"b"})"}},
        .opaque = {}});
    CHECK(rig.finished(rig.start(Rig::turnOn(id, "add two"), cid), cid).status == chat::TurnStatus::Failed);
    rig.fake->script(chat::Response{.text = "ok", .calls = {}, .opaque = {}});
    static_cast<void>(rig.finished(rig.start(Rig::turnOn(id, "next"), cid), cid));
    auto const messages = rig.fake->requests().back().messages;
    REQUIRE(messages.size() == 4);
    CHECK(messages.at(0).text == "add two");
    CHECK(messages.at(1).calls.size() == 1);
    CHECK(messages.at(2).results.size() == 1);
    CHECK(messages.at(3).text == "next");
}

// Mutation: pass a tool's whole result along.
TEST_CASE("a_large_tool_result_is_cut_and_the_turn_stays_readable", "[chat][service]") {
    chat::ChatLimits limits;
    limits.maxToolResultBytes = 200;
    Rig rig{limits};
    auto const cid = rig.gui();
    auto const id = rig.instance(cid);
    std::string const big(5000, 'x');
    rig.fake->script(
        chat::Response{.text = "",
                       .calls = {chat::ToolCall{
                           .id = "1", .name = "notes__Chat_Add", .argumentsJson = R"({"text":")" + big + R"("})"}},
                       .opaque = {}});
    rig.fake->script(chat::Response{.text = "done", .calls = {}, .opaque = {}});
    auto const state = rig.finished(rig.start(Rig::turnOn(id, "add a lot"), cid), cid);
    CHECK(state.status == chat::TurnStatus::Done);
    REQUIRE(state.steps.size() == 1);
    CHECK(state.steps.at(0).ok);
    CHECK(state.steps.at(0).message.size() < 1000);
    auto const sent = rig.fake->requests().at(1).messages.at(2).results.at(0).text;
    CHECK(sent.size() < 400);
    // The model is told, so it does not take a part for the whole.
    CHECK(sent.contains("cut off"));
}

// Mutation: leave a provider's failure out of the turn's state.
TEST_CASE("a_failing_provider_fails_the_turn_and_says_why", "[chat][service]") {
    Rig rig;
    auto const cid = rig.gui();
    auto const id = rig.instance(cid);
    auto const state = rig.finished(rig.start(Rig::turnOn(id, "hello"), cid), cid);
    CHECK(state.status == chat::TurnStatus::Failed);
    CHECK(state.error.contains("script ran out"));
}

// Mutation: show only what was streamed.
TEST_CASE("a_provider_that_answers_only_at_the_end_still_shows_its_text", "[chat][service]") {
    auto const provider = std::make_shared<ManualProvider>();
    Rig rig{{}, true, provider};
    auto const cid = rig.gui();
    auto const id = rig.instance(cid);
    auto const turnId = rig.start(Rig::turnOn(id, "hello"), cid);
    provider->called().done(chat::Response{.text = "the whole reply", .calls = {}, .opaque = {}});
    auto const state = rig.finished(turnId, cid);
    CHECK(state.status == chat::TurnStatus::Done);
    CHECK(state.text == "the whole reply");
    CHECK(waitFor([&] {
        auto const events = rig.eventWire.events();
        return std::ranges::any_of(events, [](wire::EventBody const& event) {
            return event.topic == chat::kChatTopic && event.payload.str.contains("the whole reply");
        });
    }));
}

// Mutation: take a provider's text whatever the turn's status.
TEST_CASE("a_provider_speaking_after_a_cancel_changes_nothing", "[chat][service]") {
    auto const provider = std::make_shared<ManualProvider>();
    Rig rig{{}, true, provider};
    auto const cid = rig.gui();
    auto const id = rig.instance(cid);
    auto const turnId = rig.start(Rig::turnOn(id, "hello"), cid);
    auto const sink = provider->called();
    wire::Envelope cancel;
    cancel.kind = std::string{chat::kCancelKind};
    auto const cancelReply = rig.call(std::move(cancel), cid);
    REQUIRE(cancelReply.kind == "ok");
    sink.text("late");
    sink.done(chat::Response{.text = "late", .calls = {}, .opaque = {}});
    morph::testing::storageOwner().runFor(50ms);
    auto const state = rig.state(turnId, cid);
    CHECK(state.status == chat::TurnStatus::Cancelled);
    CHECK(state.text.empty());
}

// Mutation: refuse a call with no arguments as not being a JSON object.
TEST_CASE("a_tool_call_with_no_arguments_runs_with_an_empty_object", "[chat][service]") {
    Rig rig;
    auto const cid = rig.gui();
    auto const id = rig.instance(cid);
    rig.fake->script(
        chat::Response{.text = "",
                       .calls = {chat::ToolCall{.id = "1", .name = "notes__Chat_Wipe", .argumentsJson = ""}},
                       .opaque = {}});
    rig.fake->script(chat::Response{.text = "wiped", .calls = {}, .opaque = {}});
    auto const state = rig.finished(rig.start(Rig::turnOn(id, "wipe"), cid), cid);
    CHECK(state.status == chat::TurnStatus::Done);
    REQUIRE(state.steps.size() == 1);
    CHECK(state.steps.at(0).ok);
}

// Mutation: keep every finished turn.
TEST_CASE("only_the_latest_finished_turns_can_be_read", "[chat][service]") {
    Rig rig;
    auto const cid = rig.gui();
    auto const id = rig.instance(cid);
    std::vector<std::uint64_t> turns;
    for (int count = 0; count < 9; ++count) {
        rig.fake->script(chat::Response{.text = "done", .calls = {}, .opaque = {}});
        turns.push_back(rig.start(Rig::turnOn(id, "hello"), cid));
        static_cast<void>(rig.finished(turns.back(), cid));
    }
    CHECK(rig.state(turns.front(), cid).turnId == 0);
    CHECK(rig.state(turns.at(1), cid).turnId == turns.at(1));
    CHECK(rig.state(turns.back(), cid).status == chat::TurnStatus::Done);
}

// Mutation: answer an unreadable request as if it named no turn.
TEST_CASE("a_state_request_names_a_turn_or_none_and_a_malformed_one_is_refused", "[chat][service]") {
    Rig rig;
    auto const cid = rig.gui();
    wire::Envelope none;
    none.kind = std::string{chat::kStateKind};
    auto const noneReply = rig.call(std::move(none), cid);
    REQUIRE(noneReply.kind == "ok");
    auto const read = chat::readTurnState(noneReply.body);
    REQUIRE(read.has_value());
    CHECK(read->turnId == 0);
    CHECK(rig.state(9999, cid).turnId == 0);
    wire::Envelope malformed;
    malformed.kind = std::string{chat::kStateKind};
    malformed.body = "[1]";
    auto const malformedReply = rig.call(std::move(malformed), cid);
    CHECK(malformedReply.kind == "err");
}

// Mutation: build a service with a provider and nothing to run it on.
TEST_CASE("a_provider_needs_a_worker_and_a_bundle", "[chat][service]") {
    Rig rig;
    chat::ChatOptions noWorker;
    noWorker.provider = rig.fake;
    noWorker.worker = nullptr;
    noWorker.bundle = rig.bundle;
    CHECK_THROWS_AS(chat::ChatService{std::move(noWorker)}, std::invalid_argument);
    chat::ChatOptions noBundle;
    noBundle.provider = rig.fake;
    noBundle.worker = &rig.worker;
    noBundle.bundle = nullptr;
    CHECK_THROWS_AS(chat::ChatService{std::move(noBundle)}, std::invalid_argument);
}

// Mutation: answer every kind.
TEST_CASE("the_service_answers_only_the_chat_kinds", "[chat][service]") {
    Rig const rig;
    CHECK(rig.chatService->serves(chat::kEndKind));
    CHECK_FALSE(rig.chatService->serves("ui-chat-unknown"));
}

// Mutation: cancel every running turn, or count another principal's turn against this one.
TEST_CASE("cancelling_stops_only_this_connections_turn", "[chat][service]") {
    Rig rig;
    auto const mine = rig.gui();
    auto const theirs = rig.gui("bob");
    auto const myInstance = rig.instance(mine);
    auto const theirInstance = rig.instance(theirs, "bob");
    rig.fake->hold(true);
    rig.fake->script(chat::Response{.text = "mine", .calls = {}, .opaque = {}});
    rig.fake->script(chat::Response{.text = "theirs", .calls = {}, .opaque = {}});
    auto const myTurn = rig.start(Rig::turnOn(myInstance, "go"), mine);
    REQUIRE(waitFor([&] { return rig.fake->calls() == 1; }));
    auto const theirTurn = rig.start(Rig::turnOn(theirInstance, "go"), theirs, "bob");
    REQUIRE(waitFor([&] { return rig.fake->calls() == 2; }));
    wire::Envelope cancel;
    cancel.kind = std::string{chat::kCancelKind};
    auto const cancelReply = rig.call(std::move(cancel), mine);
    REQUIRE(cancelReply.kind == "ok");
    CHECK(rig.fake->cancelled() == 1);
    CHECK(rig.state(myTurn, mine).status == chat::TurnStatus::Cancelled);
    CHECK(rig.state(theirTurn, theirs, "bob").status == chat::TurnStatus::Running);
    rig.fake->release();
    CHECK(rig.finished(theirTurn, theirs, "bob").status == chat::TurnStatus::Done);
}

// Mutation: cut at the limit whatever falls there.
TEST_CASE("a_cut_tool_result_never_splits_a_character", "[chat][service]") {
    chat::ChatLimits limits;
    // The note's reply opens with 16 bytes of ASCII, so the limit falls inside the first two-byte character.
    limits.maxToolResultBytes = 17;
    Rig rig{limits};
    auto const cid = rig.gui();
    auto const id = rig.instance(cid);
    rig.scriptAdd("\xC3\xA9\xC3\xA9\xC3\xA9");
    auto const state = rig.finished(rig.start(Rig::turnOn(id, "add"), cid), cid);
    REQUIRE(state.status == chat::TurnStatus::Done);
    auto const sent = rig.fake->requests().at(1).messages.at(2).results.at(0).text;
    CHECK(sent.starts_with(std::string{R"({"id":1,"text":")"} + "\n[cut off"));
}
