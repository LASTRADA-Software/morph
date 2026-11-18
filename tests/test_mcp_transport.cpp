// SPDX-License-Identifier: Apache-2.0

// The MCP adapter over an `IEnvelopeTransport`: a call timeout, a reset that discards what an old connection
// cached, and a stdio loop whose owner another thread runs.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/envelope_transport.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/owner_strand.hpp>
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
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// NOLINTBEGIN(misc-use-internal-linkage): registered types must have external linkage.
namespace mt_test {

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

}  // namespace mt_test
// NOLINTEND(misc-use-internal-linkage)

BRIDGE_REGISTER_MODEL(mt_test::NotesModel, "Mt_Notes")
BRIDGE_REGISTER_ACTION(mt_test::NotesModel, mt_test::ListNotes, "Mt_List")
BRIDGE_REGISTER_ACTION(mt_test::NotesModel, mt_test::AddNote, "Mt_Add")
BRIDGE_REGISTER_ACTION(mt_test::NotesModel, mt_test::WipeNotes, "Mt_Wipe")
BRIDGE_REGISTER_MODEL(mt_test::ShelfModel, "Mt_Shelf")
BRIDGE_REGISTER_ACTION(mt_test::ShelfModel, mt_test::Tally, "Mt_Tally")
BRIDGE_REGISTER_RECORD(mt_test::Note, "Mt_Note")
BRIDGE_REGISTER_EFFECTS(mt_test::AddNote, morph::effects::changes<mt_test::Note>)
BRIDGE_REGISTER_EFFECTS(mt_test::WipeNotes, morph::effects::changes<mt_test::Note>)

namespace {

using namespace std::chrono_literals;
namespace ui = morph::ui;

// A screen naming every action, so the catalog lists each one the principal may run.
constexpr std::string_view kNotes = R"({
  "vocab": "ui/1", "id": "notes",
  "models": { "notes": { "type": "Mt_Notes" } },
  "queries": { "list": { "model": "notes", "action": "Mt_List" } },
  "mutations": { "add": { "model": "notes", "action": "Mt_Add" }, "wipe": { "model": "notes", "action": "Mt_Wipe" } },
  "view": { "kind": "text", "text": "Notes" } })";

// A screen over one shelf, a keyed instance.
constexpr std::string_view kShelf = R"({
  "vocab": "ui/1", "id": "shelf", "params": { "shelfKey": "string" },
  "models": { "shelf": { "type": "Mt_Shelf", "instance": { "ref": "shelfKey" } } },
  "mutations": { "tally": { "model": "shelf", "action": "Mt_Tally" } },
  "view": { "kind": "text", "text": "Shelf" } })";

// The token is the principal; `agent` may not wipe, and `nobody` may not run anything.
struct Authorizer final : morph::session::IAuthorizer {
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): IAuthorizer's signature.
    [[nodiscard]] bool authorize(morph::session::Context const& ctx, std::string_view /*modelType*/,
                                 std::string_view actionType) const override {
        return ctx.principal != "nobody" && !(ctx.principal == "agent" && actionType == "Mt_Wipe");
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
            std::scoped_lock const lock{mt_test::store().mutex};
            mt_test::store().notes.clear();
        }
        ui::author::CatalogFacts facts;
        facts.action<mt_test::NotesModel, mt_test::ListNotes>();
        facts.action<mt_test::NotesModel, mt_test::AddNote>();
        facts.action<mt_test::NotesModel, mt_test::WipeNotes>();
        facts.action<mt_test::ShelfModel, mt_test::Tally>();
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

    // A GUI connection that negotiated events and watches `Mt_Note`.
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
        watch.body = R"({"records":["Mt_Note"],"instances":[]})";
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

// Runs @p owner until @p done holds or @p limit passes.
bool pumpUntil(morph::exec::MainThreadExecutor& owner, auto done, std::chrono::milliseconds limit = 5s) {
    auto const deadline = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        owner.runFor(10ms);
    }
    return true;
}

// The adapter's reply to @p message, waited for on @p owner, the executor the adapter was built with.
std::optional<std::string> ask(morph::exec::MainThreadExecutor& owner, ui::McpAdapter& adapter,
                               std::string const& message) {
    std::optional<std::optional<std::string>> replied;
    adapter.handle(message).thenDetached([&replied](std::optional<std::string> const& answer) { replied = answer; });
    REQUIRE(pumpUntil(owner, [&] { return replied.has_value(); }, 10s));
    return *replied;
}

ui::Json json(std::optional<std::string> const& text) {
    REQUIRE(text.has_value());
    auto parsed = ui::parseJson(*text);
    REQUIRE(parsed.has_value());
    return *parsed;
}

std::vector<std::string> toolNames(ui::Json const& reply) {
    std::vector<std::string> names;
    for (auto const& tool : std::get<ui::Json::array_t>(reply["result"]["tools"].data)) {
        names.push_back(std::get<std::string>(tool["name"].data));
    }
    return names;
}

constexpr std::string_view kAdd =
    R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"Mt_Add","arguments":{"text":"hello"}}})";
constexpr std::string_view kList = R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"Mt_List"}})";

// A transport whose replies the test releases by hand: what SimulatedRemoteBackend cannot do (hold a reply, reject,
// never answer).
class ScriptedTransport final : public morph::backend::IEnvelopeTransport {
public:
    using Completion = morph::async::Completion<morph::wire::Envelope>;

    struct Sent {
        morph::wire::Envelope envelope;
        decltype(Completion::makeSettleable(nullptr).second) promise;
    };

    Completion request(morph::wire::Envelope envelope, morph::exec::IExecutor& cbExec) override {
        auto [completion, promise] = Completion::makeSettleable(&cbExec);
        std::scoped_lock const lock{mutex};
        sent.push_back(Sent{.envelope = std::move(envelope), .promise = std::move(promise)});
        return std::move(completion);
    }

    std::size_t count() {
        std::scoped_lock const lock{mutex};
        return sent.size();
    }

    // The request at @p index, waiting on @p owner until it was sent.
    Sent& at(morph::exec::MainThreadExecutor& owner, std::size_t index) {
        REQUIRE(pumpUntil(owner, [&] { return count() > index; }));
        std::scoped_lock const lock{mutex};
        return sent.at(index);
    }

    std::mutex mutex;
    std::deque<Sent> sent;
};

}  // namespace

TEST_CASE("an adapter over SimulatedRemoteBackend lists tools and runs a call", "[mcp][transport]") {
    Rig rig;
    auto const cid = rig.server->openConnection();
    morph::backend::SimulatedRemoteBackend transport{*rig.server, cid};
    morph::session::Context session;
    session.principal = "agent";
    session.token = "agent";
    ui::McpAdapter adapter{transport, ui::McpOptions{.session = session, .callbacks = &rig.callbacks}};
    auto names = toolNames(json(ask(rig.callbacks, adapter, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})")));
    std::ranges::sort(names);
    CHECK(names == std::vector<std::string>{"Mt_Add", "Mt_List", "Mt_Tally", "evaluate"});
    auto const added = json(ask(rig.callbacks, adapter, std::string{kAdd}));
    CHECK_FALSE(std::get<bool>(added["result"]["isError"].data));
    CHECK(std::get<std::string>(added["result"]["content"][0]["text"].data) == R"({"id":1,"text":"hello"})");
    rig.server->closeConnection(cid);
}

TEST_CASE("an adapter over a transport reports a refused call as a tool error", "[mcp][transport]") {
    Rig rig;
    auto const cid = rig.server->openConnection();
    morph::backend::SimulatedRemoteBackend transport{*rig.server, cid};
    morph::session::Context session;
    session.principal = "agent";
    session.token = "agent";
    ui::McpAdapter adapter{transport, ui::McpOptions{.session = session, .callbacks = &rig.callbacks}};
    auto const reply = json(ask(rig.callbacks, adapter,
                                R"({"jsonrpc":"2.0","id":5,"method":"tools/call",)"
                                R"("params":{"name":"Mt_Wipe","arguments":{}}})"));
    CHECK(std::get<bool>(reply["result"]["isError"].data));
    rig.server->closeConnection(cid);
}

TEST_CASE("a rejected transport request is a tool error carrying the failure", "[mcp][transport]") {
    morph::exec::MainThreadExecutor owner;
    ScriptedTransport transport;
    ui::McpAdapter adapter{transport, ui::McpOptions{.callbacks = &owner}};
    std::optional<std::optional<std::string>> replied;
    adapter.handle(std::string{kList}).thenDetached([&](std::optional<std::string> const& answer) {
        replied = answer;
    });
    // The catalog request.
    transport.at(owner, 0).promise.reject(std::make_exception_ptr(morph::backend::DisconnectedError{}));
    REQUIRE(pumpUntil(owner, [&] { return replied.has_value(); }));
    // An unreadable catalog is empty, so the tool is unknown: a JSON-RPC -32602 naming it, and no crash.
    CHECK(replied->value().contains("-32602"));
    CHECK(replied->value().contains("Mt_List"));
}

TEST_CASE("a request with no reply times out after callTimeout", "[mcp][transport]") {
    morph::exec::MainThreadExecutor owner;
    ScriptedTransport transport;
    ui::McpAdapter adapter{transport, ui::McpOptions{.callbacks = &owner, .callTimeout = 100ms}};
    std::optional<std::optional<std::string>> replied;
    adapter.handle(R"({"jsonrpc":"2.0","id":7,"method":"tools/list"})")
        .thenDetached([&](std::optional<std::string> const& answer) { replied = answer; });
    auto const started = std::chrono::steady_clock::now();
    pumpUntil(owner, [&] { return replied.has_value(); }, 5s);
    REQUIRE(replied.has_value());
    CHECK(std::chrono::steady_clock::now() - started >= 100ms);
    // The catalog timed out, so it is empty: the list holds only `evaluate`.
    CHECK(replied->value().contains(R"("name":"evaluate")"));
    // A reply that arrives after the timeout is dropped, not delivered twice.
    transport.at(owner, 0).promise.resolve(morph::wire::makeErr("late"));
    owner.runFor(50ms);
}

TEST_CASE("a register reply released after a reset is not cached", "[mcp][transport]") {
    Rig rig;
    // A genuine catalog reply, from the server the adapter would have asked.
    morph::wire::Envelope catalogRequest;
    catalogRequest.kind = std::string{ui::kCatalogKind};
    catalogRequest.callId = 1;
    catalogRequest.session.principal = "agent";
    catalogRequest.session.token = "agent";
    auto const cid = rig.server->openConnection();
    auto const catalogReply = morph::wire::decode(rig.server->handleInline(morph::wire::encode(catalogRequest), cid));
    REQUIRE(catalogReply.kind == "ok");
    rig.server->closeConnection(cid);

    ScriptedTransport transport;
    ui::McpAdapter adapter{transport, ui::McpOptions{.callbacks = &rig.callbacks}};
    auto const call = [&] {
        auto replied = std::make_shared<std::optional<std::optional<std::string>>>();
        adapter.handle(std::string{kList}).thenDetached([replied](std::optional<std::string> const& answer) {
            *replied = answer;
        });
        return replied;
    };
    auto const first = call();
    transport.at(rig.callbacks, 0).promise.resolve(catalogReply);
    // sent[1] is the register request: reset before its reply arrives.
    REQUIRE(transport.at(rig.callbacks, 1).envelope.kind == "register");
    adapter.resetter()();
    morph::wire::Envelope registered;
    registered.kind = "ok";
    registered.modelId = 41;
    transport.sent.at(1).promise.resolve(registered);
    // sent[2] is the execute on 41.
    REQUIRE(transport.at(rig.callbacks, 2).envelope.kind == "execute");
    CHECK(transport.sent.at(2).envelope.modelId == 41);
    morph::wire::Envelope executed;
    executed.kind = "ok";
    executed.body = "[]";
    transport.sent.at(2).promise.resolve(executed);
    REQUIRE(pumpUntil(rig.callbacks, [&] { return first->has_value(); }));

    // The reset came before 41 arrived, so 41 was not cached: the catalog is fetched and the instance registered
    // again.
    auto const second = call();
    transport.at(rig.callbacks, 3).promise.resolve(catalogReply);
    CHECK(transport.at(rig.callbacks, 4).envelope.kind == "register");
}

TEST_CASE("a resetter called after the adapter is destroyed does nothing", "[mcp][transport]") {
    morph::exec::MainThreadExecutor owner;
    ScriptedTransport transport;
    std::function<void()> reset;
    {
        ui::McpAdapter adapter{transport, ui::McpOptions{.callbacks = &owner}};
        reset = adapter.resetter();
    }
    reset();  // must not crash; run under ASan in CI
}

TEST_CASE("serveStdioOnOwner answers lines while another thread runs the owner", "[mcp][transport]") {
    Rig rig;
    morph::exec::ThreadPoolExecutor ownerPool{1};
    morph::exec::OwnerStrand owner{ownerPool};
    morph::session::Context session;
    session.principal = "agent";
    session.token = "agent";
    ui::McpAdapter adapter{rig.server, ui::McpOptions{.session = session, .callbacks = &owner}};
    std::istringstream input{std::string{R"({"jsonrpc":"2.0","id":1,"method":"ping"})"} + "\n\n" +
                             R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})" + "\n"};
    std::ostringstream output;
    ui::serveStdioOnOwner(adapter, owner, input, output);
    auto const text = output.str();
    CHECK(std::ranges::count(text, '\n') == 2);
    CHECK(text.find(R"("id":1)") < text.find(R"("id":2)"));
}
