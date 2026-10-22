// SPDX-License-Identifier: Apache-2.0

// The MCP adapter (spec 8 §11): an agent's tool calls run through the server on a connection of their own, with every
// check a GUI's calls get, and a GUI watching what they change is told.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/records.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/agent.hpp>
#include <morph/ui/author.hpp>
#include <morph/ui/service.hpp>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// NOLINTBEGIN(misc-use-internal-linkage): registered types must have external linkage.
namespace agent_test {

struct Note {
    std::int64_t id = 0;
    std::string text;
};
struct ListNotes {};
struct AddNote {
    std::string text;
};
struct WipeNotes {};
struct Tally {
    std::string label;
};

struct Store {
    std::mutex mutex;
    std::vector<Note> notes;
};
inline Store& store() {
    static Store shared;
    return shared;
}

struct NotesModel {
    std::vector<Note> execute(ListNotes const& /*action*/) {
        std::scoped_lock const lock{store().mutex};
        return store().notes;
    }
    Note execute(AddNote const& action) {
        std::scoped_lock const lock{store().mutex};
        store().notes.push_back(Note{.id = static_cast<std::int64_t>(store().notes.size()) + 1, .text = action.text});
        return store().notes.back();
    }
    bool execute(WipeNotes const& /*action*/) {
        std::scoped_lock const lock{store().mutex};
        store().notes.clear();
        return true;
    }
};

// Keyed: each key is an instance of its own, which counts the tallies made on it.
struct ShelfModel {
    using PrimaryKey = std::string;
    int count = 0;
    int execute(Tally const& /*action*/) { return ++count; }
};

}  // namespace agent_test
// NOLINTEND(misc-use-internal-linkage)

BRIDGE_REGISTER_MODEL(agent_test::NotesModel, "Ag_Notes")
BRIDGE_REGISTER_ACTION(agent_test::NotesModel, agent_test::ListNotes, "Ag_List")
BRIDGE_REGISTER_ACTION(agent_test::NotesModel, agent_test::AddNote, "Ag_Add")
BRIDGE_REGISTER_ACTION(agent_test::NotesModel, agent_test::WipeNotes, "Ag_Wipe")
BRIDGE_REGISTER_MODEL(agent_test::ShelfModel, "Ag_Shelf")
BRIDGE_REGISTER_ACTION(agent_test::ShelfModel, agent_test::Tally, "Ag_Tally")
BRIDGE_REGISTER_RECORD(agent_test::Note, "Ag_Note")
BRIDGE_REGISTER_EFFECTS(agent_test::AddNote, morph::effects::changes<agent_test::Note>)
BRIDGE_REGISTER_EFFECTS(agent_test::WipeNotes, morph::effects::changes<agent_test::Note>)

namespace {

using namespace std::chrono_literals;
namespace ui = morph::ui;

// A screen naming every action, so the catalog lists each one the principal may run.
constexpr std::string_view kNotes = R"({
  "vocab": "ui/1", "id": "notes",
  "models": { "notes": { "type": "Ag_Notes" } },
  "queries": { "list": { "model": "notes", "action": "Ag_List" } },
  "mutations": { "add": { "model": "notes", "action": "Ag_Add" }, "wipe": { "model": "notes", "action": "Ag_Wipe" } },
  "view": { "kind": "text", "text": "Notes" } })";

// A screen over one shelf, a keyed instance.
constexpr std::string_view kShelf = R"({
  "vocab": "ui/1", "id": "shelf", "params": { "shelfKey": "string" },
  "models": { "shelf": { "type": "Ag_Shelf", "instance": { "ref": "shelfKey" } } },
  "mutations": { "tally": { "model": "shelf", "action": "Ag_Tally" } },
  "view": { "kind": "text", "text": "Shelf" } })";

// The token is the principal; `agent` may not wipe.
struct Authorizer final : morph::session::IAuthorizer {
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): IAuthorizer's signature.
    [[nodiscard]] bool authorize(morph::session::Context const& ctx, std::string_view /*modelType*/,
                                 std::string_view actionType) const override {
        return !(ctx.principal == "agent" && actionType == "Ag_Wipe");
    }
    [[nodiscard]] std::optional<std::string> authenticate(morph::session::Context const& ctx) const override {
        return ctx.token.empty() ? std::nullopt : std::optional{ctx.token};
    }
};

// What the server wrote to a GUI connection: its events.
struct EventWire {
    std::mutex mutex;
    std::vector<std::string> sent;

    morph::backend::ConnectionWriter writer() {
        return {.write =
                    [this](std::string frame) {
                        std::scoped_lock const lock{mutex};
                        sent.push_back(std::move(frame));
                    },
                .backlog = [] { return std::size_t{0}; }};
    }

    std::size_t count() {
        std::scoped_lock const lock{mutex};
        return sent.size();
    }
};

struct Rig {
    Rig() {
        {
            std::scoped_lock const lock{agent_test::store().mutex};
            agent_test::store().notes.clear();
        }
        ui::author::CatalogFacts facts;
        facts.action<agent_test::NotesModel, agent_test::ListNotes>();
        facts.action<agent_test::NotesModel, agent_test::AddNote>();
        facts.action<agent_test::NotesModel, agent_test::WipeNotes>();
        facts.action<agent_test::ShelfModel, agent_test::Tally>();
        ui::UiRegistry registry{ui::ApplicationInfo{.id = "notes", .version = "1"}, facts.catalog()};
        ui::CatalogEntry entry;
        entry.id = "notes";
        REQUIRE(registry.addScreen(entry, std::string{kNotes}).has_value());
        ui::CatalogEntry shelf;
        shelf.id = "shelf";
        REQUIRE(registry.addScreen(shelf, std::string{kShelf}).has_value());
        auto built = registry.build();
        REQUIRE(built.has_value());
        morph::backend::ServerConfig config;
        config.services.push_back(std::make_shared<ui::UiService>(*built));
        server =
            std::make_shared<morph::backend::RemoteServer>(pool, std::make_shared<Authorizer>(), std::move(config));
    }

    // A GUI connection that negotiated events and watches `Ag_Note`.
    morph::backend::ConnectionId gui() {
        auto const cid = server->openConnection();
        server->setConnectionWriter(cid, wire.writer());
        morph::wire::Envelope hello;
        hello.kind = std::string{ui::kHelloKind};
        hello.callId = 1;
        hello.session.token = "gui";
        hello.body = R"({"vocabularies":["ui/1","expr/1"],"events":true})";
        REQUIRE(morph::wire::decode(server->handleInline(morph::wire::encode(hello), cid)).kind == "ok");
        morph::wire::Envelope watch;
        watch.kind = std::string{morph::wire::kWatchKind};
        watch.callId = 2;
        watch.session.token = "gui";
        watch.body = R"({"records":["Ag_Note"],"instances":[]})";
        REQUIRE(morph::wire::decode(server->handleInline(morph::wire::encode(watch), cid)).kind == "ok");
        return cid;
    }

    std::unique_ptr<ui::McpAdapter> adapter(std::string token = "agent") {
        // `authorize` reads the session as sent, so it carries the principal its token proves.
        morph::session::Context session;
        session.principal = token;
        session.token = std::move(token);
        return std::make_unique<ui::McpAdapter>(server, ui::McpOptions{.session = session, .callbacks = &callbacks});
    }

    morph::exec::ThreadPoolExecutor pool{2};
    // The adapter's owner, which this thread pumps: declared before the server, so it outlives every reply.
    morph::exec::MainThreadExecutor callbacks;
    std::shared_ptr<morph::backend::RemoteServer> server;
    EventWire wire;
};

// The adapter's reply to @p message, waited for on the adapter's owner.
std::optional<std::string> ask(Rig& rig, ui::McpAdapter& adapter, std::string const& message) {
    std::optional<std::optional<std::string>> replied;
    adapter.handle(message).then([&replied](std::optional<std::string> const& answer) { replied = answer; });
    auto const deadline = std::chrono::steady_clock::now() + 10s;
    while (!replied && std::chrono::steady_clock::now() < deadline) {
        rig.callbacks.runFor(10ms);
    }
    REQUIRE(replied.has_value());
    return *replied;
}

ui::Json json(std::optional<std::string> const& text) {
    REQUIRE(text.has_value());
    auto parsed = ui::parseJson(*text);
    REQUIRE(parsed.has_value());
    return *parsed;
}

std::string dump(ui::Json const& value) { return glz::write_json(value).value_or(std::string{}); }

std::vector<std::string> toolNames(ui::Json const& reply) {
    std::vector<std::string> names;
    for (auto const& tool : std::get<ui::Json::array_t>(reply["result"]["tools"].data)) {
        names.push_back(std::get<std::string>(tool["name"].data));
    }
    return names;
}

bool waitFor(auto done) {
    auto const deadline = std::chrono::steady_clock::now() + 5s;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

constexpr std::string_view kAdd =
    R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"Ag_Add","arguments":{"text":"hello"}}})";

}  // namespace

TEST_CASE("initialize_answers_capabilities", "[push][agent]") {
    Rig rig;
    auto adapter = rig.adapter();
    auto const reply = json(ask(rig, *adapter,
                                R"({"jsonrpc":"2.0","id":1,"method":"initialize",)"
                                R"("params":{"protocolVersion":"2025-06-18","capabilities":{}}})"));
    CHECK(std::get<std::string>(reply["jsonrpc"].data) == "2.0");
    CHECK(dump(reply["id"]) == "1");
    CHECK(std::get<std::string>(reply["result"]["protocolVersion"].data) == "2025-06-18");
    CHECK(reply["result"]["capabilities"].contains("tools"));
    CHECK(std::get<std::string>(reply["result"]["serverInfo"]["name"].data) == "morph");
}

// Mutation: list every action of the catalog, not only the permitted ones (Ag_Wipe appears).
TEST_CASE("tools_list_is_permitted_plus_evaluate", "[push][agent]") {
    Rig rig;
    auto adapter = rig.adapter();
    auto const reply = json(ask(rig, *adapter, R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})"));
    std::vector<std::string> names = toolNames(reply);
    std::ranges::sort(names);
    CHECK(names == std::vector<std::string>{"Ag_Add", "Ag_List", "Ag_Tally", "evaluate"});
    for (auto const& tool : std::get<ui::Json::array_t>(reply["result"]["tools"].data)) {
        if (std::get<std::string>(tool["name"].data) == "Ag_Add") {
            CHECK(tool["inputSchema"]["properties"].contains("text"));
        }
    }
}

TEST_CASE("tools_call_runs_and_returns_result_text", "[push][agent]") {
    Rig rig;
    auto adapter = rig.adapter();
    auto const reply = json(ask(rig, *adapter, std::string{kAdd}));
    CHECK_FALSE(std::get<bool>(reply["result"]["isError"].data));
    auto const text = std::get<std::string>(reply["result"]["content"][0]["text"].data);
    CHECK(text == R"({"id":1,"text":"hello"})");
    auto const listed = json(ask(rig, *adapter,
                                 R"({"jsonrpc":"2.0","id":4,"method":"tools/call",)"
                                 R"("params":{"name":"Ag_List","arguments":{}}})"));
    CHECK(std::get<std::string>(listed["result"]["content"][0]["text"].data) == R"([{"id":1,"text":"hello"}])");
    // No `arguments` member is the same call with none.
    auto const bare =
        json(ask(rig, *adapter, R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"Ag_List"}})"));
    REQUIRE(bare.contains("result"));
    CHECK(std::get<std::string>(bare["result"]["content"][0]["text"].data) == R"([{"id":1,"text":"hello"}])");
}

// Mutation: run the call without the session (the server refuses an unsigned execute) — or with it, through a path
// that skips `authorize` (the wipe runs and posts an event).
TEST_CASE("refused_call_is_error_and_posts_nothing", "[push][agent]") {
    Rig rig;
    auto const gui = rig.gui();
    static_cast<void>(gui);
    auto adapter = rig.adapter();
    auto const reply = json(ask(rig, *adapter,
                                R"({"jsonrpc":"2.0","id":5,"method":"tools/call",)"
                                R"("params":{"name":"Ag_Wipe","arguments":{}}})"));
    CHECK(std::get<bool>(reply["result"]["isError"].data));
    // A refused call changes nothing, so no event follows; give the coalescer its window to prove it.
    std::this_thread::sleep_for(200ms);
    CHECK(rig.wire.count() == 0);
}

// Mutation: open no connection of the adapter's own (call unscoped) — the GUI's watch still sees it, so instead drop
// the effects declaration of Ag_Add: no event reaches the GUI.
TEST_CASE("call_reaches_watching_gui_connection_as_event", "[push][agent]") {
    Rig rig;
    auto const gui = rig.gui();
    static_cast<void>(gui);
    auto adapter = rig.adapter();
    auto const reply = json(ask(rig, *adapter, std::string{kAdd}));
    REQUIRE_FALSE(std::get<bool>(reply["result"]["isError"].data));
    REQUIRE(waitFor([&] { return rig.wire.count() >= 1; }));
    std::scoped_lock const lock{rig.wire.mutex};
    auto const event = morph::wire::decode(rig.wire.sent.front());
    CHECK(event.kind == morph::wire::kEventKind);
    CHECK(event.body.contains("Ag_Note"));
}

TEST_CASE("unknown_method_is_32601", "[push][agent]") {
    Rig rig;
    auto adapter = rig.adapter();
    auto const reply = json(ask(rig, *adapter, R"({"jsonrpc":"2.0","id":"x","method":"resources/list"})"));
    CHECK(dump(reply["id"]) == R"("x")");
    CHECK(dump(reply["error"]["code"]) == "-32601");
}

TEST_CASE("notification_gets_no_reply", "[push][agent]") {
    Rig rig;
    auto adapter = rig.adapter();
    CHECK_FALSE(ask(rig, *adapter, R"({"jsonrpc":"2.0","method":"notifications/initialized"})").has_value());
    CHECK_FALSE(ask(rig, *adapter, R"({"jsonrpc":"2.0","method":"tools/list"})").has_value());
}

TEST_CASE("evaluate_is_a_dry_run", "[push][agent]") {
    Rig rig;
    auto adapter = rig.adapter();
    auto const reply = json(ask(rig, *adapter,
                                R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"evaluate",)"
                                R"("arguments":{"action":"Ag_Add","body":{"text":"draft"}}}})"));
    CHECK_FALSE(std::get<bool>(reply["result"]["isError"].data));
    std::scoped_lock const lock{agent_test::store().mutex};
    CHECK(agent_test::store().notes.empty());
}

TEST_CASE("stdio_round_trip", "[push][agent]") {
    Rig rig;
    auto adapter = rig.adapter();
    std::istringstream in{R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})"
                          "\n\n"
                          R"({"jsonrpc":"2.0","method":"notifications/initialized"})"
                          "\n"
                          R"({"jsonrpc":"2.0","id":2,"method":"ping"})"
                          "\n"};
    std::ostringstream out;
    ui::serveStdio(*adapter, rig.callbacks, in, out);
    std::istringstream lines{out.str()};
    std::vector<std::string> replies;
    for (std::string line; std::getline(lines, line);) {
        replies.push_back(line);
    }
    REQUIRE(replies.size() == 2);
    CHECK(dump(json(replies.at(0))["id"]) == "1");
    CHECK(dump(json(replies.at(1))["id"]) == "2");
}

// Mutation: register a private instance for each key — another connection tallies a shelf of its own.
TEST_CASE("keyed_tool_runs_on_the_instance_it_names", "[push][agent]") {
    Rig rig;
    auto adapter = rig.adapter();
    auto const listed = json(ask(rig, *adapter, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"));
    bool found = false;
    for (auto const& tool : std::get<ui::Json::array_t>(listed["result"]["tools"].data)) {
        if (std::get<std::string>(tool["name"].data) == "Ag_Tally") {
            found = true;
            CHECK(tool["inputSchema"]["properties"].contains("instance"));
            CHECK(dump(tool["inputSchema"]["required"]) == R"(["label","instance"])");
        }
    }
    REQUIRE(found);

    auto const tally = [&](std::string_view instance, ui::McpAdapter& through) {
        auto const reply = json(ask(rig, through,
                                    R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"Ag_Tally",)"
                                    R"("arguments":{"label":"x","instance":)" +
                                        std::string{instance} + "}}}"));
        REQUIRE_FALSE(std::get<bool>(reply["result"]["isError"].data));
        return std::get<std::string>(reply["result"]["content"][0]["text"].data);
    };
    CHECK(tally(R"("a")", *adapter) == "1");
    CHECK(tally(R"("a")", *adapter) == "2");
    CHECK(tally(R"("b")", *adapter) == "1");
    // The instance is the shared one under that key, so another connection's call reaches it too.
    auto other = rig.adapter();
    CHECK(tally(R"("a")", *other) == "3");

    auto const keyless = json(ask(rig, *adapter,
                                  R"({"jsonrpc":"2.0","id":3,"method":"tools/call",)"
                                  R"("params":{"name":"Ag_Tally","arguments":{"label":"x"}}})"));
    CHECK(dump(keyless["error"]["code"]) == "-32602");
}

TEST_CASE("faults_answer_as_json_rpc_errors", "[push][agent]") {
    Rig rig;
    auto adapter = rig.adapter();
    auto const unparsed = json(ask(rig, *adapter, "{not json"));
    CHECK(dump(unparsed["id"]) == "null");
    CHECK(dump(unparsed["error"]["code"]) == "-32700");
    auto const noMethod = json(ask(rig, *adapter, R"({"jsonrpc":"2.0","id":7})"));
    CHECK(dump(noMethod["id"]) == "7");
    CHECK(dump(noMethod["error"]["code"]) == "-32600");
    auto const noTool = json(ask(rig, *adapter, R"({"jsonrpc":"2.0","id":8,"method":"tools/call","params":{}})"));
    CHECK(dump(noTool["error"]["code"]) == "-32602");
    auto const unknownTool = json(ask(rig, *adapter,
                                      R"({"jsonrpc":"2.0","id":9,"method":"tools/call",)"
                                      R"("params":{"name":"Ag_Missing","arguments":{}}})"));
    CHECK(dump(unknownTool["error"]["code"]) == "-32602");
    auto const badArguments = json(ask(rig, *adapter,
                                       R"({"jsonrpc":"2.0","id":10,"method":"tools/call",)"
                                       R"("params":{"name":"Ag_Add","arguments":[1]}})"));
    CHECK(dump(badArguments["error"]["code"]) == "-32602");
}

TEST_CASE("evaluate_tool_requires_action_and_body", "[push][agent]") {
    Rig rig;
    auto adapter = rig.adapter();
    auto const reply = json(ask(rig, *adapter, R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})"));
    for (auto const& tool : std::get<ui::Json::array_t>(reply["result"]["tools"].data)) {
        if (std::get<std::string>(tool["name"].data) == "evaluate") {
            CHECK(dump(tool["inputSchema"]["required"]) == R"(["action","body"])");
        }
    }
    auto const unknown = json(ask(rig, *adapter,
                                  R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"evaluate",)"
                                  R"("arguments":{"action":"Ag_Missing","body":{}}}})"));
    CHECK(dump(unknown["error"]["code"]) == "-32602");
}
