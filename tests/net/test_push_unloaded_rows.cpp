// SPDX-License-Identifier: Apache-2.0

// Slow levels over the `net` transport (docs/spec/table/table.md, "Arrival", "The server level cache", "The view
// cache"): one server serving `bench::WideTableModel` with its recipe level cache attached, and clients sending raw
// envelopes. A reply never waits for a load; a level is loaded once for every client and only for what a query
// needs; an action's effects invalidate the cache; `announce` reaches the watchers of the record and its derived
// records; and no value crosses a row scope.
//
// The end of each case names the mutation it is meant to catch. These cases need a POSIX build, and none of
// those mutations has been run yet.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
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
#include <morph/session/session.hpp>
#include <morph/table/description.hpp>
#include <morph/table/level_cache.hpp>
#include <morph/ui/author.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/service_client.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/value.hpp>
#include <mutex>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "table/wide_table_model.hpp"

// NOLINTBEGIN(misc-use-internal-linkage): registered types must have external linkage.
namespace unloaded_rows {

// A record derived from `WideTableModel.small`, and an action holding it, so a client may watch it.
struct UrSummary {
    std::int64_t rows = 0;
};
struct ReadSummary {};

// A row-scoped table: `p1` sees rows 0-499, `p2` rows 250-749, of one shared 1,000-row source with a recipe
// column `r` read through one level cache for both scopes. `UrEmptyScope` is the same table with `scope` left
// empty.
class UrModel {
public:
    morph::table::ServedTable rows(morph::table::TableContext const& context);
    morph::table::ServedTable emptyScope(morph::table::TableContext const& context);
    UrSummary execute(ReadSummary const& action);
};

}  // namespace unloaded_rows
// NOLINTEND(misc-use-internal-linkage)

BRIDGE_REGISTER_RECORD(unloaded_rows::UrSummary, "UrSummary", morph::record::keyless,
                       morph::record::derivedFrom<morph::table::TableOf<&bench::WideTableModel::small>>)
BRIDGE_REGISTER_MODEL(unloaded_rows::UrModel, "UrModel")
BRIDGE_REGISTER_TABLE(unloaded_rows::UrModel, "urRows", &unloaded_rows::UrModel::rows, morph::table::rowScoped)
BRIDGE_REGISTER_TABLE(unloaded_rows::UrModel, "urEmptyScope", &unloaded_rows::UrModel::emptyScope,
                      morph::table::rowScoped)
BRIDGE_REGISTER_ACTION(unloaded_rows::UrModel, unloaded_rows::ReadSummary, "ReadSummary")

namespace unloaded_rows {

namespace {

using morph::table::Cell;
using morph::table::RowId;

// The shared rows, the scopes' sources and the recipe cache, with the loader's log.
struct UrState {
    std::mutex mutex;
    std::shared_ptr<morph::table::VectorSource> source;
    std::map<std::string, std::shared_ptr<morph::table::DataSource const>, std::less<>> scopes;
    morph::exec::ThreadPoolExecutor loads{1};
    std::vector<RowId> logged;
    std::shared_ptr<morph::table::CellLevelCache> cache;

    UrState() {
        source = std::make_shared<morph::table::VectorSource>(std::vector<morph::table::ColumnInfo>{
            {.id = "id", .kind = morph::table::ColumnKind::Integer, .comparator = {}, .format = {}, .level = {}}});
        std::vector<RowId> keys;
        std::vector<std::vector<Cell>> cells;
        for (std::int64_t row = 0; row < 1'000; ++row) {
            keys.emplace_back(row);
            cells.push_back({Cell{row}});
        }
        source->setRows(std::move(keys), std::move(cells));
        morph::table::LevelCacheOptions options;
        options.records = {"UrModel.urRows", "UrModel.urEmptyScope"};
        options.level = "recipe";
        options.columns = {
            {.id = "r", .kind = morph::table::ColumnKind::Text, .comparator = {}, .format = {}, .level = "recipe"}};
        options.loads = &loads;
        cache =
            std::make_shared<morph::table::CellLevelCache>(std::move(options), [this](std::span<RowId const> asked) {
                std::vector<std::pair<RowId, std::vector<Cell>>> out;
                std::scoped_lock const lock{mutex};
                for (auto const& key : asked) {
                    logged.push_back(key);
                    out.emplace_back(key, std::vector<Cell>{Cell{"r" + std::to_string(std::get<std::int64_t>(key))}});
                }
                return out;
            });
    }
};

UrState& urState() {
    static UrState state;
    return state;
}

morph::table::ServedTable urTable(std::string_view principal, std::string scope) {
    auto& state = urState();
    morph::table::ServedTable served;
    served.description.id = "urRows";
    served.description.label = "rows";
    served.description.key = "id";
    served.description.levels = {"recipe"};
    served.description.columns = {
        morph::table::TableColumn{.info = state.source->columns()[0], .label = "id", .group = {}},
        morph::table::TableColumn{.info = state.cache->columns()[0], .label = "r", .group = {}}};
    std::int64_t const first = principal == "p2" ? 250 : 0;
    {
        std::scoped_lock const lock{state.mutex};
        auto& scoped = state.scopes[std::string{principal}];
        if (scoped == nullptr) {
            std::vector<RowId> keys;
            for (auto key = first; key < first + 500; ++key) {
                keys.emplace_back(key);
            }
            scoped = morph::table::scoped(state.source, std::move(keys));
        }
        served.source = scoped;
    }
    served.levels = {state.cache};
    served.scope = std::move(scope);
    return served;
}

}  // namespace

morph::table::ServedTable UrModel::rows(morph::table::TableContext const& context) {
    return urTable(context.session.principal, context.session.principal);
}

morph::table::ServedTable UrModel::emptyScope(morph::table::TableContext const& context) {
    auto served = urTable(context.session.principal, {});
    served.description.id = "urEmptyScope";
    return served;
}

UrSummary UrModel::execute(ReadSummary const& /*action*/) { return UrSummary{.rows = 1'000}; }

}  // namespace unloaded_rows

namespace {

using namespace std::chrono_literals;
namespace ui = morph::ui;
using morph::table::RowId;

constexpr std::string_view kHello = "t-unloaded-hello";
constexpr auto kCoalesce = 20ms;
constexpr std::string_view kWide = bench::kWideModelType;

// Negotiates events for the connection its hello arrives on.
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

// Takes the principal a client names as verified.
class NamedAuthorizer final : public morph::session::IAuthorizer {
public:
    [[nodiscard]] bool authorize(morph::session::Context const& /*ctx*/, std::string_view /*modelType*/,
                                 std::string_view /*actionType*/) const override {
        return true;
    }

    [[nodiscard]] std::optional<std::string> authenticate(morph::session::Context const& ctx) const override {
        return ctx.principal;
    }
};

// One server on a socket over `WideTableModel` configured as given, its recipe cache attached.
struct Server {
    explicit Server(bench::WideTableOptions small, bench::WideTableOptions wide = {.rows = 2'000,
                                                                                   .columns = 300,
                                                                                   .seed = 7,
                                                                                   .loadDelay = {},
                                                                                   .recipePreloaded = 1.0,
                                                                                   .recipeBatch = 64,
                                                                                   .recipeFails = false}) {
        bench::WideTableModel::recipeGate().release();
        bench::WideTableModel::configure(wide, small);
        morph::backend::ServerConfig config;
        config.services.push_back(std::make_shared<Hello>());
        config.coalesceWindow = kCoalesce;
        config.tableWork = tableWork;
        server = std::make_shared<morph::backend::RemoteServer>(pool, std::make_shared<NamedAuthorizer>(),
                                                                std::move(config));
        server->attach(bench::WideTableModel::recipeCache());
        server->attach(unloaded_rows::urState().cache);
        socket = std::make_unique<morph::net::SocketServer>(*server, 0);
        REQUIRE(socket->listen());
        url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(socket->port()));
    }

    ~Server() { bench::WideTableModel::recipeGate().release(); }

    Server(Server const&) = delete;
    Server& operator=(Server const&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    morph::exec::ThreadPoolExecutor pool{4};
    std::shared_ptr<morph::exec::ThreadPoolExecutor> tableWork = std::make_shared<morph::exec::ThreadPoolExecutor>(2);
    std::shared_ptr<morph::backend::RemoteServer> server;
    std::unique_ptr<morph::net::SocketServer> socket;
    std::string url;
};

// One client as `principal`: raw envelopes over a socket, and the `changes` events it receives.
struct Client {
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): `Client(url, principal)` is a test helper whose arguments read differently at each call; a swap would fail the case's own assertion
    Client(std::string const& url, std::string principal) : _principal{std::move(principal)} {
        socket = std::make_unique<morph::net::SocketBackend>(url);
        REQUIRE(socket->waitForConnected());
        exchange = ui::exchangeOver(*socket, owner);
        socket->setEventHandler(
            [this](morph::wire::EventBody const& event) {
                if (event.topic != morph::wire::kChangesTopic) {
                    return;
                }
                auto body = morph::wire::readChangesBody(event.payload.str);
                REQUIRE(body);
                changes.push_back(std::move(*body));
            },
            &owner);
        morph::wire::Envelope hello;
        hello.kind = std::string{kHello};
        auto const sent = send(std::move(hello));
        REQUIRE(sent.kind == "ok");
    }

    morph::wire::Envelope send(morph::wire::Envelope request) {
        request.session.principal = _principal;
        std::optional<ui::ExchangeReply> reply;
        exchange(std::move(request), [&reply](ui::ExchangeReply answer) { reply = std::move(answer); });
        REQUIRE(pumpUntil([&] { return reply.has_value(); }));
        REQUIRE(reply->has_value());
        return **reply;
    }

    std::vector<std::string> watch(std::vector<std::string> records) {
        morph::wire::Envelope env;
        env.kind = std::string{morph::wire::kWatchKind};
        env.body =
            glz::write_json(morph::wire::WatchBody{.records = std::move(records), .instances = {}, .replace = false})
                .value_or(std::string{});
        auto const reply = send(std::move(env));
        REQUIRE(reply.kind == "ok");
        auto const parsed = morph::wire::readWatchReply(reply.body);
        REQUIRE(parsed);
        return parsed->refusedRecords;
    }

    std::uint64_t create(std::string const& modelType) {
        auto const reply = send(morph::wire::makeRegister(modelType));
        REQUIRE(reply.kind == "ok");
        return reply.modelId;
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): `execute(instance, modelType, action, body)` is a test helper whose arguments read differently at each call; a swap would fail the case's own assertion
    morph::wire::Envelope execute(std::uint64_t instance, std::string_view modelType, std::string const& action,
                                  std::string body) {
        morph::wire::Envelope env;
        env.kind = "execute";
        env.modelId = instance;
        env.modelType = std::string{modelType};
        env.actionType = action;
        env.body = std::move(body);
        return send(std::move(env));
    }

    template <typename Done>
    bool pumpUntil(Done done, std::chrono::milliseconds budget = 10s) {
        auto const deadline = std::chrono::steady_clock::now() + budget;
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

    void pumpFor(std::chrono::milliseconds span) {
        static_cast<void>(pumpUntil([] { return false; }, span));
    }

    // The keys of every change of @p record received so far, and whether one came without keys.
    std::vector<std::string> keysOf(std::string_view record, bool* whole = nullptr) const {
        std::vector<std::string> keys;
        for (auto const& body : changes) {
            for (auto const& change : body.records) {
                if (change.record != record) {
                    continue;
                }
                if (!change.keys) {
                    if (whole != nullptr) {
                        *whole = true;
                    }
                    continue;
                }
                for (auto const& key : *change.keys) {
                    keys.push_back(key.str);
                }
            }
        }
        return keys;
    }

    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): the client's executor, socket, exchange and received changes, which the cases drive and read directly
    morph::exec::MainThreadExecutor owner;
    std::unique_ptr<morph::net::SocketBackend> socket;
    ui::Exchange exchange;
    std::vector<morph::wire::ChangesBody> changes;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

private:
    std::string _principal;
};

morph::table::GenericPage pageOf(morph::wire::Envelope const& reply) {
    REQUIRE(reply.kind == "ok");
    morph::table::GenericPage page;
    REQUIRE_FALSE(glz::read_json(page, reply.body));
    return page;
}

bench::WideTableOptions smallOptions(double preloaded, bool fails = false) {
    return bench::WideTableOptions{.rows = 1'000,
                                   .columns = 30,
                                   .seed = 7,
                                   .loadDelay = {},
                                   .recipePreloaded = preloaded,
                                   .recipeBatch = 64,
                                   .recipeFails = fails};
}

// Every key the recipe loader was given, sorted.
std::vector<RowId> loggedKeys() {
    std::vector<RowId> out;
    for (auto const& call : bench::WideTableModel::loaderCalls()) {
        out.insert(out.end(), call.begin(), call.end());
    }
    std::ranges::sort(out);
    return out;
}

std::vector<RowId> keyRange(std::int64_t first, std::int64_t last) {
    std::vector<RowId> out;
    for (auto key = first; key < last; ++key) {
        out.emplace_back(key);
    }
    return out;
}

bool waitFor(std::function<bool()> const& done, std::chrono::milliseconds budget = 10s) {
    auto const deadline = std::chrono::steady_clock::now() + budget;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

constexpr std::string_view kRecipePage =
    R"({"sort":[{"column":"id","dir":"asc"}],"columns":["id","d000","t001"],"page":{"offset":0,"limit":200}})";

}  // namespace

TEST_CASE("announce: a derived record is included, many keys are whole, an unregistered record is refused",
          "[net][push][levels]") {
    Server const server{smallOptions(1.0)};
    Client client{server.url, "alice"};
    REQUIRE(client.watch({"WideTableModel.small", "UrSummary"}).empty());

    std::vector<RowId> const three{RowId{std::int64_t{1}}, RowId{std::int64_t{2}}, RowId{std::int64_t{3}}};
    REQUIRE(server.server->announce("WideTableModel.small", morph::table::changeKeys(three)));
    REQUIRE(client.pumpUntil([&] { return !client.keysOf("WideTableModel.small").empty(); }));
    CHECK(client.keysOf("WideTableModel.small") == std::vector<std::string>{"1", "2", "3"});
    bool summaryWhole = false;
    static_cast<void>(client.keysOf("UrSummary", &summaryWhole));
    CHECK(summaryWhole);

    client.changes.clear();
    REQUIRE(server.server->announce("WideTableModel.small", morph::table::changeKeys(keyRange(1, 4'098))));
    bool smallWhole = false;
    REQUIRE(client.pumpUntil([&] {
        static_cast<void>(client.keysOf("WideTableModel.small", &smallWhole));
        return smallWhole;
    }));
    CHECK(client.keysOf("WideTableModel.small").empty());

    client.changes.clear();
    CHECK_FALSE(server.server->announce("NoSuchRecord", std::vector<glz::raw_json>{}).has_value());
    client.pumpFor(200ms);
    CHECK(client.changes.empty());
    // Mutation: announce bypassing the derived-record walk (UrSummary is not told).
}

TEST_CASE("table over the bridge: the first page arrives before the level", "[net][push][levels]") {
    Server const server{smallOptions(0.0)};
    bench::WideTableModel::recipeGate().hold();
    Client client{server.url, "alice"};
    auto const instance = client.create(std::string{kWide});
    auto const page = pageOf(client.execute(instance, kWide, "Table.small", std::string{kRecipePage}));
    REQUIRE(page.rows.size() == 200);
    REQUIRE(page.unloadedRows.size() == 200);
    for (std::size_t at = 0; at < 200; ++at) {
        CHECK(std::cmp_equal(page.unloadedRows[at].index, at));
        CHECK(page.unloadedRows[at].levels == std::vector<std::string>{"recipe"});
        CHECK(page.keys[at] == RowId{static_cast<std::int64_t>(at + 1)});
        CHECK(std::holds_alternative<std::monostate>(page.rows[at][1]));
        CHECK(std::holds_alternative<std::monostate>(page.rows[at][2]));
    }
    // Mutation: the answer path waiting for its loader (the reply never comes while the gate is held).
}

TEST_CASE("table over the bridge: a level loads only for what a query needs", "[net][push][levels]") {
    {
        Server const server{smallOptions(0.0)};
        Client client{server.url, "alice"};
        auto const instance = client.create(std::string{kWide});
        static_cast<void>(pageOf(client.execute(instance, kWide, "Table.small",
                                                R"({"columns":["id","i001"],"page":{"offset":0,"limit":200}})")));
        std::this_thread::sleep_for(200ms);
        CHECK(bench::WideTableModel::loaderCalls().empty());
    }
    {
        Server const server{smallOptions(0.0)};
        Client client{server.url, "alice"};
        auto const instance = client.create(std::string{kWide});
        static_cast<void>(pageOf(client.execute(instance, kWide, "Table.small", std::string{kRecipePage})));
        REQUIRE(waitFor([] { return bench::WideTableModel::recipeCache()->stats().loadedKeys >= 200; }));
        CHECK(loggedKeys() == keyRange(1, 201));
    }
    {
        Server const server{smallOptions(0.0)};
        Client client{server.url, "alice"};
        auto const instance = client.create(std::string{kWide});
        static_cast<void>(pageOf(client.execute(
            instance, kWide, "Table.small",
            R"({"sort":[{"column":"d000","dir":"asc"}],"columns":["id","i001"],"page":{"offset":0,"limit":200}})")));
        REQUIRE(waitFor([] { return bench::WideTableModel::recipeCache()->stats().loadedKeys >= 1'000; }));
        CHECK(loggedKeys() == keyRange(1, 1'001));
    }
    // Mutation: levelsNeeded returning every level (the first query loads its page's keys).
}

TEST_CASE("table over the bridge: two clients load a value once", "[net][push][levels]") {
    Server const server{smallOptions(0.0)};
    bench::WideTableModel::recipeGate().hold();
    Client first{server.url, "alice"};
    Client second{server.url, "carol"};
    auto const one = first.create(std::string{kWide});
    auto const two = second.create(std::string{kWide});
    static_cast<void>(pageOf(first.execute(one, kWide, "Table.small", std::string{kRecipePage})));
    static_cast<void>(pageOf(second.execute(two, kWide, "Table.small", std::string{kRecipePage})));
    bench::WideTableModel::recipeGate().release();
    REQUIRE(waitFor([] { return bench::WideTableModel::recipeCache()->stats().loadedKeys >= 200; }));
    std::this_thread::sleep_for(100ms);
    CHECK(bench::WideTableModel::recipeCache()->stats().loadedKeys == 200);
    CHECK(loggedKeys() == keyRange(1, 201));
    // Mutation: want not checking the cache (each key is loaded again by the second client's want).
}

TEST_CASE("table over the bridge: an updates effect invalidates without a call", "[net][push][levels]") {
    Server const server{smallOptions(1.0)};
    Client client{server.url, "alice"};
    REQUIRE(client.watch({"WideTableModel.small"}).empty());
    auto const instance = client.create(std::string{kWide});
    auto const before = pageOf(client.execute(instance, kWide, "Table.small", std::string{kRecipePage}));
    CHECK(before.unloadedRows.empty());
    REQUIRE(client.execute(instance, kWide, "EditWideRow", R"({"row":4,"column":"i001","value":1})").kind == "ok");
    // The observer runs before the change's event is queued, so the event says the invalidation happened.
    REQUIRE(client.pumpUntil([&] { return !client.keysOf("WideTableModel.small").empty(); }));
    bench::WideTableModel::recipeGate().hold();
    auto const after = pageOf(client.execute(instance, kWide, "Table.small", std::string{kRecipePage}));
    REQUIRE(after.unloadedRows.size() == 1);
    CHECK(after.unloadedRows[0].index == 4);  // the row keyed 5
    bench::WideTableModel::recipeGate().release();
    REQUIRE(waitFor([] { return bench::WideTableModel::recipeCache()->stats().loadedKeys >= 1; }));
    CHECK(loggedKeys() == std::vector<RowId>{RowId{std::int64_t{5}}});
    // Mutation: fanOut not handing the change to observers (the row keyed 5 stays loaded and nothing is loaded).
}

TEST_CASE("table over the bridge: no value crosses a scope", "[net][push][levels]") {
    Server const server{smallOptions(1.0)};
    unloaded_rows::urState().cache->invalidateAll();
    {
        std::scoped_lock const lock{unloaded_rows::urState().mutex};
        unloaded_rows::urState().logged.clear();
    }
    Client p1{server.url, "p1"};
    Client p2{server.url, "p2"};
    auto const one = p1.create("UrModel");
    auto const two = p2.create("UrModel");
    std::string const query = R"({"columns":["id","r"],"page":{"offset":0,"limit":500}})";
    auto const first = pageOf(p1.execute(one, "UrModel", "Table.urRows", query));
    auto const second = pageOf(p2.execute(two, "UrModel", "Table.urRows", query));
    CHECK(first.keys == keyRange(0, 500));
    CHECK(second.keys == keyRange(250, 750));
    REQUIRE(waitFor([] { return unloaded_rows::urState().cache->stats().loadedKeys >= 750; }));
    std::vector<RowId> logged;
    {
        std::scoped_lock const lock{unloaded_rows::urState().mutex};
        logged = unloaded_rows::urState().logged;
    }
    for (std::int64_t key = 250; key < 500; ++key) {
        CHECK(std::ranges::count(logged, RowId{key}) == 1);
    }
    // Each scope's source has its own snapshot, so the snapshot identity alone keeps the two views apart; the
    // scope in the view key is held by "table over the bridge: two scopes over one snapshot get two views".
    // Mutation: the scoped source answering every shared row (each reply holds rows of the other scope).
}

TEST_CASE("table over the bridge: a row-scoped table announces without keys", "[net][push][levels]") {
    Server const server{smallOptions(1.0)};
    unloaded_rows::urState().cache->invalidateAll();
    Client p1{server.url, "p1"};
    REQUIRE(p1.watch({"UrModel.urRows"}).empty());
    auto const instance = p1.create("UrModel");
    static_cast<void>(pageOf(
        p1.execute(instance, "UrModel", "Table.urRows", R"({"columns":["id","r"],"page":{"offset":0,"limit":50}})")));
    bool whole = false;
    REQUIRE(p1.pumpUntil([&] {
        static_cast<void>(p1.keysOf("UrModel.urRows", &whole));
        return whole;
    }));
    CHECK(p1.keysOf("UrModel.urRows").empty());
    auto const refused = p1.execute(instance, "UrModel", "Table.urEmptyScope", R"({"page":{"offset":0,"limit":5}})");
    CHECK(refused.kind == "err");
    REQUIRE(refused.refusal.has_value());
    CHECK(refused.refusal->code == "table.sourceContract");
    // Mutation: the record registered without privateKeys (p1 is told the arrived rows' keys).
}

TEST_CASE("table over the bridge: failed keys are not loaded again inside the delay", "[net][push][levels]") {
    Server const server{smallOptions(0.0, true)};
    Client client{server.url, "alice"};
    REQUIRE(client.watch({"WideTableModel.small"}).empty());
    auto const instance = client.create(std::string{kWide});
    std::string const sorted =
        R"({"sort":[{"column":"d000","dir":"asc"}],"columns":["id","d000"],"page":{"offset":0,"limit":200}})";
    static_cast<void>(pageOf(client.execute(instance, kWide, "Table.small", sorted)));
    REQUIRE(waitFor([] { return bench::WideTableModel::recipeCache()->stats().failedLoads >= 1'000; }));
    auto const calls = bench::WideTableModel::loaderCalls().size();
    for (int request = 0; request < 10; ++request) {
        auto const page = pageOf(client.execute(instance, kWide, "Table.small", sorted));
        CHECK(page.awaiting.empty());
        CHECK(page.failed == std::vector<std::string>{"recipe"});
        CHECK(std::ranges::all_of(page.unloadedRows, [](morph::table::UnloadedRow const& row) {
            return row.failed == std::vector<std::string>{"recipe"};
        }));
    }
    std::this_thread::sleep_for(100ms);
    CHECK(bench::WideTableModel::loaderCalls().size() == calls);
    client.pumpFor(100ms);
    std::size_t announced = 0;
    for (auto const& key : client.keysOf("WideTableModel.small")) {
        static_cast<void>(key);
        ++announced;
    }
    CHECK(announced == 1'000);  // each failed key once
    static_cast<void>(pageOf(client.execute(
        instance, kWide, "Table.small",
        R"({"sort":[{"column":"d000","dir":"asc"}],"columns":["id","d000"],"page":{"offset":0,"limit":200},"retry":true})")));
    REQUIRE(waitFor([] { return bench::WideTableModel::recipeCache()->stats().failedLoads >= 2'000; }));
    CHECK(bench::WideTableModel::loaderCalls().size() == calls + 16);  // ceil(1000 / 64)
    // Mutation: a failed key wanted again by every request (the ten requests call the loader).
}

// ── A window and a query cache over the bridge ──────────────────────────────

namespace {

// Forwards every call, counting page requests (a body without `keys`) and keyed requests apart.
class CountingChannel final : public ui::ModelChannel {
public:
    explicit CountingChannel(std::shared_ptr<ui::ModelChannel> inner) : _inner{std::move(inner)} {}

    [[nodiscard]] morph::async::Completion<std::string> execute(std::string const& actionId,
                                                                std::string const& bodyJson,
                                                                ui::CallOptions options) override {
        if (!actionId.ends_with(".describe")) {
            if (bodyJson.contains(R"("keys")")) {
                ++keyedRequests;
            } else {
                ++pageRequests;
            }
        }
        return _inner->execute(actionId, bodyJson, std::move(options));
    }

    [[nodiscard]] ui::ChannelFacts facts() const override { return _inner->facts(); }

    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): the request counters the cases read after a run
    int pageRequests = 0;
    int keyedRequests = 0;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

private:
    std::shared_ptr<ui::ModelChannel> _inner;
};

// One client as a UI session over the bridge: its watches and events go through the session, its window's fetches
// through `Session::read`.
struct SessionClient {
    explicit SessionClient(std::string const& url, ui::Catalog catalog = {}) {
        auto made = std::make_unique<morph::net::SocketBackend>(url);
        socket = made.get();
        REQUIRE(socket->waitForConnected());
        exchange = ui::exchangeOver(*socket, owner);
        bridge = std::make_unique<morph::bridge::Bridge>(std::move(made), owner);
        dispatcher = std::make_unique<ui::BridgeDispatcher>(*bridge, &owner);
        ui::SessionOptions options;
        options.catalog = std::move(catalog);
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
        channel = std::make_shared<CountingChannel>(
            dispatcher->bind(std::string{kWide}, morph::bridge::BindSharing::Private, std::string{}));
    }

    template <typename Done>
    bool pumpUntil(Done done, std::chrono::milliseconds budget = 10s) {
        auto const deadline = std::chrono::steady_clock::now() + budget;
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

    void pumpFor(std::chrono::milliseconds span) {
        static_cast<void>(pumpUntil([] { return false; }, span));
    }

    // Reads an action's reply text through `Session::read`.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): `read(action, body)` is a test helper whose arguments read differently at each call; a swap would fail the case's own assertion
    std::string read(std::string const& action, std::string const& body) {
        auto json = ui::parseJson(body);
        REQUIRE(json);
        auto value = ui::valueFromJson(*json);
        REQUIRE(value);
        std::optional<std::string> text;
        bool failed = false;
        auto completion = session->read(channel, action, *value);
        completion.then([&text](std::string const& reply) { text = reply; })
            .onError([&failed](std::exception_ptr const&) { failed = true; });
        REQUIRE(pumpUntil([&] { return text.has_value() || failed; }));
        REQUIRE(text.has_value());
        return *text;
    }

    morph::exec::MainThreadExecutor owner;
    morph::reactive::Runtime runtime{owner};
    morph::net::SocketBackend* socket = nullptr;
    ui::Exchange exchange;
    std::unique_ptr<morph::bridge::Bridge> bridge;
    std::unique_ptr<ui::BridgeDispatcher> dispatcher;
    std::unique_ptr<ui::Session> session;
    std::shared_ptr<CountingChannel> channel;
};

bench::WideTableOptions gatedOptions() {
    return bench::WideTableOptions{.rows = 1'000,
                                   .columns = 30,
                                   .seed = 7,
                                   .loadDelay = {},
                                   .recipePreloaded = 0.0,
                                   .recipeBatch = 20,
                                   .recipeFails = false};
}

// A window's page fetch as the body of `Table.small`, sorted by key.
std::string pageBody(morph::table::PageFetch const& fetch) {
    morph::table::TableQuery query;
    query.sort = {morph::table::SortKey{.column = "id", .dir = morph::table::SortDirection::Ascending}};
    query.columns = fetch.columns;
    query.page = fetch.request;
    return glz::write_json(query).value_or(std::string{});
}

// A window's keyed fetch as the body of `Table.small`, under the same sort.
std::string keyedBody(morph::table::RowsFetch const& fetch) {
    morph::table::TableQuery query;
    query.sort = {morph::table::SortKey{.column = "id", .dir = morph::table::SortDirection::Ascending}};
    query.columns = fetch.columns;
    query.page = morph::table::PageRequest{.offset = 0, .limit = 200};
    query.keys = fetch.keys;
    return glz::write_json(query).value_or(std::string{});
}

}  // namespace

// Mutations: refresh asking for every held key (the plan holds 230 keys, not 21); refresh dropping the keys the
// window does not hold (the plan misses the loaded keys of rows past the held ones). Unbuilt on the host that wrote
// it, and neither mutation has been run.
TEST_CASE("table over the bridge: exactly the arrived rows update, without polling", "[net][push][levels]") {
    Server const server{gatedOptions()};
    bench::WideTableModel::recipeGate().hold();
    SessionClient client{server.url};

    // 1. Watch, then fetch.
    std::optional<ui::WatchHolder> watch;
    std::vector<std::vector<RowId>> events;
    {
        auto watching =
            client.session->watchRecord("WideTableModel.small", [&](morph::effects::RecordChange const& change) {
                std::vector<RowId> keys;
                for (auto const& key : change.keys.value_or(std::vector<glz::raw_json>{})) {
                    if (auto id = morph::table::rowIdOf(key)) {
                        keys.push_back(*id);
                    }
                }
                events.push_back(std::move(keys));
            });
        watching.then([&watch](ui::WatchHolder const& holder) { watch = holder; });
        REQUIRE(client.pumpUntil([&] { return watch.has_value(); }));
    }
    morph::table::TableDescription description;
    REQUIRE_FALSE(
        glz::read<glz::opts{.error_on_unknown_keys = false}>(description, client.read("Table.small.describe", "{}")));
    morph::table::PageWindow<morph::table::GenericRow> window{200, 8};
    std::vector<std::string> const columns{"id", "d000"};
    auto const fetches = window.visible(0, 30, columns);
    REQUIRE(fetches.size() == 2);  // pages 0 and 1: no total is known yet
    for (auto const& fetch : fetches) {
        auto page = morph::table::readGenericPage(client.read("Table.small", pageBody(fetch)), description);
        REQUIRE(page.has_value());
        REQUIRE(window.accept(fetch, std::move(*page)));
    }
    CHECK(client.channel->pageRequests == 2);
    CHECK(client.channel->keyedRequests == 0);
    for (std::int64_t row = 0; row < 400; ++row) {
        INFO("view row " << row);
        CHECK(std::ranges::equal(window.unloadedLevels(row).levels, std::vector<std::string>{"recipe"}));
    }

    // 2. Nothing is fetched because time passed.
    client.pumpFor(200ms);
    CHECK(client.channel->pageRequests == 2);
    CHECK(client.channel->keyedRequests == 0);

    // 3. One loader call of 20 keys; the cache stores, then announces them.
    bench::WideTableModel::recipeGate().release(1);
    REQUIRE(waitFor([] { return !bench::WideTableModel::loaderCalls().empty(); }));
    auto const loaded = bench::WideTableModel::loaderCalls().front();
    REQUIRE(loaded.size() == 20);
    REQUIRE(client.pumpUntil([&] { return !events.empty(); }));

    // 4. The plan asks for the arrived keys and the one held run's probe.
    std::vector<RowId> expected = loaded;
    if (std::ranges::find(expected, RowId{std::int64_t{1}}) == expected.end()) {
        expected.emplace_back(std::int64_t{1});
    }
    auto const plan = window.refresh(events.front());
    REQUIRE(plan.kind == morph::table::Refresh::Rows);
    CHECK(std::ranges::is_permutation(plan.rows.keys, expected));
    auto reply = morph::table::readGenericPage(client.read("Table.small", keyedBody(plan.rows)), description);
    REQUIRE(reply.has_value());
    CHECK(client.channel->pageRequests == 2);
    CHECK(client.channel->keyedRequests == 1);

    // 5. Exactly the arrived rows (and the probe's row) are patched.
    auto const replaced = window.patch(plan.rows, std::move(*reply));
    std::vector<std::int64_t> rows;
    for (auto const& key : expected) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `rows` is a fixture list built once; sizing it first would not change what the case checks
        rows.push_back(std::get<std::int64_t>(key) - 1);
    }
    std::ranges::sort(rows);
    CHECK(replaced == rows);
    for (auto const& key : loaded) {
        auto const row = std::get<std::int64_t>(key) - 1;
        INFO("view row " << row);
        CHECK(window.unloadedLevels(row).levels.empty());
        auto const* const cell = window.cell(row, "d000");
        REQUIRE(cell != nullptr);
        CHECK_FALSE(std::holds_alternative<morph::table::Unloaded>(*cell));
        CHECK_FALSE(std::holds_alternative<std::monostate>(*cell));
    }
    for (std::int64_t row = 0; row < 400; ++row) {
        if (std::ranges::find(loaded, RowId{row + 1}) == loaded.end()) {
            INFO("view row " << row);
            CHECK(std::ranges::equal(window.unloadedLevels(row).levels, std::vector<std::string>{"recipe"}));
        }
    }
    bench::WideTableModel::recipeGate().release();
}

namespace {

// Binds through a dispatcher and counts the calls of every channel it hands out.
class CountingDispatcher final : public ui::Dispatcher {
public:
    explicit CountingDispatcher(ui::Dispatcher& inner) : _inner{&inner} {}

    [[nodiscard]] std::shared_ptr<ui::ModelChannel> bind(std::string const& typeId, morph::bridge::BindSharing sharing,
                                                         std::string const& instanceKey) override {
        auto channel = std::make_shared<CountingChannel>(_inner->bind(typeId, sharing, instanceKey));
        channels.push_back(channel);
        return channel;
    }

    [[nodiscard]] int pageRequests() const {
        int total = 0;
        for (auto const& channel : channels) {
            total += channel->pageRequests;
        }
        return total;
    }

    // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes): `channels` collects the counting channels the case inspects
    std::vector<std::shared_ptr<CountingChannel>> channels;

private:
    ui::Dispatcher* _inner;
};

}  // namespace

// Mutation: the cache looking for a key member in an opaque entry (the arrival's keys match no row, and nothing
// refetches). Unbuilt on the host that wrote it, and the mutation has not been run.
TEST_CASE("table over the bridge: the same arrival through the query cache", "[net][push][levels]") {
    Server const server{gatedOptions()};
    bench::WideTableModel::recipeGate().hold();
    ui::author::CatalogFacts facts;
    facts.action<bench::WideTableModel, morph::table::QueryTable<&bench::WideTableModel::small>>();
    SessionClient client{server.url, facts.catalog()};
    CountingDispatcher counting{*client.dispatcher};
    ui::Interpreter interpreter{*client.session, counting, ui::InterpreterOptions{}};
    constexpr std::string_view kSmall = R"({
      "vocab": "ui/1", "id": "small",
      "models": { "wide": { "type": "WideTableModel" } },
      "queries": { "page": { "model": "wide", "action": "Table.small",
        "body": { "obj": { "sort": { "list": [ { "obj": { "column": "id", "dir": "asc" } } ] },
                           "columns": { "list": [ "id", "d000" ] },
                           "page": { "obj": { "offset": 0, "limit": 200 } } } } } },
      "view": { "kind": "text", "testId": "rows", "text": { "fn": "format", "args": [ "{0}", { "query": "page" } ] } } })";
    auto loaded = ui::loadDocumentText(kSmall, ui::LoadOptions{.catalog = facts.catalog()});
    if (!loaded) {
        FAIL(loaded.error().message());
    }
    ui::testing::RecordingBackend view;
    auto const mounted = interpreter.mount(std::make_shared<ui::Document const>(std::move(*loaded)), {}, view, {});
    REQUIRE(mounted.has_value());
    REQUIRE(client.pumpUntil([&] { return counting.pageRequests() == 1; }));
    client.pumpFor(200ms);
    REQUIRE(counting.pageRequests() == 1);
    CHECK(client.session->cacheSize() == 1);

    // The arrival marks the one entry, which fetches its page once more: one page request per marked entry.
    bench::WideTableModel::recipeGate().release(1);
    REQUIRE(waitFor([] { return !bench::WideTableModel::loaderCalls().empty(); }));
    CHECK(client.pumpUntil([&] { return counting.pageRequests() == 2; }));
    client.pumpFor(200ms);
    CHECK(counting.pageRequests() == 2);
    bench::WideTableModel::recipeGate().release();
}

// E1 and E2 over the bridge: the first page of the sorted 100,000 x 300 table with `recipe` unloaded, from request
// to accepted page; and from a loader's return to patched rows, for 20 rows. Unbuilt and not run on the host that
// wrote it.
TEST_CASE("table benchmark: E1 E2 a window over the bridge", "[.benchmark][net]") {
    Server const server{gatedOptions(), bench::WideTableOptions{.rows = 100'000,
                                                                .columns = 300,
                                                                .seed = 7,
                                                                .loadDelay = {},
                                                                .recipePreloaded = 0.0,
                                                                .recipeBatch = 20,
                                                                .recipeFails = false}};
    bench::WideTableModel::recipeGate().hold();
    SessionClient client{server.url};
    morph::table::TableDescription description;
    REQUIRE_FALSE(
        glz::read<glz::opts{.error_on_unknown_keys = false}>(description, client.read("Table.wide.describe", "{}")));
    std::optional<ui::WatchHolder> watch;
    std::vector<std::vector<RowId>> events;
    auto watching =
        client.session->watchRecord("WideTableModel.wide", [&](morph::effects::RecordChange const& change) {
            std::vector<RowId> keys;
            for (auto const& key : change.keys.value_or(std::vector<glz::raw_json>{})) {
                if (auto id = morph::table::rowIdOf(key)) {
                    keys.push_back(*id);
                }
            }
            events.push_back(std::move(keys));
        });
    watching.then([&watch](ui::WatchHolder const& holder) { watch = holder; });
    REQUIRE(client.pumpUntil([&] { return watch.has_value(); }));
    morph::table::PageWindow<morph::table::GenericRow> window{200, 8};
    std::vector<std::string> columns;
    for (auto const& column : description.columns) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `columns` is a fixture list built once; sizing it first would not change what the case checks
        columns.push_back(column.info.id);
    }
    auto const started = std::chrono::steady_clock::now();
    auto const fetches = window.visible(0, 30, columns);
    REQUIRE_FALSE(fetches.empty());
    auto page = morph::table::readGenericPage(client.read("Table.wide", pageBody(fetches.front())), description);
    REQUIRE(page.has_value());
    REQUIRE(window.accept(fetches.front(), std::move(*page)));
    auto const e1 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    std::println("| E1 | first page, request to accepted | {:.3f} ms |", e1);

    std::vector<double> samples;
    for (int round = 0; round < 20; ++round) {
        events.clear();
        auto const calls = bench::WideTableModel::loaderCalls().size();
        bench::WideTableModel::recipeGate().release(1);
        REQUIRE(waitFor([&] { return bench::WideTableModel::loaderCalls().size() > calls; }));
        auto const returned = std::chrono::steady_clock::now();
        REQUIRE(client.pumpUntil([&] { return !events.empty(); }));
        auto const plan = window.refresh(events.front());
        if (plan.kind != morph::table::Refresh::Rows) {
            continue;
        }
        auto reply = morph::table::readGenericPage(client.read("Table.wide", keyedBody(plan.rows)), description);
        REQUIRE(reply.has_value());
        static_cast<void>(window.patch(plan.rows, std::move(*reply)));
        samples.push_back(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - returned).count());
    }
    REQUIRE_FALSE(samples.empty());
    std::ranges::sort(samples);
    std::println("| E2 | loader return to patched rows, 20 rows, p50 / p99 | {:.3f} / {:.3f} ms |",
                 samples[samples.size() / 2], samples.back());
    bench::WideTableModel::recipeGate().release();
}
