// SPDX-License-Identifier: Apache-2.0

// Server push end to end (spec 8): one server, two clients over the `net` transport, each with a UI session whose
// screen lists rows through a query and declares no refresh. Client A's mutation reaches client B's screen as an
// event, and B refetches, with no `refreshEvery`.

// clang-format off
#include <morph/ui/value.hpp>
// clang-format on

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/records.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/net/socket_backend.hpp>
#include <morph/net/socket_server.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/ui/author.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/service_client.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// NOLINTBEGIN(misc-use-internal-linkage): registered types must have external linkage.
namespace two_clients {

struct Row {
    std::int64_t id = 0;
    std::string name;
};
struct ListRows {};
struct EditRow {
    std::int64_t id = 0;
    std::string name;
};

// The rows every instance reads and writes: each client's connection has its own model instance.
struct Store {
    std::mutex mutex;
    std::vector<Row> rows;
};
inline Store& store() {
    static Store shared;
    return shared;
}

struct RowsModel {
    std::vector<Row> execute(ListRows const& /*action*/) {
        std::scoped_lock const lock{store().mutex};
        return store().rows;
    }
    Row execute(EditRow const& action) {
        std::scoped_lock const lock{store().mutex};
        for (Row& row : store().rows) {
            if (row.id == action.id) {
                row.name = action.name;
                return row;
            }
        }
        return Row{};
    }
};

}  // namespace two_clients
// NOLINTEND(misc-use-internal-linkage)

BRIDGE_REGISTER_MODEL(two_clients::RowsModel, "Tc_RowsModel")
BRIDGE_REGISTER_ACTION(two_clients::RowsModel, two_clients::ListRows, "Tc_ListRows")
BRIDGE_REGISTER_ACTION(two_clients::RowsModel, two_clients::EditRow, "Tc_EditRow")
BRIDGE_REGISTER_RECORD(two_clients::Row, "Tc_Row")
BRIDGE_REGISTER_EFFECTS(two_clients::EditRow, morph::effects::changes<two_clients::Row>)

namespace {

using namespace std::chrono_literals;
namespace ui = morph::ui;

constexpr std::string_view kHello = "t-two-hello";

// Negotiates events for the connection its hello arrives on, as the UI service's `ui-hello` does.
class Hello final : public morph::backend::IEnvelopeService {
public:
    using IEnvelopeService::handle;

    [[nodiscard]] bool serves(std::string_view kind) const override { return kind == kHello; }

    [[nodiscard]] std::string handle(morph::wire::Envelope const& request,
                                     morph::session::IAuthorizer const& /*authorizer*/) override {
        return morph::wire::encode(morph::wire::makeOk(request.callId));
    }

    [[nodiscard]] std::string handle(morph::wire::Envelope const& request,
                                     morph::session::IAuthorizer const& authorizer,
                                     morph::backend::ConnectionFacts& facts) override {
        facts.events = true;
        return handle(request, authorizer);
    }
};

// The screen: the rows, by a query, as `id:name`; an input that renames row 1; no refresh declarations.
constexpr std::string_view kRows = R"({
  "vocab": "ui/1", "id": "rows",
  "models": { "table": { "type": "Tc_RowsModel" } },
  "queries": { "rows": { "model": "table", "action": "Tc_ListRows" } },
  "mutations": { "rename": { "model": "table", "action": "Tc_EditRow" } },
  "view": { "kind": "column", "children": [
    { "kind": "textInput", "testId": "rename", "onSubmit": { "run": "rename", "body": { "obj": { "id": 1, "name": { "ref": "event" } } } } },
    { "kind": "forEach", "rows": { "query": "rows" }, "key": { "row": "id" },
      "children": [ { "kind": "text", "text": { "fn": "format", "args": [ "{0}:{1}", { "row": "id" }, { "row": "name" } ] } } ] } ] } })";

ui::Catalog catalog() {
    ui::author::CatalogFacts facts;
    facts.action<two_clients::RowsModel, two_clients::ListRows>();
    facts.action<two_clients::RowsModel, two_clients::EditRow>();
    return facts.catalog();
}

// One client: a socket backend under a bridge, a UI session wired to the server's events and watches, and the
// screen mounted on a recording view.
struct Client {
    explicit Client(std::string const& url) {
        auto made = std::make_unique<morph::net::SocketBackend>(url);
        socket = made.get();
        REQUIRE(socket->waitForConnected());
        exchange = ui::exchangeOver(*socket, owner);
        bridge = std::make_unique<morph::bridge::Bridge>(std::move(made), owner);
        dispatcher = std::make_unique<ui::BridgeDispatcher>(*bridge, &owner);
        ui::SessionOptions options;
        options.catalog = catalog();
        options.watcher = ui::watcherOver(exchange, [] { return morph::session::Context{}; }, &owner);
        session = std::make_unique<ui::Session>(runtime, std::move(options));
        socket->setEventHandler([this](morph::wire::EventBody const& event) { session->deliverEvent(event); }, &owner);
        bool negotiated = false;
        morph::wire::Envelope hello;
        hello.kind = std::string{kHello};
        exchange(std::move(hello), [&negotiated](ui::ExchangeReply const& reply) {
            negotiated = reply.has_value() && reply->kind == "ok";
        });
        REQUIRE(pumpUntil([&] { return negotiated; }));
        interpreter = std::make_unique<ui::Interpreter>(*session, *dispatcher, ui::InterpreterOptions{});
        auto loaded = ui::loadDocumentText(kRows, ui::LoadOptions{.catalog = catalog()});
        if (!loaded) {
            FAIL(loaded.error().message());
        }
        REQUIRE(loaded->warnings.empty());
        auto mounted = interpreter->mount(std::make_shared<ui::Document const>(std::move(*loaded)), {}, view, {});
        REQUIRE(mounted.has_value());
        screen = std::move(*mounted);
    }

    // Runs the owner's tasks until @p done holds, or 5 s pass.
    template <typename Done>
    bool pumpUntil(Done done) {
        auto const deadline = std::chrono::steady_clock::now() + 5s;
        while (!done()) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            if (!owner.runOnce()) {
                std::this_thread::sleep_for(1ms);
            }
        }
        return true;
    }

    [[nodiscard]] std::vector<std::string> rows() const {
        std::vector<std::string> texts;
        for (int const id : view.all("Text")) {
            texts.push_back(view.prop(id, "text"));
        }
        return texts;
    }

    morph::exec::MainThreadExecutor owner;
    morph::reactive::Runtime runtime{owner};
    morph::net::SocketBackend* socket = nullptr;
    ui::Exchange exchange;
    std::unique_ptr<morph::bridge::Bridge> bridge;
    std::unique_ptr<ui::BridgeDispatcher> dispatcher;
    std::unique_ptr<ui::Session> session;
    std::unique_ptr<ui::Interpreter> interpreter;
    ui::testing::RecordingBackend view;
    std::unique_ptr<ui::Screen> screen;
};

}  // namespace

// Control (run by hand): with B's event handler dropping the events, A still updates from its reply's effects and
// B never sees the rename, so the last check fails.
TEST_CASE("one_clients_mutation_updates_another_clients_screen_over_net", "[push][e2e]") {
    {
        std::scoped_lock const lock{two_clients::store().mutex};
        two_clients::store().rows = {two_clients::Row{.id = 1, .name = "one"},
                                     two_clients::Row{.id = 2, .name = "two"}};
    }
    morph::exec::ThreadPoolExecutor pool{2};
    morph::backend::ServerConfig config;
    config.services.push_back(std::make_shared<Hello>());
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, std::move(config));
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());
    std::string const url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(wsServer.port()));

    Client a{url};
    Client b{url};
    std::vector<std::string> const before{"1:one", "2:two"};
    REQUIRE(a.pumpUntil([&] { return a.rows() == before; }));
    REQUIRE(b.pumpUntil([&] { return b.rows() == before; }));

    a.view.submit(*a.view.find("TextInput", "testId", "rename"), "renamed");
    std::vector<std::string> const after{"1:renamed", "2:two"};
    // A refetches from its own reply's effects; B from the server's event.
    REQUIRE(a.pumpUntil([&] { return a.rows() == after; }));
    CHECK(b.pumpUntil([&] { return b.rows() == after; }));
}
