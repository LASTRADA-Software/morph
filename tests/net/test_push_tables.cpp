// SPDX-License-Identifier: Apache-2.0

// A model's tables over the `net` transport (docs/spec/table/table.md, "A model's tables"): one server, two clients,
// each table a record of its own. Admission to a table's record follows the actions holding it; an action's changed
// rows reach the watchers of their own table; a page's columns are the caller's; and the reply is built on the
// server's table executor.
//
// Mutations these tests were seen to fail on are named at the end of each case.

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/net/socket_backend.hpp>
#include <morph/net/socket_server.hpp>
#include <morph/session/session.hpp>
#include <morph/table/description.hpp>
#include <morph/ui/service_client.hpp>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
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
        .info = {.id = "v", .kind = morph::table::ColumnKind::Integer, .comparator = {}, .format = {}},
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

// Takes the principal a client names as verified, and refuses `bob` every action holding `PtModel.second`.
class TablesAuthorizer final : public morph::session::IAuthorizer {
public:
    [[nodiscard]] bool authorize(morph::session::Context const& ctx, std::string_view /*modelType*/,
                                 std::string_view actionType) const override {
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
    morph::exec::ThreadPoolExecutor _pool{2};
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
        REQUIRE(send(std::move(hello)).kind == "ok");
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

    morph::exec::MainThreadExecutor owner;
    std::unique_ptr<morph::net::SocketBackend> socket;
    ui::Exchange exchange;
    std::vector<morph::wire::ChangesBody> changes;

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
    Server server;
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
    Server server;
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
    Server server;
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
    Server server;
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
    Server server{tableWork};
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
