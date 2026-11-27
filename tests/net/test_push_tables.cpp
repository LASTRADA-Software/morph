// SPDX-License-Identifier: Apache-2.0

// A model's tables over the `net` transport (docs/spec/table/table.md, "A model's tables"): one server, two clients,
// each table a record of its own. Admission to a table's record follows the actions holding it; an action's changed
// rows reach the watchers of their own table; a page's columns are the caller's; and the reply is built on the
// server's table executor.
//
// The end of each case names the mutation it is meant to catch. These cases need a POSIX build, and none of
// those mutations has been run yet.

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/forms/forms.hpp>
#include <morph/net/socket_backend.hpp>
#include <morph/net/socket_server.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/session/session.hpp>
#include <morph/table/description.hpp>
#include <morph/table/edits.hpp>
#include <morph/table/level_cache.hpp>
#include <morph/table/row_selection.hpp>
#include <morph/ui/author.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/preferences.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/service_client.hpp>
#include <morph/ui/table_rows.hpp>
#include <morph/ui/table_state_model.hpp>
#include <mutex>
#include <optional>
#include <print>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "table/wide_table_model.hpp"

// NOLINTBEGIN(misc-use-internal-linkage): registered types must have external linkage.
namespace push_tables {

// The threads a recorded read ran on, shared by the source and the test.
struct ReadThreads {
    std::mutex mutex;
    std::set<std::thread::id> threads;

    void note() {
        std::scoped_lock const lock{mutex};
        threads.insert(std::this_thread::get_id());
    }

    std::set<std::thread::id> taken() {
        std::scoped_lock const lock{mutex};
        return threads;
    }

    void clear() {
        std::scoped_lock const lock{mutex};
        threads.clear();
    }
};

inline ReadThreads& reads() {
    static ReadThreads shared;
    return shared;
}

// A snapshot that records the thread of every read of a cell.
class RecordingSnapshot final : public morph::table::RowSnapshot {
public:
    explicit RecordingSnapshot(std::shared_ptr<morph::table::RowSnapshot const> inner) : _inner{std::move(inner)} {}

    [[nodiscard]] std::size_t rowCount() const override { return _inner->rowCount(); }
    [[nodiscard]] morph::table::RowId rowId(std::size_t row) const override { return _inner->rowId(row); }
    [[nodiscard]] morph::table::Cell cell(std::size_t row, std::size_t column) const override {
        reads().note();
        return _inner->cell(row, column);
    }
    void readColumn(std::size_t column, morph::table::ColumnSink& sink, std::size_t first,
                    std::size_t last) const override {
        reads().note();
        _inner->readColumn(column, sink, first, last);
    }

private:
    std::shared_ptr<morph::table::RowSnapshot const> _inner;
};

// A source whose snapshots record their reads.
class RecordingSource final : public morph::table::DataSource {
public:
    explicit RecordingSource(std::shared_ptr<morph::table::DataSource const> inner) : _inner{std::move(inner)} {}

    [[nodiscard]] std::span<morph::table::ColumnInfo const> columns() const override { return _inner->columns(); }
    [[nodiscard]] std::shared_ptr<morph::table::RowSnapshot const> snapshot() const override {
        return std::make_shared<RecordingSnapshot const>(_inner->snapshot());
    }
    void subscribe(morph::table::ChangeListener& /*listener*/) override {}
    void unsubscribe(morph::table::ChangeListener& /*listener*/) override {}

private:
    std::shared_ptr<morph::table::DataSource const> _inner;
};

// The thread `PtModel::first` last ran on.
inline std::atomic<std::thread::id>& firstThread() {
    static std::atomic<std::thread::id> id{};
    return id;
}

struct EditFirst {
    std::int64_t key = 0;
};
struct EditSecond {
    std::int64_t key = 0;
};
struct EditBoth {
    std::int64_t first = 0;
    std::int64_t second = 0;
};

class PtModel;
struct EditBothResult;

class PtModel {
public:
    morph::table::ServedTable first(morph::table::TableContext const& context);
    morph::table::ServedTable second(morph::table::TableContext const& context);
    morph::table::ChangedRows<&PtModel::first> execute(EditFirst const& action);
    morph::table::ChangedRows<&PtModel::second> execute(EditSecond const& action);
    EditBothResult execute(EditBoth const& action);
};

struct EditBothResult {
    morph::table::ChangedRows<&PtModel::first> first;
    morph::table::ChangedRows<&PtModel::second> second;
};

}  // namespace push_tables
// NOLINTEND(misc-use-internal-linkage)

BRIDGE_REGISTER_MODEL(push_tables::PtModel, "PtModel")
BRIDGE_REGISTER_TABLE(push_tables::PtModel, "first", &push_tables::PtModel::first)
BRIDGE_REGISTER_TABLE(push_tables::PtModel, "second", &push_tables::PtModel::second)
BRIDGE_REGISTER_ACTION(push_tables::PtModel, push_tables::EditFirst, "EditFirst")
BRIDGE_REGISTER_ACTION(push_tables::PtModel, push_tables::EditSecond, "EditSecond")
BRIDGE_REGISTER_ACTION(push_tables::PtModel, push_tables::EditBoth, "EditBoth")
BRIDGE_REGISTER_EFFECTS(push_tables::EditFirst, morph::effects::updatesTable<&push_tables::PtModel::first>)
BRIDGE_REGISTER_EFFECTS(push_tables::EditSecond, morph::effects::updatesTable<&push_tables::PtModel::second>)
BRIDGE_REGISTER_EFFECTS(push_tables::EditBoth, morph::effects::updatesTable<&push_tables::PtModel::first>,
                        morph::effects::updatesTable<&push_tables::PtModel::second>)

namespace push_tables {

namespace {

// A one-column table of `rows` integer rows keyed 1..rows, read through a recording source.
morph::table::ServedTable smallTable(std::string id, std::int64_t rows) {
    morph::table::ServedTable served;
    served.description.id = std::move(id);
    served.description.label = served.description.id;
    served.description.key = "v";
    served.description.columns.push_back(morph::table::TableColumn{
        .info = {.id = "v", .kind = morph::table::ColumnKind::Integer, .comparator = {}, .format = {}, .level = {}},
        .label = "V",
        .group = {}});
    auto source = std::make_shared<morph::table::VectorSource>(morph::table::columnInfos(served.description));
    std::vector<morph::table::RowId> keys;
    std::vector<std::vector<morph::table::Cell>> cells;
    for (std::int64_t row = 1; row <= rows; ++row) {
        keys.emplace_back(row);
        cells.push_back({morph::table::Cell{row}});
    }
    source->setRows(std::move(keys), std::move(cells));
    served.source = std::make_shared<RecordingSource const>(std::move(source));
    return served;
}

}  // namespace

morph::table::ServedTable PtModel::first(morph::table::TableContext const& /*context*/) {
    firstThread() = std::this_thread::get_id();
    return smallTable("first", 8);
}

morph::table::ServedTable PtModel::second(morph::table::TableContext const& /*context*/) {
    return smallTable("second", 8);
}

morph::table::ChangedRows<&PtModel::first> PtModel::execute(EditFirst const& action) {
    return {.rows = {morph::table::ChangedRow{.key = morph::table::RowId{action.key}}}};
}

morph::table::ChangedRows<&PtModel::second> PtModel::execute(EditSecond const& action) {
    return {.rows = {morph::table::ChangedRow{.key = morph::table::RowId{action.key}}}};
}

EditBothResult PtModel::execute(EditBoth const& action) {
    return {.first = {.rows = {morph::table::ChangedRow{.key = morph::table::RowId{action.first}}}},
            .second = {.rows = {morph::table::ChangedRow{.key = morph::table::RowId{action.second}}}}};
}

}  // namespace push_tables

namespace {

using namespace std::chrono_literals;
namespace ui = morph::ui;

constexpr std::string_view kHello = "t-tables-hello";
constexpr auto kCoalesce = 20ms;

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

// Takes the principal a client names as verified, refuses `bob` every action holding `PtModel.second`, and refuses
// `viewer` the edit action of `editnet::Specimens`.
class TablesAuthorizer final : public morph::session::IAuthorizer {
public:
    [[nodiscard]] bool authorize(morph::session::Context const& ctx, std::string_view /*modelType*/,
                                 std::string_view actionType) const override {
        if (ctx.principal == "viewer") {
            return actionType != "Lims_EditSpecimenCount";
        }
        if (ctx.principal != "bob") {
            return true;
        }
        return actionType != "Table.second" && actionType != "EditSecond" && actionType != "EditBoth";
    }

    [[nodiscard]] std::optional<std::string> authenticate(morph::session::Context const& ctx) const override {
        return ctx.principal;
    }
};

// A pool that records the threads its tasks run on.
class RecordingPool final : public morph::exec::IExecutor {
public:
    explicit RecordingPool(std::size_t threads = 2) : _pool{threads} {}

    void post(std::function<void()> task) override {
        _pool.post([this, task = std::move(task)] {
            {
                std::scoped_lock const lock{_mutex};
                _threads.insert(std::this_thread::get_id());
            }
            task();
        });
    }

    [[nodiscard]] bool isSerial() const noexcept override { return false; }

    std::set<std::thread::id> threads() {
        std::scoped_lock const lock{_mutex};
        return _threads;
    }

private:
    std::mutex _mutex;
    std::set<std::thread::id> _threads;
    morph::exec::ThreadPoolExecutor _pool;
};

// One server on a socket, with the tables authorizer.
struct Server {
    explicit Server(std::shared_ptr<morph::exec::IExecutor> tableWork = nullptr) {
        morph::backend::ServerConfig config;
        config.services.push_back(std::make_shared<Hello>());
        config.coalesceWindow = kCoalesce;
        config.tableWork = std::move(tableWork);
        server = std::make_shared<morph::backend::RemoteServer>(pool, std::make_shared<TablesAuthorizer>(),
                                                                std::move(config));
        socket = std::make_unique<morph::net::SocketServer>(*server, 0);
        REQUIRE(socket->listen());
        url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(socket->port()));
    }

    morph::exec::ThreadPoolExecutor pool{2};
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

    // Sends @p request as this client's principal and waits for its reply.
    morph::wire::Envelope send(morph::wire::Envelope request) {
        request.session.principal = _principal;
        std::optional<ui::ExchangeReply> reply;
        exchange(std::move(request), [&reply](ui::ExchangeReply answer) { reply = std::move(answer); });
        REQUIRE(pumpUntil([&] { return reply.has_value(); }));
        REQUIRE(reply->has_value());
        return **reply;
    }

    // Watches @p records; returns the refused ones.
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

    // Registers a private PtModel or WideTableModel instance.
    std::uint64_t create(std::string const& modelType) {
        auto const reply = send(morph::wire::makeRegister(modelType));
        REQUIRE(reply.kind == "ok");
        return reply.modelId;
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): `execute(instance, modelType, action, body)` is a test helper whose arguments read differently at each call; a swap would fail the case's own assertion
    morph::wire::Envelope execute(std::uint64_t instance, std::string const& modelType, std::string const& action,
                                  std::string body) {
        morph::wire::Envelope env;
        env.kind = "execute";
        env.modelId = instance;
        env.modelType = modelType;
        env.actionType = action;
        env.body = std::move(body);
        return send(std::move(env));
    }

    template <typename Done>
    bool pumpUntil(Done done, std::chrono::milliseconds budget = 5s) {
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

    // Runs the owner's tasks for @p span, so an event that would arrive arrives.
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

}  // namespace

TEST_CASE("tables: a principal allowed one table's action is refused the other", "[net][tables]") {
    Server const server;
    Client bob{server.url, "bob"};
    Client alice{server.url, "alice"};
    CHECK(bob.watch({"PtModel.first"}).empty());
    CHECK(bob.watch({"PtModel.second"}) == std::vector<std::string>{"PtModel.second"});
    auto const instance = bob.create("PtModel");
    CHECK(bob.execute(instance, "PtModel", "Table.second", "{}").kind == "err");
    CHECK(bob.execute(instance, "PtModel", "Table.first", "{}").kind == "ok");
    CHECK(alice.watch({"PtModel.first", "PtModel.second"}).empty());
    // bob is refused Table.second, EditSecond and EditBoth, the only actions holding PtModel.second.
    // Mutation: one record for the model (bob is admitted to both or neither).
}

TEST_CASE("tables: updatesTable on one table marks only its watchers", "[net][tables]") {
    Server const server;
    Client c1{server.url, "alice"};
    Client c2{server.url, "alice"};
    REQUIRE(c1.watch({"PtModel.first"}).empty());
    REQUIRE(c2.watch({"PtModel.second"}).empty());
    auto const instance = c1.create("PtModel");

    REQUIRE(c1.execute(instance, "PtModel", "EditFirst", R"({"key":3})").kind == "ok");
    REQUIRE(c1.pumpUntil([&] { return !c1.keysOf("PtModel.first").empty(); }));
    CHECK(c1.keysOf("PtModel.first") == std::vector<std::string>{"3"});
    c2.pumpFor(3 * kCoalesce);
    CHECK(c2.changes.empty());

    REQUIRE(c1.execute(instance, "PtModel", "EditSecond", R"({"key":4})").kind == "ok");
    REQUIRE(c2.pumpUntil([&] { return !c2.keysOf("PtModel.second").empty(); }));
    CHECK(c2.keysOf("PtModel.second") == std::vector<std::string>{"4"});
    c1.pumpFor(3 * kCoalesce);
    CHECK(c1.keysOf("PtModel.second").empty());
    CHECK(c1.keysOf("PtModel.first") == std::vector<std::string>{"3"});
    // Mutation: ChangedRows<F>::heldRecord naming the model's first registered table whatever F is (the registries
    // refuse EditSecond's updatesTable, or c1 is told of key 4).
}

TEST_CASE("tables: an action changing two tables sends each table its keys", "[net][tables]") {
    Server const server;
    Client c1{server.url, "alice"};
    Client c2{server.url, "alice"};
    REQUIRE(c1.watch({"PtModel.first"}).empty());
    REQUIRE(c2.watch({"PtModel.second"}).empty());
    auto const instance = c1.create("PtModel");

    REQUIRE(c1.execute(instance, "PtModel", "EditBoth", R"({"first":1,"second":2})").kind == "ok");
    REQUIRE(c1.pumpUntil([&] { return !c1.changes.empty(); }));
    REQUIRE(c2.pumpUntil([&] { return !c2.changes.empty(); }));
    bool firstWhole = false;
    bool secondWhole = false;
    CHECK(c1.keysOf("PtModel.first", &firstWhole) == std::vector<std::string>{"1"});
    CHECK(c2.keysOf("PtModel.second", &secondWhole) == std::vector<std::string>{"2"});
    CHECK_FALSE(firstWhole);
    CHECK_FALSE(secondWhole);
    // Mutation: the walk reporting a holder's record at the holder's own path instead of at its `rows` member (no key
    // is found at "first" or "second", so each watcher gets a change without keys).
}

TEST_CASE("tables: a page's columns are the caller's", "[net][tables]") {
    bench::WideTableModel::configure({.rows = 2'000, .columns = 300, .seed = 7, .loadDelay = {}},
                                     {.rows = 1'000, .columns = 30, .seed = 7, .loadDelay = {}});
    Server const server;
    Client alice{server.url, "alice"};
    Client restricted{server.url, "restricted"};
    auto const aliceModel = alice.create(std::string{bench::kWideModelType});
    auto const restrictedModel = restricted.create(std::string{bench::kWideModelType});
    auto const describe = [](Client& client, std::uint64_t instance) {
        auto const reply = client.execute(instance, std::string{bench::kWideModelType}, "Table.scoped.describe", "{}");
        REQUIRE(reply.kind == "ok");
        morph::table::TableDescription description;
        REQUIRE_FALSE(glz::read_json(description, reply.body));
        return description;
    };
    auto const aliceDescription = describe(alice, aliceModel);
    auto const restrictedDescription = describe(restricted, restrictedModel);
    CHECK(aliceDescription.columns.size() == 24);
    CHECK(restrictedDescription.columns.size() == 17);
    CHECK(aliceDescription.revision != restrictedDescription.revision);

    std::string const query = R"({"page":{"offset":0,"limit":5}})";
    auto const alicePage =
        pageOf(alice.execute(aliceModel, std::string{bench::kWideModelType}, "Table.scoped", query));
    auto const restrictedPage =
        pageOf(restricted.execute(restrictedModel, std::string{bench::kWideModelType}, "Table.scoped", query));
    CHECK(alicePage.columns.size() == 24);
    CHECK(restrictedPage.columns.size() == 17);
    CHECK(alicePage.revision == aliceDescription.revision);
    CHECK(restrictedPage.revision == restrictedDescription.revision);
    // Spec row "Columns by session", over the socket.
    // Mutation: QueryTable::invoke building TableContext from an empty session instead of session::current() (both
    // principals get the same 24 columns).
}

TEST_CASE("tables: a table's reply is built on tableWork", "[net][tables]") {
    auto const tableWork = std::make_shared<RecordingPool>();
    Server const server{tableWork};
    Client alice{server.url, "alice"};
    auto const instance = alice.create("PtModel");
    push_tables::reads().clear();

    auto const page = pageOf(alice.execute(instance, "PtModel", "Table.first",
                                           R"({"sort":[{"column":"v","dir":"desc"}],"page":{"offset":0,"limit":4}})"));
    CHECK(page.keys.size() == 4);
    auto const readThreads = push_tables::reads().taken();
    auto const workThreads = tableWork->threads();
    REQUIRE_FALSE(readThreads.empty());
    CHECK(std::ranges::all_of(readThreads, [&](std::thread::id id) { return workThreads.contains(id); }));
    CHECK_FALSE(readThreads.contains(push_tables::firstThread().load()));
    // Mutation: RemoteServer not installing the TableRuntimeScope (answerQuery runs inline on the strand).
}

namespace {

// A server whose `tableWork` and view cache a case configures, `WideTableModel`'s recipe cache attached.
struct ViewServer {
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): `ViewServer(maxViewBytes)` is a test helper whose arguments read differently at each call; a swap would fail the case's own assertion
    explicit ViewServer(std::size_t maxViewBytes = 64U << 20U, std::size_t tableThreads = 2)
        : tableWork{std::make_shared<RecordingPool>(tableThreads)} {
        morph::backend::ServerConfig config;
        config.services.push_back(std::make_shared<Hello>());
        config.coalesceWindow = kCoalesce;
        config.tableWork = tableWork;
        config.views.maxBytes = maxViewBytes;
        config.views.onCompute = [this] {
            {
                std::scoped_lock const lock{mutex};
                computedOn.insert(std::this_thread::get_id());
            }
            if (gate) {
                gate();
            }
        };
        server = std::make_shared<morph::backend::RemoteServer>(pool, std::make_shared<TablesAuthorizer>(),
                                                                std::move(config));
        server->attach(bench::WideTableModel::recipeCache());
        socket = std::make_unique<morph::net::SocketServer>(*server, 0);
        REQUIRE(socket->listen());
        url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(socket->port()));
    }

    std::set<std::thread::id> computeThreads() {
        std::scoped_lock const lock{mutex};
        return computedOn;
    }

    morph::exec::ThreadPoolExecutor pool{4};
    std::shared_ptr<RecordingPool> tableWork;
    std::mutex mutex;
    std::set<std::thread::id> computedOn;
    // Called where a view's computation starts, when set; set before any request.
    std::function<void()> gate;
    std::shared_ptr<morph::backend::RemoteServer> server;
    std::unique_ptr<morph::net::SocketServer> socket;
    std::string url;
};

bool waitUntil(std::function<bool()> const& done, std::chrono::milliseconds budget = 10s) {
    auto const deadline = std::chrono::steady_clock::now() + budget;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

}  // namespace

TEST_CASE("table over the bridge: a level's layout is the cache's", "[net][tables][levels]") {
    bench::WideTableModel::recipeGate().hold();
    bench::WideTableModel::configure(
        {.rows = 2'000, .columns = 300, .seed = 7, .loadDelay = {}, .recipePreloaded = 0.0},
        {.rows = 1'000, .columns = 30, .seed = 7, .loadDelay = {}, .recipePreloaded = 0.0});
    ViewServer const server;
    Client extended{server.url, "extended"};
    Client alice{server.url, "alice"};
    auto const extendedModel = extended.create(std::string{bench::kWideModelType});
    auto const aliceModel = alice.create(std::string{bench::kWideModelType});
    std::string const extendedQuery =
        R"({"sort":[{"column":"id","dir":"asc"}],"columns":["id","d000","d001","d002","d003","d004","d005",)"
        R"("t001","t002","t003","t004","t005","t006"],"page":{"offset":0,"limit":200}})";
    std::string const aliceQuery =
        R"({"sort":[{"column":"id","dir":"asc"}],"columns":["id","t001","t002","t003","t004","t005","t006"],)"
        R"("page":{"offset":0,"limit":200}})";
    auto const ask = [&](Client& client, std::uint64_t instance, std::string const& query) {
        return pageOf(client.execute(instance, std::string{bench::kWideModelType}, "Table.scoped", query));
    };
    auto const firstExtended = ask(extended, extendedModel, extendedQuery);
    auto const firstAlice = ask(alice, aliceModel, aliceQuery);
    CHECK(firstExtended.unloadedRows.size() == 200);
    CHECK(firstAlice.unloadedRows.size() == 200);
    CHECK(std::holds_alternative<std::monostate>(firstAlice.rows.at(0).at(1)));
    // Both first replies are in, each wholly unloaded: the loads may run now.
    bench::WideTableModel::recipeGate().release();
    REQUIRE(waitUntil([] { return bench::WideTableModel::recipeCache()->stats().loadedKeys >= 200; }));
    auto const secondExtended = ask(extended, extendedModel, extendedQuery);
    auto const secondAlice = ask(alice, aliceModel, aliceQuery);
    CHECK(secondExtended.unloadedRows.empty());
    CHECK(secondAlice.unloadedRows.empty());
    // The generator's cells: the small source holds the same values in its own columns.
    bench::WideTableSource const truth{{.rows = 1'000, .columns = 30, .seed = 7, .loadDelay = {}}};
    auto const snapshot = truth.snapshot();
    auto const columnOf = [&](std::string_view id) {
        auto const& columns = truth.columns();
        return static_cast<std::size_t>(std::ranges::find(columns, id, &morph::table::ColumnInfo::id) -
                                        columns.begin());
    };
    for (std::size_t row = 0; row < 200; ++row) {
        for (std::size_t at = 1; at < secondAlice.columns.size(); ++at) {
            CHECK(secondAlice.rows[row][at] == snapshot->cell(row, columnOf(secondAlice.columns[at])));
        }
        for (std::size_t at = 1; at < secondExtended.columns.size(); ++at) {
            CHECK(secondExtended.rows[row][at] == snapshot->cell(row, columnOf(secondExtended.columns[at])));
        }
    }
    std::map<morph::table::RowId, int> counts;
    for (auto const& call : bench::WideTableModel::loaderCalls()) {
        for (auto const& key : call) {
            ++counts[key];
        }
    }
    CHECK(counts.size() == 200);
    CHECK(std::ranges::all_of(counts, [](auto const& entry) { return entry.second == 1; }));
    // Mutation: reading the value by the caller's column order (alice's t001 then holds d000's value).
}

TEST_CASE("table over the bridge: one sort for many watchers", "[net][tables][views]") {
    bench::WideTableModel::recipeGate().release();
    bench::WideTableModel::configure(
        {.rows = 2'000, .columns = 300, .seed = 7, .loadDelay = {}, .recipePreloaded = 1.0},
        {.rows = 1'000, .columns = 30, .seed = 7, .loadDelay = {}, .recipePreloaded = 1.0});
    // Enough table threads for every refetch to be in the cache at once: one computes, forty-nine wait.
    ViewServer server{64U << 20U, 64};
    std::vector<std::unique_ptr<Client>> clients;
    std::vector<std::uint64_t> instances;
    std::string const query = R"({"sort":[{"column":"d000","dir":"asc"}],"columns":["id","d000"],)"
                              R"("page":{"offset":0,"limit":50}})";
    for (int i = 0; i < 50; ++i) {
        clients.push_back(std::make_unique<Client>(server.url, "alice"));
        REQUIRE(clients.back()->watch({"WideTableModel.wide"}).empty());
        instances.push_back(clients.back()->create(std::string{bench::kWideModelType}));
        static_cast<void>(pageOf(
            clients.back()->execute(instances.back(), std::string{bench::kWideModelType}, "Table.wide", query)));
    }
    auto const before = server.server->viewStats().computations;
    CHECK(before == 1);
    // An arrival on the order level: one value stored again gives the level a new generation.
    auto const cache = bench::WideTableModel::recipeCache();
    std::vector<morph::table::Cell> cells(20);
    REQUIRE(cache->snapshot()->read(morph::table::RowId{std::int64_t{1}}, cells) == morph::table::LevelRead::Loaded);
    morph::table::detail::LevelCacheAccess::store(
        *cache, std::vector<std::pair<morph::table::RowId, std::vector<morph::table::Cell>>>{
                    {morph::table::RowId{std::int64_t{1}}, cells}});
    REQUIRE(server.server->announce("WideTableModel.wide", std::nullopt));
    for (auto const& held : clients) {
        auto& client = *held;
        REQUIRE(client.pumpUntil([&] { return !client.changes.empty(); }));
    }
    // The fifty refetches are sent before any reply is read, and the computation they share waits until the other
    // forty-nine have joined it (or ten seconds, when they compute instead).
    auto const waitsBefore = server.server->viewStats().waits;
    server.gate = [&server, waitsBefore] {
        auto const deadline = std::chrono::steady_clock::now() + 10s;
        while (server.server->viewStats().waits < waitsBefore + 49 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
    };
    std::vector<std::optional<ui::ExchangeReply>> replies(clients.size());
    for (std::size_t i = 0; i < clients.size(); ++i) {
        morph::wire::Envelope env;
        env.kind = "execute";
        env.modelId = instances[i];
        env.modelType = std::string{bench::kWideModelType};
        env.actionType = "Table.wide";
        env.body = query;
        env.session.principal = "alice";
        clients[i]->exchange(std::move(env),
                             [&replies, i](ui::ExchangeReply answer) { replies[i] = std::move(answer); });
    }
    for (std::size_t i = 0; i < clients.size(); ++i) {
        REQUIRE(clients[i]->pumpUntil([&] { return replies[i].has_value(); }, 30s));
        REQUIRE(replies[i]->has_value());
        CHECK((*replies[i])->kind == "ok");
    }
    server.gate = {};
    CHECK(server.server->viewStats().computations == before + 1);
    CHECK(server.server->viewStats().waits == waitsBefore + 49);
    auto const work = server.tableWork->threads();
    auto const computed = server.computeThreads();
    REQUIRE_FALSE(computed.empty());
    CHECK(std::ranges::all_of(computed, [&](std::thread::id id) { return work.contains(id); }));
    // Mutations: no single flight (a computation per refetch, each waiting out the gate); QueryTable::invoke
    // running answerQuery inline (the computing thread is a strand's, not a tableWork thread).
}

TEST_CASE("table over the bridge: fifty sorts stay inside the bound", "[net][tables][views]") {
    bench::WideTableModel::recipeGate().release();
    bench::WideTableModel::configure(
        {.rows = 2'000, .columns = 300, .seed = 7, .loadDelay = {}, .recipePreloaded = 1.0},
        {.rows = 1'000, .columns = 30, .seed = 7, .loadDelay = {}, .recipePreloaded = 1.0});
    std::size_t const viewBytes = 4UZ * 2'000;  // a view of every row of `wide`
    ViewServer const server{10 * viewBytes};
    Client client{server.url, "alice"};
    auto const instance = client.create(std::string{bench::kWideModelType});
    for (int i = 1; i <= 50; ++i) {
        auto const query = std::format(R"({{"sort":[{{"column":"i{:03}","dir":"asc"}}],"columns":["id"],)"
                                       R"("page":{{"offset":0,"limit":10}}}})",
                                       i);
        static_cast<void>(pageOf(client.execute(instance, std::string{bench::kWideModelType}, "Table.wide", query)));
    }
    auto const stats = server.server->viewStats();
    CHECK(stats.computations == 50);
    CHECK(stats.bytes <= 10 * viewBytes);
    CHECK(stats.evictions > 0);
    // Mutation: no bound (forty views over it are kept and nothing is evicted).
}

TEST_CASE("table over the bridge: two scopes over one snapshot get two views", "[net][tables][views]") {
    bench::WideTableModel::recipeGate().release();
    bench::WideTableModel::configure(
        {.rows = 2'000, .columns = 300, .seed = 7, .loadDelay = {}, .recipePreloaded = 1.0},
        {.rows = 1'000, .columns = 30, .seed = 7, .loadDelay = {}, .recipePreloaded = 1.0});
    ViewServer const server;
    Client alice{server.url, "alice"};
    Client carol{server.url, "carol"};
    auto const aliceModel = alice.create(std::string{bench::kWideModelType});
    auto const carolModel = carol.create(std::string{bench::kWideModelType});
    std::string const query = R"({"sort":[{"column":"i001","dir":"asc"}],"columns":["id","i001"],)"
                              R"("page":{"offset":0,"limit":10}})";
    // `scoped` serves one source to every principal, so both views are over one snapshot.
    static_cast<void>(pageOf(alice.execute(aliceModel, std::string{bench::kWideModelType}, "Table.scoped", query)));
    static_cast<void>(pageOf(carol.execute(carolModel, std::string{bench::kWideModelType}, "Table.scoped", query)));
    static_cast<void>(pageOf(alice.execute(aliceModel, std::string{bench::kWideModelType}, "Table.scoped", query)));
    CHECK(server.server->viewStats().computations == 2);
    CHECK(server.server->viewStats().hits == 1);
    // Mutation: an empty scope in the view key (carol is answered alice's view: one computation).
}

TEST_CASE("table benchmark: C1 C2 C3 many clients over the bridge", "[.benchmark][net]") {
    bench::WideTableModel::recipeGate().release();
    bench::WideTableModel::configure(
        {.rows = 100'000, .columns = 300, .seed = 7, .loadDelay = {}, .recipePreloaded = 1.0},
        {.rows = 1'000, .columns = 30, .seed = 7, .loadDelay = {}, .recipePreloaded = 1.0});
    ViewServer const server{10UZ * 4 * 100'000};
    std::vector<std::unique_ptr<Client>> clients;
    std::vector<std::uint64_t> instances;
    for (int i = 0; i < 50; ++i) {
        clients.push_back(std::make_unique<Client>(server.url, "alice"));
        instances.push_back(clients.back()->create(std::string{bench::kWideModelType}));
    }
    auto const ask = [&](std::size_t i, std::string const& body) {
        morph::wire::Envelope env;
        env.kind = "execute";
        env.modelId = instances[i];
        env.modelType = std::string{bench::kWideModelType};
        env.actionType = "Table.wide";
        env.body = body;
        env.session.principal = "alice";
        return env;
    };
    std::string const sorted = R"({"sort":[{"column":"d000","dir":"asc"}],"columns":["id","d000"],)"
                               R"("page":{"offset":0,"limit":50}})";
    // C1: fifty concurrent refetches of one sorted view after a new order-level generation.
    auto const cache = bench::WideTableModel::recipeCache();
    std::vector<morph::table::Cell> cells(20);
    REQUIRE(cache->snapshot()->read(morph::table::RowId{std::int64_t{1}}, cells) == morph::table::LevelRead::Loaded);
    morph::table::detail::LevelCacheAccess::store(
        *cache, std::vector<std::pair<morph::table::RowId, std::vector<morph::table::Cell>>>{
                    {morph::table::RowId{std::int64_t{1}}, cells}});
    std::vector<std::optional<ui::ExchangeReply>> replies(clients.size());
    auto const began = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < clients.size(); ++i) {
        clients[i]->exchange(ask(i, sorted),
                             [&replies, i](ui::ExchangeReply answer) { replies[i] = std::move(answer); });
    }
    for (std::size_t i = 0; i < clients.size(); ++i) {
        REQUIRE(clients[i]->pumpUntil([&] { return replies[i].has_value(); }, 60s));
    }
    auto const c1 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
    std::println("| C1 | fifty concurrent refetches of one sorted view, 100,000 x 300: {:.1f} ms, {} computed |", c1,
                 server.server->viewStats().computations);
    // C2: distinct sorts computed one after another.
    auto const computed = server.server->viewStats().computations;
    auto const c2Began = std::chrono::steady_clock::now();
    for (int column = 1; column <= 20; ++column) {
        static_cast<void>(pageOf(clients[0]->execute(
            instances[0], std::string{bench::kWideModelType}, "Table.wide",
            std::format(
                R"({{"sort":[{{"column":"i{:03}","dir":"asc"}}],"columns":["id"],"page":{{"offset":0,"limit":10}}}})",
                column))));
    }
    auto const seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - c2Began).count();
    std::println("| C2 | views computed per second: {:.1f} ({} views) |",
                 static_cast<double>(server.server->viewStats().computations - computed) / seconds,
                 server.server->viewStats().computations - computed);
    // C3: fifty sorts against a bound of ten views.
    for (int column = 21; column <= 50; ++column) {
        static_cast<void>(pageOf(clients[0]->execute(
            instances[0], std::string{bench::kWideModelType}, "Table.wide",
            std::format(
                R"({{"sort":[{{"column":"i{:03}","dir":"asc"}}],"columns":["id"],"page":{{"offset":0,"limit":10}}}})",
                column))));
    }
    auto const stats = server.server->viewStats();
    std::println("| C3 | after fifty sorts: {} bytes held of a {} bound, {} evictions |", stats.bytes,
                 10 * 4 * 100'000, stats.evictions);
}

namespace {

// The wide fixture's 100,000 x 300 table, every level loaded: configured before a server attaches its recipe cache,
// as every case of this file does.
struct WideFixture {
    WideFixture() {
        bench::WideTableModel::recipeGate().release();
        bench::WideTableModel::configure(
            {.rows = 100'000, .columns = 300, .seed = 7, .loadDelay = {}, .recipePreloaded = 1.0},
            {.rows = 1'000, .columns = 30, .seed = 7, .loadDelay = {}, .recipePreloaded = 1.0});
    }
};

// A link with @p latency each way in front of a model's channel: a call leaves after the latency and its reply is
// handed on the latency after it lands, both slept on the client's owner, so the server's work in between is
// measured as it is. It counts the first-page fetches it carries: the round trips a change of the query costs.
class SlowLink final : public ui::ModelChannel {
public:
    SlowLink(std::shared_ptr<ui::ModelChannel> inner, morph::exec::IExecutor& owner, std::chrono::milliseconds latency)
        : _inner{std::move(inner)}, _owner{&owner}, _latency{latency} {}

    [[nodiscard]] morph::async::Completion<std::string> execute(std::string const& actionId,
                                                                std::string const& bodyJson,
                                                                ui::CallOptions options) override {
        bool const page = !actionId.ends_with(".describe") && !bodyJson.contains(R"("keys")") &&
                          !bodyJson.contains(R"("distinct")") && bodyJson.contains(R"("offset":0)");
        if (page) {
            ++firstPages;
        }
        std::this_thread::sleep_for(_latency);
        auto [completion, promise] = morph::async::Completion<std::string>::makeSettleable(_owner);
        auto const settle = std::make_shared<morph::async::Completion<std::string>::Promise>(std::move(promise));
        auto const latency = _latency;
        auto sent = _inner->execute(actionId, bodyJson, std::move(options));
        sent.then([this, settle, latency, page](std::string const& reply) {
            std::this_thread::sleep_for(latency);
            if (page) {
                ++answeredPages;
            }
            settle->resolve(reply);
        });
        sent.onError([settle](std::exception_ptr const& error) { settle->reject(error); });
        return std::move(completion);
    }

    [[nodiscard]] ui::ChannelFacts facts() const override { return _inner->facts(); }

    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): the page counters the case reads after the run
    int firstPages = 0;
    int answeredPages = 0;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

private:
    std::shared_ptr<ui::ModelChannel> _inner;
    morph::exec::IExecutor* _owner;
    std::chrono::milliseconds _latency;
};

// One client as a UI session over the bridge, its model's calls through a `SlowLink`: what a model-backed table's
// `WindowRows` reads through.
struct SessionOverLink {
    SessionOverLink(std::string const& url, std::chrono::milliseconds latency) {
        auto made = std::make_unique<morph::net::SocketBackend>(url);
        socket = made.get();
        REQUIRE(socket->waitForConnected());
        exchange = ui::exchangeOver(*socket, owner);
        bridge = std::make_unique<morph::bridge::Bridge>(std::move(made), owner);
        dispatcher = std::make_unique<ui::BridgeDispatcher>(*bridge, &owner);
        ui::SessionOptions options;
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
        link = std::make_shared<SlowLink>(
            dispatcher->bind(std::string{bench::kWideModelType}, morph::bridge::BindSharing::Private, std::string{}),
            owner, latency);
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

    // The table's description, read as `Table.<table>.describe` through the session.
    morph::table::TableDescription describe(std::string const& table) {
        std::optional<std::string> text;
        bool failed = false;
        auto completion = session->read(link, "Table." + table + ".describe", ui::Value{ui::ValueRecord{}});
        completion.then([&text](std::string const& reply) { text = reply; })
            .onError([&failed](std::exception_ptr const&) { failed = true; });
        REQUIRE(pumpUntil([&] { return text.has_value() || failed; }));
        REQUIRE(text.has_value());
        morph::table::TableDescription description;
        REQUIRE_FALSE(glz::read<glz::opts{.error_on_unknown_keys = false}>(description, *text));
        return description;
    }

    morph::exec::MainThreadExecutor owner;
    morph::reactive::Runtime runtime{owner};
    morph::net::SocketBackend* socket = nullptr;
    ui::Exchange exchange;
    std::unique_ptr<morph::bridge::Bridge> bridge;
    std::unique_ptr<ui::BridgeDispatcher> dispatcher;
    std::unique_ptr<ui::Session> session;
    std::shared_ptr<SlowLink> link;
};

// A table's rows over `wide` through the link, showing rows 0 to 30 of the columns @p columns names, and whether
// the rows took a first page that came after `expect` was called.
struct WideWindow {
    WideWindow(SessionOverLink& client, std::vector<std::string> columns)
        : link{client.link},
          rows{std::make_shared<ui::WindowRows>(
              *client.session,
              ui::WindowRowsOptions{
                  .channel = client.link, .table = "wide", .records = {}, .pageSize = 200, .maxPages = 8})},
          shown{std::move(columns)} {
        rows->onChange([this](morph::table::ViewChange const& /*change*/) {
            if (link->answeredPages > mark && rows->row(0).has_value()) {
                accepted = true;
            }
        });
        rows->setDescription(client.describe("wide"));
        rows->visible(0, 30, shown);
    }

    // From now on, a first page answered after this call is the one awaited.
    void expect() {
        mark = link->answeredPages;
        accepted = false;
    }

    std::shared_ptr<SlowLink> link;
    std::shared_ptr<ui::WindowRows> rows;
    std::vector<std::string> shown;
    int mark = 0;
    bool accepted = false;
};

}  // namespace

// B3, recorded and not a gate. 50 ms each way, through the rows a model-backed table reads: a header sort's new
// chain to the accepted page (bound 150 ms), and the filter the last key of a text condition sends to the accepted
// page (bound 150 ms after the filter menu's 250 ms debounce, which "a condition applies once after typing pauses"
// gates in morph_tests). Each change costs one round trip: one first-page fetch. Unbuilt on the host that wrote it,
// and its mutation not run: WindowRows sending a query change twice (restart called a second time in `setSort` or
// `setFilter`) makes `firstPages` grow by two.
TEST_CASE("table benchmark: a sort change and a text filter over a socket with 50 ms each way",
          "[.benchmark][table]") {
    WideFixture const fixture;
    ViewServer const server{};
    SessionOverLink client{server.url, 50ms};
    WideWindow window{client, {"id", "d000", "t011"}};
    REQUIRE(client.pumpUntil([&] { return window.accepted; }, 60s));

    int const beforeSort = client.link->firstPages;
    window.expect();
    auto const sortBegan = std::chrono::steady_clock::now();
    REQUIRE(
        window.rows->setSort({morph::table::SortKey{.column = "d000", .dir = morph::table::SortDirection::Ascending}})
            .has_value());
    REQUIRE(client.pumpUntil([&] { return window.accepted; }, 10s));
    auto const sortMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sortBegan).count();
    CHECK(client.link->firstPages - beforeSort == 1);

    int const beforeFilter = client.link->firstPages;
    window.expect();
    morph::table::FilterEntry contains;
    contains.contains = "a";
    morph::table::FilterSpec filter;
    filter.columns.insert_or_assign(
        "t011",
        morph::table::ColumnFilter{.combine = morph::table::Combine::Any, .include = {contains}, .exclude = {}});
    auto const keyBegan = std::chrono::steady_clock::now();
    REQUIRE(window.rows->setFilter(filter).has_value());
    REQUIRE(client.pumpUntil([&] { return window.accepted; }, 10s));
    auto const keyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - keyBegan).count();
    CHECK(client.link->firstPages - beforeFilter == 1);

    std::println("| B3 | sort on d000, 100,000 x 300, 50 ms each way, to the accepted page: {:.1f} ms (bound 150) |",
                 sortMs);
    std::println(
        "| B3 | a text filter's query to the accepted page, 50 ms each way: {:.1f} ms (bound 150 after the "
        "250 ms debounce) |",
        keyMs);
}

// D9, recorded and not a gate: the server side of a values list, a `distinct` query on `t011` to its reply (bound
// 150 ms from the menu's opening to the list shown, which this figure does not include the client's drawing in).
TEST_CASE("table benchmark: a server-mode values list from open to shown", "[.benchmark][table]") {
    WideFixture const fixture;
    ViewServer const server{};
    Client client{server.url, "alice"};
    auto const instance = client.create(std::string{bench::kWideModelType});
    auto const began = std::chrono::steady_clock::now();
    auto const listed = pageOf(
        client.execute(instance, std::string{bench::kWideModelType}, "Table.wide",
                       R"({"distinct":{"column":"t011","search":"","bucket":"value","offset":0,"limit":200}})"));
    auto const ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
    REQUIRE(listed.distinct.has_value());
    CHECK_FALSE(listed.distinct->values.empty());
    std::println(
        "| D9 | values list of t011, 100,000 rows, the server's reply: {:.1f} ms, {} values of {} (bound 150) |", ms,
        listed.distinct->values.size(), listed.distinct->distinct);
}

namespace {

// One server on a socket offering `morph.tableState` over a memory preferences store, with the tables authorizer.
struct StateServer {
    StateServer() {
        ui::registerTableState(prefs, dispatcher, registry);
        morph::backend::ServerConfig config;
        config.services.push_back(std::make_shared<Hello>());
        server = std::make_shared<morph::backend::RemoteServer>(pool, std::make_shared<TablesAuthorizer>(),
                                                                std::move(config), dispatcher, registry);
        socket = std::make_unique<morph::net::SocketServer>(*server, 0);
        REQUIRE(socket->listen());
        url = "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(socket->port()));
    }

    morph::exec::ThreadPoolExecutor pool{2};
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    std::shared_ptr<ui::MemoryPreferencesStore> prefs = std::make_shared<ui::MemoryPreferencesStore>(pool);
    std::shared_ptr<morph::backend::RemoteServer> server;
    std::unique_ptr<morph::net::SocketServer> socket;
    std::string url;
};

}  // namespace

TEST_CASE("table state: the person is the store's", "[net][tables][state]") {
    StateServer const server;
    Client alice{server.url, "alice"};
    Client bob{server.url, "bob"};
    std::string const model{ui::kTableStateModel};
    auto const aliceState = alice.create(model);
    auto const bobState = bob.create(model);
    morph::table::TableStateKey const key{.application = "lims",
                                          .model = "Lims_SampleModel",
                                          .table = "samples",
                                          .qualifier = "samples",
                                          .part = "state"};
    auto const save = [&](Client& client, std::uint64_t instance, std::string text) {
        auto const reply = client.execute(
            instance, model, "save",
            morph::model::ActionTraits<ui::TableStateSave>::toJson(ui::TableStateSave{
                .key = key, .expected = {}, .upload = {}, .offset = 0, .text = std::move(text), .last = true}));
        REQUIRE(reply.kind == "ok");
    };
    auto const loadedText = [&](Client& client, std::uint64_t instance, std::string const& body) {
        auto const reply = client.execute(instance, model, "load", body);
        REQUIRE(reply.kind == "ok");
        auto const loaded = morph::model::ActionTraits<ui::TableStateLoad>::resultFromJson(reply.body);
        REQUIRE(loaded.entries.size() == 1);
        return loaded.entries.front().text;
    };
    save(alice, aliceState, R"({"version":1,"view":"alice's"})");
    save(bob, bobState, R"({"version":1,"view":"bob's"})");
    std::string const keys = morph::model::ActionTraits<ui::TableStateLoad>::toJson(ui::TableStateLoad{.keys = {key}});
    CHECK(loadedText(alice, aliceState, keys) == R"({"version":1,"view":"alice's"})");
    CHECK(loadedText(bob, bobState, keys) == R"({"version":1,"view":"bob's"})");
    // A body naming another person is read with that member ignored.
    std::string const naming = keys.substr(0, keys.size() - 1) + R"(,"person":"bob"})";
    CHECK(loadedText(alice, aliceState, naming) == R"({"version":1,"view":"alice's"})");
    // Mutation: the model keying by a body member (alice's load naming bob reads bob's state). Not run: this case is
    // in morph_net_tests, which builds only on POSIX, and has been neither built nor run; its in-process twin in
    // tests/test_ui_table_state.cpp has, with this mutation.
}

TEST_CASE("shared views: a view published over a socket is read by a second client", "[net][tables][state]") {
    StateServer const server;
    Client alice{server.url, "alice"};
    Client bob{server.url, "bob"};
    std::string const model{ui::kTableStateModel};
    auto const aliceState = alice.create(model);
    auto const bobState = bob.create(model);
    morph::table::TableStateKey const shared{.application = "lims",
                                             .model = "Lims_SampleModel",
                                             .table = "samples",
                                             .qualifier = "samples",
                                             .part = "shared"};
    // Mutation: publish written under the caller's person (bob does not see it).
    auto const publish =
        alice.execute(aliceState, model, "publish",
                      morph::model::ActionTraits<ui::TableStatePublish>::toJson(ui::TableStatePublish{
                          .key = shared,
                          .operation = morph::table::writeViewOperation(morph::table::AddView{
                              morph::table::SavedView{.name = "Mine", .layout = {}, .sort = {}, .filters = {}}}),
                          .expected = std::string{morph::table::kNoDocument}}));
    REQUIRE(publish.kind == "ok");
    auto const published = morph::model::ActionTraits<ui::TableStatePublish>::resultFromJson(publish.body).result;
    REQUIRE(published.outcome == morph::table::SaveOutcome::Saved);
    auto const read =
        bob.execute(bobState, model, "load",
                    morph::model::ActionTraits<ui::TableStateLoad>::toJson(ui::TableStateLoad{.keys = {shared}}));
    REQUIRE(read.kind == "ok");
    auto const loaded = morph::model::ActionTraits<ui::TableStateLoad>::resultFromJson(read.body);
    REQUIRE(loaded.entries.size() == 1);
    CHECK(loaded.entries.front().revision == published.revision);
    auto const views = morph::table::readViews(loaded.entries.front().text);
    REQUIRE(views.has_value());
    REQUIRE(views->views.size() == 1);
    CHECK(views->views.front().name == "Mine");

    // Mutation: the model treating kNoDocument as an ordinary revision in `save` (the first save answers Conflict).
    auto own = shared;
    own.part = "views";
    auto const saveOnce = [&] {
        auto const reply = alice.execute(aliceState, model, "save",
                                         morph::model::ActionTraits<ui::TableStateSave>::toJson(
                                             ui::TableStateSave{.key = own,
                                                                .expected = std::string{morph::table::kNoDocument},
                                                                .upload = {},
                                                                .offset = 0,
                                                                .text = R"({"version":1,"views":[]})",
                                                                .last = true}));
        REQUIRE(reply.kind == "ok");
        return morph::model::ActionTraits<ui::TableStateSave>::resultFromJson(reply.body).result;
    };
    auto const first = saveOnce();
    REQUIRE(first.outcome == morph::table::SaveOutcome::Saved);
    auto const again = saveOnce();
    CHECK(again.outcome == morph::table::SaveOutcome::Conflict);
    CHECK(again.revision == first.revision);
}

// A view published under a new name and then replaced under that name over a socket is one view, which a second
// client reads with the replacing sort. Mutation: `applyViewOperation` answering a `ReplaceView` as `NameTaken` (the
// second publish is Refused).
TEST_CASE("shared views: a view published twice under one name over a socket is replaced", "[net][tables][state]") {
    StateServer const server;
    Client alice{server.url, "alice"};
    Client bob{server.url, "bob"};
    std::string const model{ui::kTableStateModel};
    auto const aliceState = alice.create(model);
    auto const bobState = bob.create(model);
    morph::table::TableStateKey const shared{.application = "lims",
                                             .model = "Lims_SampleModel",
                                             .table = "samples",
                                             .qualifier = "samples",
                                             .part = "shared"};
    auto const team = [](morph::table::SortDirection direction) {
        return morph::table::SavedView{.name = "Team",
                                       .layout = {},
                                       .sort = {morph::table::SortKey{.column = "name", .dir = direction}},
                                       .filters = {}};
    };
    auto const publish = [&](morph::table::ViewOperation const& operation, std::string expected) {
        auto const reply =
            alice.execute(aliceState, model, "publish",
                          morph::model::ActionTraits<ui::TableStatePublish>::toJson(
                              ui::TableStatePublish{.key = shared,
                                                    .operation = morph::table::writeViewOperation(operation),
                                                    .expected = std::move(expected)}));
        REQUIRE(reply.kind == "ok");
        return morph::model::ActionTraits<ui::TableStatePublish>::resultFromJson(reply.body).result;
    };
    auto const first = publish(morph::table::AddView{team(morph::table::SortDirection::Ascending)},
                               std::string{morph::table::kNoDocument});
    REQUIRE(first.outcome == morph::table::SaveOutcome::Saved);
    auto const second =
        publish(morph::table::ReplaceView{team(morph::table::SortDirection::Descending)}, first.revision);
    REQUIRE(second.outcome == morph::table::SaveOutcome::Saved);
    auto const read =
        bob.execute(bobState, model, "load",
                    morph::model::ActionTraits<ui::TableStateLoad>::toJson(ui::TableStateLoad{.keys = {shared}}));
    REQUIRE(read.kind == "ok");
    auto const loaded = morph::model::ActionTraits<ui::TableStateLoad>::resultFromJson(read.body);
    REQUIRE(loaded.entries.size() == 1);
    CHECK(loaded.entries.front().revision == second.revision);
    auto const views = morph::table::readViews(loaded.entries.front().text);
    REQUIRE(views.has_value());
    REQUIRE(views->views.size() == 1);
    CHECK(views->views.front().sort.front().dir == morph::table::SortDirection::Descending);
}

// ── A selection of the view, resolved on the server ────────────────────────────────────────────────────────
// Written by reading: morph_net_tests builds on POSIX only, and was not built or run for these cases.

// NOLINTBEGIN(misc-use-internal-linkage)
namespace selnet {

struct TagRows;
struct MoveRow;
struct SaveAs;
struct SavedAs;

// Ten rows keyed 0..9: status "open" for 0..5, "closed" for 6..9. alice sees every row, bob rows 0..4.
class Rows {
public:
    Rows();
    morph::table::ServedTable rows(morph::table::TableContext const& context);
    core::async::Task<morph::table::ChangedRows<&Rows::rows>> execute(TagRows action);
    morph::table::ChangedRows<&Rows::rows> execute(MoveRow const& action);
    SavedAs execute(SaveAs const& action);

    static inline std::int64_t maxSelectedRows = 1'000'000;

private:
    std::shared_ptr<morph::table::VectorSource> _source;
};

struct TagRows {
    morph::table::RowSelection<&Rows::rows> rows;
    morph::table::RowRef<&Rows::rows> focus;
};

struct MoveRow {
    std::int64_t key = 0;
    std::string status;
};

// Saves row 2 as the new row 10, and cannot save row 4.
struct SaveAs {
    std::int64_t key = 0;
};

struct SavedAs {
    morph::table::ChangedRows<&Rows::rows> rows;
};

}  // namespace selnet

BRIDGE_REGISTER_MODEL(selnet::Rows, "selnet::Rows")
BRIDGE_REGISTER_TABLE(selnet::Rows, "rows", &selnet::Rows::rows, morph::table::rowScoped)
BRIDGE_REGISTER_ACTION(selnet::Rows, selnet::TagRows, "TagRows")
BRIDGE_REGISTER_ACTION(selnet::Rows, selnet::MoveRow, "MoveRow")
BRIDGE_REGISTER_EFFECTS(selnet::MoveRow, morph::effects::updatesTable<&selnet::Rows::rows>)
BRIDGE_REGISTER_ACTION(selnet::Rows, selnet::SaveAs, "SaveAs")
BRIDGE_REGISTER_EFFECTS(selnet::SaveAs, morph::effects::updatesTable<&selnet::Rows::rows>)

namespace selnet {

namespace {

std::vector<morph::table::TableColumn> rowColumns() {
    return {morph::table::TableColumn{.info = {.id = "id", .kind = morph::table::ColumnKind::Integer}, .label = "Id"},
            morph::table::TableColumn{.info = {.id = "status", .kind = morph::table::ColumnKind::Text},
                                      .label = "Status"}};
}

}  // namespace

Rows::Rows() {
    morph::table::TableDescription description;
    description.columns = rowColumns();
    _source = std::make_shared<morph::table::VectorSource>(morph::table::columnInfos(description));
    std::vector<morph::table::RowId> keys;
    std::vector<std::vector<morph::table::Cell>> cells;
    for (std::int64_t row = 0; row < 10; ++row) {
        keys.emplace_back(row);
        cells.push_back({morph::table::Cell{row}, morph::table::Cell{std::string{row < 6 ? "open" : "closed"}}});
    }
    _source->setRows(std::move(keys), std::move(cells));
}

morph::table::ServedTable Rows::rows(morph::table::TableContext const& context) {
    morph::table::ServedTable served;
    served.description.id = "rows";
    served.description.key = "id";
    served.description.columns = rowColumns();
    served.scope = context.session.principal;
    served.limits.maxSelectedRows = maxSelectedRows;
    if (context.session.principal == "bob") {
        std::vector<morph::table::RowId> mine;
        for (std::int64_t row = 0; row < 5; ++row) {
            // NOLINTNEXTLINE(performance-inefficient-vector-operation): `mine` is a fixture list of five keys built once; sizing it would not change what the case checks
            mine.emplace_back(row);
        }
        served.source = morph::table::scoped(_source, std::move(mine));
    } else {
        served.source = _source;
    }
    return served;
}

core::async::Task<morph::table::ChangedRows<&Rows::rows>> Rows::execute(TagRows action) {
    auto const context = morph::table::detail::callerContext();
    auto completion = morph::table::resolve(*this, action.rows, context, morph::table::SelectionPolicy::Exact);
    auto resolved = co_await std::move(completion);
    if (!resolved) {
        morph::table::refuse(resolved.error());
    }
    morph::table::ChangedRows<&Rows::rows> out;
    for (auto const& key : resolved->keys) {
        out.rows.push_back(morph::table::ChangedRow{.key = key});
    }
    co_return out;
}

morph::table::ChangedRows<&Rows::rows> Rows::execute(MoveRow const& action) {
    _source->patchRow(morph::table::RowId{action.key},
                      {morph::table::Cell{action.key}, morph::table::Cell{action.status}});
    return {.rows = {morph::table::ChangedRow{.key = morph::table::RowId{action.key}}}};
}

SavedAs Rows::execute(SaveAs const& action) {
    static_cast<void>(action);
    SavedAs out;
    out.rows.rows.push_back(morph::table::ChangedRow{.key = morph::table::RowId{std::int64_t{10}},
                                                     .from = morph::table::RowId{std::int64_t{2}}});
    out.rows.failed.push_back(
        morph::table::FailedRow{.key = morph::table::RowId{std::int64_t{4}}, .message = "Locked"});
    return out;
}

}  // namespace selnet
// NOLINTEND(misc-use-internal-linkage)

namespace {

constexpr std::string_view kOpen = R"({"columns":{"status":{"include":[{"eq":"open"}]}}})";

// The keys of a ChangedRows reply (`rows`, objects with `key`) or a SelectedRows reply (`keys`).
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): `keysOfReply(body, member)` is a test helper whose arguments read differently at each call; a swap would fail the case's own assertion
std::vector<std::int64_t> keysOfReply(std::string const& body, std::string const& member) {
    glz::generic_u64 json;
    REQUIRE_FALSE(glz::read_json(json, body));
    std::vector<std::int64_t> out;
    for (auto const& item : json[member].get_array()) {
        out.push_back(item.is_object() ? item.at("key").as<std::int64_t>() : item.as<std::int64_t>());
    }
    return out;
}

}  // namespace

TEST_CASE("tables: an action resolves a view selection on its own model", "[net][tables][selection]") {
    Server const server;
    Client alice{server.url, "alice"};
    auto const model = alice.create("selnet::Rows");
    auto const page =
        pageOf(alice.execute(model, "selnet::Rows", "Table.rows", R"({"filters":)" + std::string{kOpen} + "}"));
    std::string const revision = page.viewRevision;
    REQUIRE(revision.size() == 48);
    std::string const tag = R"({"rows":{"view":{"filters":)" + std::string{kOpen} + R"(,"retained":[],"revision":")" +
                            revision + R"("},"except":[3]},"focus":0})";
    auto const tagged = alice.execute(model, "selnet::Rows", "TagRows", tag);
    REQUIRE(tagged.kind == "ok");
    CHECK(keysOfReply(tagged.body, "rows") == std::vector<std::int64_t>{0, 1, 2, 4, 5});
    REQUIRE(alice.execute(model, "selnet::Rows", "MoveRow", R"({"key":7,"status":"open"})").kind == "ok");
    auto const refused = alice.execute(model, "selnet::Rows", "TagRows", tag);
    CHECK(refused.kind == "err");
    REQUIRE(refused.refusal.has_value());
    CHECK(refused.refusal->code == "morph.table.viewChanged");
}
// Mutation: viewRevision that ignores the source snapshot, which tags rows under the old revision.

TEST_CASE("tables: Table.<id>.resolve answers for the caller", "[net][tables][selection]") {
    Server const server;
    Client bob{server.url, "bob"};
    Client alice{server.url, "alice"};
    auto const bobModel = bob.create("selnet::Rows");
    auto const keys = bob.execute(bobModel, "selnet::Rows", "Table.rows.resolve",
                                  R"({"selection":{"keys":[1,3,8]},"policy":"current"})");
    REQUIRE(keys.kind == "ok");
    CHECK(keysOfReply(keys.body, "keys") == std::vector<std::int64_t>{1, 3});
    CHECK(keys.body.contains(R"("dropped":1)"));
    std::string const view = R"({"selection":{"view":{"filters":)" + std::string{kOpen} +
                             R"(,"retained":[],"revision":""},"except":[]},"policy":"current"})";
    auto const scoped = bob.execute(bobModel, "selnet::Rows", "Table.rows.resolve", view);
    REQUIRE(scoped.kind == "ok");
    CHECK(keysOfReply(scoped.body, "keys") == std::vector<std::int64_t>{0, 1, 2, 3, 4});  // none outside the scope
    selnet::Rows::maxSelectedRows = 5;
    auto const aliceModel = alice.create("selnet::Rows");
    auto const over = alice.execute(aliceModel, "selnet::Rows", "Table.rows.resolve", view);
    selnet::Rows::maxSelectedRows = 1'000'000;
    CHECK(over.kind == "err");
    REQUIRE(over.refusal.has_value());
    CHECK(over.refusal->code == "table.limitExceeded");
}
// Mutation: resolve run under the server principal, which returns rows outside the caller scope.

TEST_CASE("tables: a selection's schema names its table", "[net][tables][selection]") {
    glz::generic_u64 schema;
    REQUIRE_FALSE(glz::read_json(schema, morph::forms::schemaJson<selnet::TagRows>()));
    CHECK(schema["properties"]["rows"]["x-selectionOf"].get_string() == "selnet::Rows.rows");
    CHECK(schema["properties"]["focus"]["x-rowOf"].get_string() == "selnet::Rows.rows");
}
// Mutation: schemaValue returning an empty name, which leaves the client unable to mark busy rows.

TEST_CASE("tables: a changed key's event names both keys", "[net][tables][keys]") {
    Server const server;
    Client alice{server.url, "alice"};
    REQUIRE(alice.watch({"selnet::Rows.rows"}).empty());
    auto const model = alice.create("selnet::Rows");
    auto const saved = alice.execute(model, "selnet::Rows", "SaveAs", R"({"key":2})");
    REQUIRE(saved.kind == "ok");
    CHECK(saved.body.contains(R"("from":2)"));
    CHECK(saved.body.contains(R"("message":"Locked")"));
    REQUIRE(alice.pumpUntil([&] { return !alice.changes.empty(); }));
    alice.pumpFor(3 * kCoalesce);
    REQUIRE(alice.changes.size() == 1);
    auto keys = alice.keysOf("selnet::Rows.rows");
    std::ranges::sort(keys);
    CHECK(keys == std::vector<std::string>{"10", "2"});
}
// Mutation: the walk ignoring `from`, which leaves the old row's watchers untold (keys {10}).

// ── Cells of a model's table over a socket ──────────────────────────────────

// NOLINTBEGIN(misc-use-internal-linkage): registered types must have external linkage.
namespace editnet {

struct EditSpecimenCount;

// Three specimens keyed 1..3, each with a `count` equal to its key, which `Lims_EditSpecimenCount` changes.
class Specimens {
public:
    Specimens();
    morph::table::ServedTable specimens(morph::table::TableContext const& context);
    morph::table::ChangedRows<&Specimens::specimens> execute(EditSpecimenCount const& action);

private:
    std::shared_ptr<morph::table::VectorSource> _source;
};

struct EditSpecimenCount : morph::table::CellEdit<&Specimens::specimens, std::int64_t> {};

}  // namespace editnet

BRIDGE_REGISTER_MODEL(editnet::Specimens, "editnet::Specimens")
BRIDGE_REGISTER_TABLE(editnet::Specimens, "specimens", &editnet::Specimens::specimens)
BRIDGE_REGISTER_ACTION(editnet::Specimens, editnet::EditSpecimenCount, "Lims_EditSpecimenCount")
BRIDGE_REGISTER_EFFECTS(editnet::EditSpecimenCount, morph::effects::updatesTable<&editnet::Specimens::specimens>)

namespace editnet {

namespace {

// `key` (the key) and `count`, edited by `Lims_EditSpecimenCount`.
std::vector<morph::table::TableColumn> specimenColumns() {
    return {
        morph::table::TableColumn{.info = {.id = "key", .kind = morph::table::ColumnKind::Integer}, .label = "Key"},
        morph::table::TableColumn{.info = {.id = "count", .kind = morph::table::ColumnKind::Integer},
                                  .label = "Count",
                                  .edit = morph::table::EditInfo{.action = "Lims_EditSpecimenCount"}}};
}

}  // namespace

Specimens::Specimens() {
    morph::table::TableDescription description;
    description.columns = specimenColumns();
    _source = std::make_shared<morph::table::VectorSource>(morph::table::columnInfos(description));
    std::vector<morph::table::RowId> keys;
    std::vector<std::vector<morph::table::Cell>> cells;
    for (std::int64_t row = 1; row <= 3; ++row) {
        keys.emplace_back(row);
        cells.push_back({morph::table::Cell{row}, morph::table::Cell{row}});
    }
    _source->setRows(std::move(keys), std::move(cells));
}

morph::table::ServedTable Specimens::specimens(morph::table::TableContext const& /*context*/) {
    morph::table::ServedTable served;
    served.description.id = "specimens";
    served.description.key = "key";
    served.description.columns = specimenColumns();
    served.description.capabilities.keys = true;
    served.source = _source;
    return served;
}

morph::table::ChangedRows<&Specimens::specimens> Specimens::execute(EditSpecimenCount const& action) {
    auto const key = std::get<std::int64_t>(action.key);
    _source->patchRow(action.key, {morph::table::Cell{key}, morph::table::Cell{action.value}});
    return {.rows = {morph::table::ChangedRow{.key = action.key}}};
}

}  // namespace editnet
// NOLINTEND(misc-use-internal-linkage)

TEST_CASE("push tables: a cell edit reaches the model and the change event refetches the row", "[net][tables][edit]") {
    glz::generic_u64 schema;
    REQUIRE_FALSE(glz::read_json(schema, morph::forms::schemaJson<editnet::EditSpecimenCount>()));
    CHECK(schema["x-cellEditOf"].get_string() == "editnet::Specimens.specimens");
    Server const server;
    Client alice{server.url, "alice"};
    REQUIRE(alice.watch({"editnet::Specimens.specimens"}).empty());
    auto const model = alice.create("editnet::Specimens");
    // The description is checked against the action's input on the server: the count column's field fits it.
    auto const described = alice.execute(model, "editnet::Specimens", "Table.specimens.describe", "{}");
    REQUIRE(described.kind == "ok");
    CHECK(described.body.contains(R"("edit":{"action":"Lims_EditSpecimenCount")"));
    auto const edited = alice.execute(model, "editnet::Specimens", "Lims_EditSpecimenCount", R"({"key":1,"value":4})");
    REQUIRE(edited.kind == "ok");
    REQUIRE(alice.pumpUntil([&] { return !alice.changes.empty(); }));
    CHECK(alice.keysOf("editnet::Specimens.specimens") == std::vector<std::string>{"1"});
    // The refetch the change event causes: a keyed query of row 1 holds the count the model now has.
    auto const page = pageOf(alice.execute(model, "editnet::Specimens", "Table.specimens", R"({"keys":[1]})"));
    REQUIRE(page.rows.size() == 1);
    auto const at = std::ranges::find(page.columns, std::string{"count"});
    REQUIRE(at != page.columns.end());
    CHECK(page.rows.at(0).at(static_cast<std::size_t>(at - page.columns.begin())) ==
          morph::table::Cell{std::int64_t{4}});
}
// Mutation: the client sending {"key":…, "value":…} under the wrong action id (the model never sees it).

TEST_CASE("push tables: a principal refused the edit action sees the column read-only", "[net][tables][edit]") {
    // A bundle of one screen whose mutation runs the edit action, so the catalog lists it.
    morph::ui::author::CatalogFacts facts;
    facts.model<editnet::Specimens>();
    facts.action<editnet::Specimens, editnet::EditSpecimenCount>();
    morph::ui::UiRegistry registry{morph::ui::ApplicationInfo{.id = "lims", .version = "1.0.0"}, facts.catalog()};
    morph::ui::CatalogEntry entry;
    entry.id = "lims.edits";
    REQUIRE(registry.addScreen(entry, R"({"vocab":"ui/1","id":"lims.edits",)"
                                      R"("models":{"s":{"type":"editnet::Specimens"}},)"
                                      R"("mutations":{"edit":{"model":"s","action":"Lims_EditSpecimenCount",)"
                                      R"("body":{"lit":{"key":1,"value":4}}}},)"
                                      R"("view":{"kind":"button","label":"Edit","onClick":[{"run":"edit"}]}})"));
    auto const bundle = registry.build();
    REQUIRE(bundle);
    morph::exec::ThreadPoolExecutor pool{2};
    morph::backend::ServerConfig config;
    config.services.push_back(std::make_shared<morph::ui::UiService>(*bundle));
    auto const server =
        std::make_shared<morph::backend::RemoteServer>(pool, std::make_shared<TablesAuthorizer>(), std::move(config));
    auto const permitted = [&server](std::string principal) {
        morph::wire::Envelope request;
        request.kind = std::string{morph::ui::kCatalogKind};
        request.callId = 1;
        request.body = "{}";
        request.session.principal = std::move(principal);
        morph::wire::Envelope const reply = morph::wire::decode(server->handleInline(morph::wire::encode(request)));
        REQUIRE(reply.kind == "ok");
        auto const parsed = morph::ui::parseCatalogReply(reply.body);
        REQUIRE(parsed);
        return parsed->permitted;
    };
    // The catalog's permitted actions are what the client lowers the column by: the viewer's lack the edit action,
    // so its column is read-only ("model table: a column whose action is not permitted is read-only" in
    // tests/test_ui_table_model_*.cpp lowers that), and alice's have it.
    CHECK_FALSE(std::ranges::contains(permitted("viewer"), std::string{"Lims_EditSpecimenCount"}));
    CHECK(std::ranges::contains(permitted("alice"), std::string{"Lims_EditSpecimenCount"}));
}
// Mutation: the server's catalog listing every action as permitted (the viewer's column stays editable).
