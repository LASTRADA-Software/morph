// SPDX-License-Identifier: Apache-2.0
//
// Persisted table state on the client and on the server (docs/spec/table/table.md, "Persisted state"):
// `PreferencesTableStateStore` over the `morph.tableState` model of an in-process server whose preferences store a
// test can look into, and `TableStates`, the session's kept states, over a store whose loads a test holds.
//
// Each case names the mutation it was seen to fail on.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <core/async/ExecutorContext.hpp>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <future>
#include <memory>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/table/sort.hpp>
#include <morph/table/state_store.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/preferences.hpp>
#include <morph/ui/table_state.hpp>
#include <morph/ui/table_state_model.hpp>
#include <morph/ui/table_views.hpp>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ui_preferences_support.hpp"
#include "ui_table_support.hpp"

namespace {

using namespace std::chrono_literals;
using morph::ui::PrefKey;
using morph::ui::StateParts;
using morph::ui::TableStateOptions;
using morph::ui::TableStates;
using morph::ui::testing::GatedTableStateStore;
using morph::ui::testing::ProbePreferencesStore;
using morph::ui::testing::RowsFixture;
using morph::ui::testing::sharedOf;
namespace table = morph::table;
namespace ui = morph::ui;

constexpr char const* kPerson = "alice";

table::TableStateKey keyOf(std::string tableId) {
    return table::TableStateKey{.application = "lims",
                                .model = "Lims_SampleModel",
                                .table = std::move(tableId),
                                .qualifier = "samples",
                                .part = "state"};
}

// NOLINTNEXTLINE(bugprone-throwing-static-initialization, cert-err58-cpp): `kKey` is a test constant built from literals; if building it threw, the run would fail at load, which is what a broken fixture should do
table::TableStateKey const kKey = keyOf("samples");

// A server offering `morph.tableState` over a probe store; its token is the verified principal.
struct StateServer {
    explicit StateServer(TableStateOptions options, std::shared_ptr<morph::session::IAuthorizer> authorizer =
                                                        std::make_shared<prefs_test::TokenAuthorizer>()) {
        ui::registerTableState(prefs, dispatcher, registry, std::move(options));
        server = std::make_shared<morph::backend::RemoteServer>(pool, std::move(authorizer), dispatcher, registry);
    }

    morph::wire::Envelope send(morph::wire::Envelope const& request) {
        std::promise<std::string> replied;
        auto reply = replied.get_future();
        server->handle(morph::wire::encode(request),
                       [&replied](std::string text) { replied.set_value(std::move(text)); });
        REQUIRE(reply.wait_for(std::chrono::seconds{10}) == std::future_status::ready);
        return morph::wire::decode(reply.get());
    }

    std::uint64_t bind() {
        auto const reply = send(morph::wire::makeRegister(std::string{ui::kTableStateModel}));
        REQUIRE(reply.kind == "ok");
        return reply.modelId;
    }

    // Runs @p action with @p body, as the caller whose token is @p token, on @p instance.
    morph::wire::Envelope call(std::uint64_t instance, std::string_view action, std::string body, std::string token) {
        morph::wire::Envelope request;
        request.kind = "execute";
        request.callId = ++lastCallId;
        request.modelId = instance;
        request.modelType = std::string{ui::kTableStateModel};
        request.actionType = std::string{action};
        request.body = std::move(body);
        request.session.token = std::move(token);
        return send(request);
    }

    morph::exec::ThreadPoolExecutor pool{2};
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    std::shared_ptr<ProbePreferencesStore> prefs = std::make_shared<ProbePreferencesStore>(pool);
    std::shared_ptr<morph::backend::RemoteServer> server;
    std::uint64_t lastCallId = 0;
};

// A client dispatcher whose every call goes through the server, as `person`, and is answered on the owner.
class ServerDispatcher final : public ui::Dispatcher {
public:
    ServerDispatcher(StateServer& server, morph::exec::IExecutor& owner, std::string person)
        : _server{&server}, _owner{&owner}, _person{std::move(person)} {}

    [[nodiscard]] std::shared_ptr<ui::ModelChannel> bind(std::string const& typeId,
                                                         morph::bridge::BindSharing /*sharing*/,
                                                         std::string const& /*instanceKey*/) override {
        ++binds;
        REQUIRE(typeId == ui::kTableStateModel);
        return std::make_shared<Channel>(*this, _server->bind());
    }

    // The calls of @p action sent so far.
    [[nodiscard]] std::size_t count(std::string_view action) const {
        return static_cast<std::size_t>(std::ranges::count(calls, action));
    }

    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): the call and body logs and the bind counter, which the cases read directly
    std::vector<std::string> calls;
    /// The body of each call, in order.
    std::vector<std::string> bodies;
    int binds = 0;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

private:
    class Channel final : public ui::ModelChannel {
    public:
        Channel(ServerDispatcher& owner, std::uint64_t instance) : _dispatcher{&owner}, _instance{instance} {}

        [[nodiscard]] morph::async::Completion<std::string> execute(std::string const& actionId,
                                                                    std::string const& bodyJson,
                                                                    ui::CallOptions /*options*/) override {
            _dispatcher->calls.push_back(actionId);
            _dispatcher->bodies.push_back(bodyJson);
            auto const reply = _dispatcher->_server->call(_instance, actionId, bodyJson, _dispatcher->_person);
            auto [completion, promise] = morph::async::Completion<std::string>::makeSettleable(_dispatcher->_owner);
            if (reply.kind == "ok") {
                promise.resolve(reply.body);
            } else {
                promise.reject(std::make_exception_ptr(std::runtime_error{reply.kind + ": " + reply.message}));
            }
            return std::move(completion);
        }

    private:
        ServerDispatcher* _dispatcher;
        std::uint64_t _instance;
    };

    StateServer* _server;
    morph::exec::IExecutor* _owner;
    std::string _person;
};

// Admits every call but `morph.tableState`'s `publish` for the token `bob`; the token is the verified principal.
struct NoPublishForBob final : morph::session::IAuthorizer {
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): IAuthorizer's signature.
    [[nodiscard]] bool authorize(morph::session::Context const& ctx, std::string_view modelType,
                                 std::string_view actionType) const override {
        return !(ctx.principal == "bob" && modelType == ui::kTableStateModel && actionType == "publish");
    }

    [[nodiscard]] std::optional<std::string> authenticate(morph::session::Context const& ctx) const override {
        return ctx.token.empty() ? std::nullopt : std::optional<std::string>{ctx.token};
    }
};

// A session's owner and timers, and the preferences-backed store over an in-process server.
struct StoreFixture : RowsFixture {
    explicit StoreFixture(TableStateOptions options = {}, std::shared_ptr<morph::session::IAuthorizer> authorizer =
                                                              std::make_shared<prefs_test::TokenAuthorizer>())
        : server{std::move(options), std::move(authorizer)} {}

    StateServer server;
    ProbePreferencesStore& prefs = *server.prefs;
    ServerDispatcher client{server, owner, kPerson};
    ui::PreferencesTableStateStore store{client, owner};
};

// Settles @p completion on the fixture's owner and returns its value, or rethrows its failure.
template <typename T>
T settled(RowsFixture& h, morph::async::Completion<T> completion) {
    std::optional<T> value;
    std::exception_ptr error;
    completion.then([&value](T const& got) { value = got; }).onError([&error](std::exception_ptr const& failure) {
        error = failure;
    });
    h.settle();
    if (error) {
        std::rethrow_exception(error);
    }
    REQUIRE(value.has_value());
    return std::move(*value);
}

table::SaveResult save(StoreFixture& h, table::TableStateKey const& key, std::string json) {
    return settled(h, h.store.save(key, std::move(json)));
}

bool trySave(StoreFixture& h, table::TableStateKey const& key, std::string json) {
    try {
        return save(h, key, std::move(json)).outcome == table::SaveOutcome::Saved;
    } catch (std::exception const&) {
        return false;
    }
}

std::optional<table::StoredTableState> load(StoreFixture& h, table::TableStateKey const& key) {
    return settled(h, h.store.load(key));
}

bool remove(StoreFixture& h, table::TableStateKey const& key) { return settled(h, h.store.remove(key)); }

// The layout's names, as the model writes them.
std::string scopeOf(table::TableStateKey const& key) { return ui::detail::tableStateScope(key); }

PrefKey pointOf(table::TableStateKey const& key, std::string point) {
    return PrefKey{.person = kPerson, .app = key.application, .scope = scopeOf(key), .point = std::move(point)};
}

PrefKey pendingPoint(table::TableStateKey const& key) { return pointOf(key, key.part + ".pending"); }

PrefKey usageKey(std::string const& person) {
    return PrefKey{.person = person, .app = "morph.tableState", .scope = "morph.table", .point = "usage"};
}

// The head entry as stored.
ui::detail::StoredStateHead headOf(ProbePreferencesStore const& prefs, table::TableStateKey const& key) {
    ui::detail::StoredStateHead head;
    REQUIRE_FALSE(glz::read<ui::detail::kStoredRead>(head, prefs.value(pointOf(key, key.part))));
    return head;
}

PrefKey segmentPoint(ProbePreferencesStore const& prefs, table::TableStateKey const& key, std::size_t index) {
    return pointOf(key, key.part + "." + headOf(prefs, key).revision + "." + std::to_string(index));
}

// The points the stored head names: itself and its segments.
std::set<std::string> headAndSegmentsOf(ProbePreferencesStore const& prefs, table::TableStateKey const& key) {
    auto const head = headOf(prefs, key);
    std::set<std::string> out{key.part};
    for (std::uint64_t index = 0; index < head.segments.value_or(0); ++index) {
        out.insert(key.part + "." + head.revision + "." + std::to_string(index));
    }
    return out;
}

std::size_t segmentsOf(std::string const& text) { return ui::detail::segmentText(text, 12UZ * 1024).size(); }

// A state whose filter ticks @p values values, each with a quote and a two-byte character to escape and keep whole.
std::string stateWithOneOfValues(int values, std::string_view tag = "a") {
    std::string out = R"({"version":1,"filters":{"columns":{"name":{"include":[{"oneOf":[)";
    for (int at = 0; at < values; ++at) {
        out += at == 0 ? "\"" : ",\"";
        out += std::format(R"(sample {:05} of the \"{}\" study, 12 )", at, tag);
        out += "\xC2\xB5";  // the micro sign, two bytes
        out += "g in a long-named lims batch\"";
    }
    out += "]}]}}}}";
    return out;
}

// The options of a server whose quota is @p bytes.
TableStateOptions quota(std::size_t bytes) {
    TableStateOptions options;
    options.maxBytesPerPerson = bytes;
    return options;
}

// A state of exactly @p bytes bytes.
std::string docOf(std::size_t bytes) {
    std::string const head = R"({"version":1,"view":")";
    std::string const tail = R"("})";
    return head + std::string(bytes - head.size() - tail.size(), 'x') + tail;
}

table::StateDocument docWithSort(std::string column, table::SortDirection direction) {
    table::StateDocument document;
    document.sort = table::SortChain{table::SortKey{.column = std::move(column), .dir = direction}};
    return document;
}

table::StateDocument docWithSortOn(int index) {
    return docWithSort("c" + std::to_string(index), table::SortDirection::Ascending);
}

// Loads @p key through @p states and settles; whether the load delivered.
bool loaded(RowsFixture& h, TableStates& states, table::TableStateKey const& key) {
    bool delivered = false;
    states.load(key)
        .then([&delivered](std::optional<table::StoredTableState> const&) { delivered = true; })
        .onError([](std::exception_ptr const&) {});
    h.settle();
    return delivered;
}

// The answer of a load that must deliver within one settle.
std::optional<table::StoredTableState> loadNow(RowsFixture& h, TableStates& states, table::TableStateKey const& key) {
    std::optional<std::optional<table::StoredTableState>> got;
    states.load(key).then([&got](std::optional<table::StoredTableState> const& state) { got = state; });
    h.settle();
    REQUIRE(got.has_value());
    return *got;
}

}  // namespace

// --- The preferences-backed store and the model --------------------------------------------------------------------

// Mutation: no segments (the whole document in the head: an entry over segmentBytes).
TEST_CASE("table state: state of any size", "[ui][state]") {
    StoreFixture h;
    std::string const big = stateWithOneOfValues(20'000, "a");
    std::string const big2 = stateWithOneOfValues(20'000, "b");
    std::string const big3 = stateWithOneOfValues(20'000, "c");
    REQUIRE(big.size() > std::size_t{1} << 20);  // sent in parts, and read back past the first head
    REQUIRE(save(h, kKey, big).outcome == table::SaveOutcome::Saved);
    CHECK(load(h, kKey)->json == big);
    CHECK(h.prefs.largestValue() <=
          std::size_t{12} * 1024);  // every entry within segmentBytes, a segment's quotes included

    h.prefs.failSetsAfter(h.prefs.setCount() + 1 + segmentsOf(big2));  // the intent and segments go; the head fails
    CHECK_FALSE(trySave(h, kKey, big2));
    CHECK(load(h, kKey)->json == big);       // the previous state still reads
    CHECK(h.prefs.has(pendingPoint(kKey)));  // the stopped save left its intent
    h.prefs.healSets();

    REQUIRE(save(h, kKey, big3).outcome == table::SaveOutcome::Saved);  // the next save cleans up first
    CHECK_FALSE(h.prefs.has(pendingPoint(kKey)));
    CHECK(h.prefs.pointsUnder(scopeOf(kKey)) == headAndSegmentsOf(h.prefs, kKey));  // no stray of big2 or big
    CHECK(load(h, kKey)->json == big3);

    h.prefs.erase(segmentPoint(h.prefs, kKey, 3));
    CHECK_THROWS(load(h, kKey));  // a missing segment is a failed load
}

// Mutation: the intent naming only the new revision (the previous revision's segments stay after a stop at step 5).
TEST_CASE("table state: a save stopped after its head leaves no stray segment behind the next save", "[ui][state]") {
    StoreFixture h;
    std::string const big = stateWithOneOfValues(1'000, "a");
    std::string const big2 = stateWithOneOfValues(1'000, "b");
    REQUIRE(segmentsOf(big) > 1);
    REQUIRE(save(h, kKey, big).outcome == table::SaveOutcome::Saved);
    h.prefs.failRemovesAfter(h.prefs.removeCount());                    // step 5 fails: big's segments stay
    REQUIRE(save(h, kKey, big2).outcome == table::SaveOutcome::Saved);  // the head names big2
    CHECK(load(h, kKey)->json == big2);
    h.prefs.healRemoves();
    REQUIRE(save(h, kKey, R"({"version":1})").outcome == table::SaveOutcome::Saved);  // unsegmented
    CHECK(h.prefs.pointsUnder(scopeOf(kKey)) == std::set<std::string>{"state"});
    CHECK(load(h, kKey)->json == R"({"version":1})");
}

// Mutation: the bounds "1 to 256" on every member (a rows table's key, model "", is refused).
TEST_CASE("table state: a key's model may be empty, its table may not", "[ui][state]") {
    StoreFixture h;
    auto const withModel = [](std::string model) {
        auto key = kKey;
        key.model = std::move(model);
        return key;
    };
    auto const withTable = [](std::string tableId) {
        auto key = kKey;
        key.table = std::move(tableId);
        return key;
    };
    CHECK(save(h, withModel(""), "{}").outcome == table::SaveOutcome::Saved);
    CHECK(load(h, withModel(""))->json == "{}");
    CHECK_THROWS(save(h, withTable(""), "{}"));
    CHECK_THROWS(save(h, withModel(std::string(257, 'm')), "{}"));
    CHECK(save(h, withModel(std::string(256, 'm')), "{}").outcome == table::SaveOutcome::Saved);
}

// Mutation: the quota checked against the stored total plus the new size (a replace of a large state is refused).
TEST_CASE("table state: a save over the quota is refused and the previous state still reads", "[ui][state]") {
    StoreFixture h{quota(10'000)};
    auto const keyA = keyOf("a");
    auto const keyB = keyOf("b");
    auto const keyC = keyOf("c");
    REQUIRE(save(h, keyA, docOf(6'000)).outcome == table::SaveOutcome::Saved);
    REQUIRE(save(h, keyA, docOf(9'000)).outcome == table::SaveOutcome::Saved);  // a replace: 9,000 in use
    auto const over = save(h, keyB, docOf(2'000));
    CHECK(over.outcome == table::SaveOutcome::Refused);
    CHECK(over.reason == "over the table-state quota");
    CHECK_FALSE(load(h, keyB).has_value());
    CHECK(load(h, keyA)->json == docOf(9'000));
    CHECK(h.prefs.value(usageKey(kPerson)) == R"({"bytes":9000})");  // 6,000 replaced, not added
    CHECK(remove(h, keyA));
    CHECK(h.prefs.value(usageKey(kPerson)) == R"({"bytes":0})");
    CHECK(save(h, keyB, docOf(2'000)).outcome == table::SaveOutcome::Saved);     // 9,000 freed
    REQUIRE(save(h, keyC, docOf(8'000)).outcome == table::SaveOutcome::Saved);   // 10,000: at the quota is allowed
    CHECK(save(h, keyC, docOf(20'000)).outcome == table::SaveOutcome::Refused);  // 10,000 - 8,000 + 20,000
    CHECK(h.prefs.value(usageKey(kPerson)) == R"({"bytes":10000})");             // a refusal writes nothing
    CHECK(load(h, keyC)->json == docOf(8'000));
}

// Mutation: the replaced size of a segmented head read as its entry's length (the replace is refused).
TEST_CASE("table state: the quota counts a segmented document by its bytes", "[ui][state]") {
    StoreFixture h{quota(40'000)};
    REQUIRE(segmentsOf(docOf(30'000)) == 3);
    REQUIRE(save(h, kKey, docOf(30'000)).outcome == table::SaveOutcome::Saved);
    CHECK(save(h, kKey, docOf(35'000)).outcome == table::SaveOutcome::Saved);  // 30,000 - 30,000 + 35,000
    CHECK(h.prefs.value(usageKey(kPerson)) == R"({"bytes":35000})");
}

// Mutation: usage written without the option set.
TEST_CASE("table state: no quota, no usage entry", "[ui][state]") {
    StoreFixture h;
    REQUIRE(save(h, kKey, docOf(6'000)).outcome == table::SaveOutcome::Saved);
    REQUIRE(remove(h, kKey));
    REQUIRE(save(h, kKey, docOf(6'000)).outcome == table::SaveOutcome::Saved);
    CHECK_FALSE(h.prefs.has(usageKey(kPerson)));
}

// Mutation: the model keying by a body member (alice's load naming bob reads bob's state).
TEST_CASE("table state: each person reads and writes their own state", "[ui][state]") {
    StoreFixture h;
    REQUIRE(save(h, kKey, R"({"version":1,"view":"alice's"})").outcome == table::SaveOutcome::Saved);
    ServerDispatcher bobClient{h.server, h.owner, "bob"};
    ui::PreferencesTableStateStore bobStore{bobClient, h.owner};
    REQUIRE(settled(h, bobStore.save(kKey, R"({"version":1,"view":"bob's"})")).outcome == table::SaveOutcome::Saved);
    CHECK(load(h, kKey)->json == R"({"version":1,"view":"alice's"})");
    CHECK(settled(h, bobStore.load(kKey))->json == R"({"version":1,"view":"bob's"})");

    // A body naming another person is read with that member ignored.
    auto const instance = h.server.bind();
    std::string const keyJson = R"({"application":"lims","model":"Lims_SampleModel","table":"samples",)"
                                R"("qualifier":"samples","part":"state"})";
    auto const reply = h.server.call(instance, "load", R"({"keys":[)" + keyJson + R"(],"person":"bob"})", kPerson);
    REQUIRE(reply.kind == "ok");
    auto const heads = morph::model::ActionTraits<ui::TableStateLoad>::resultFromJson(reply.body);
    REQUIRE(heads.entries.size() == 1);
    CHECK(heads.entries.front().text == R"({"version":1,"view":"alice's"})");

    // No verified person: nothing is found, and nothing is written.
    auto const anonymous = h.server.call(instance, "load", R"({"keys":[)" + keyJson + "]}", "");
    REQUIRE(anonymous.kind == "ok");
    CHECK_FALSE(morph::model::ActionTraits<ui::TableStateLoad>::resultFromJson(anonymous.body).entries.front().found);
    CHECK(h.server.call(instance, "save", R"({"key":)" + keyJson + R"(,"text":"{}"})", "").kind != "ok");
}

// --- The session's kept states
// ---------------------------------------------------------------------------------------

// Review: a save during a held load. Mutation: the change saved over an empty document at once.
TEST_CASE("table state: a change during a held load keeps the stored parts", "[ui][state]") {
    RowsFixture h;
    GatedTableStateStore gated{h.owner,
                               R"({"version":1,"filters":{"columns":{"name":{"include":[{"contains":"a"}]}}}})"};
    TableStates states{sharedOf(gated), h.owner, &h.scheduler, 500ms};
    std::optional<table::StoredTableState> first;
    states.load(kKey).then([&first](std::optional<table::StoredTableState> const& state) { first = state; });
    states.changed(kKey, docWithSort("name", table::SortDirection::Descending), StateParts::Sort);
    h.advance(600ms);
    CHECK(gated.saves() == 0);  // held until the load settles
    gated.release();
    h.advance(600ms);
    REQUIRE(gated.saves() == 1);
    CHECK(gated.loads() == 1);
    CHECK(gated.lastSaved().contains(R"("contains":"a")"));  // the stored filter carried
    CHECK(gated.lastSaved().contains(R"("column":"name")"));
    CHECK(first.has_value());
}

// Review Focus 3, the store's half. Mutation: merging the change into the stored text (the save still holds "junk").
TEST_CASE("table state: a change over a stored document that does not read replaces it", "[ui][state]") {
    for (std::string const& stored : {std::string{"{not json"}, std::string{R"({"version":2,"junk":true})"}}) {
        CAPTURE(stored);
        RowsFixture h;
        GatedTableStateStore gated{h.owner, stored};
        TableStates states{sharedOf(gated), h.owner, &h.scheduler, 500ms};
        gated.release();
        REQUIRE(loaded(h, states, kKey));
        h.advance(600ms);
        CHECK(gated.saves() == 0);  // nothing is written until a change
        states.changed(kKey, docWithSort("name", table::SortDirection::Descending), StateParts::Sort);
        h.advance(500ms);
        REQUIRE(gated.saves() == 1);
        CHECK_FALSE(gated.lastSaved().contains("junk"));
        auto const saved = table::readState(gated.lastSaved());  // the framework's version
        REQUIRE(saved.has_value());
        CHECK(saved->version == table::kStateVersion);
        REQUIRE(saved->sort.has_value());
        CHECK(saved->sort->at(0).column == "name");
    }
}

// Mutation: no call of the handler for a Refused save (`failures` stays empty).
TEST_CASE("table state: a save that is refused reaches the session's handler and keeps the kept copy", "[ui][state]") {
    RowsFixture h;
    GatedTableStateStore gated{h.owner, R"({"version":1})"};
    TableStates states{sharedOf(gated), h.owner, &h.scheduler, 500ms};
    std::vector<table::SaveResult> failures;
    states.onSaveFailed(
        [&failures](table::TableStateKey const&, table::SaveResult const& result) { failures.push_back(result); });
    gated.release();
    REQUIRE(loaded(h, states, kKey));
    gated.refuseSaves("over the table-state quota");
    states.changed(kKey, docWithSort("name", table::SortDirection::Descending), StateParts::Sort);
    h.advance(500ms);
    REQUIRE(failures.size() == 1);
    CHECK(failures.front().outcome == table::SaveOutcome::Refused);
    CHECK(failures.front().reason == "over the table-state quota");
    REQUIRE(states.kept(kKey).has_value());
    CHECK(states.kept(kKey)->json == R"({"version":1})");  // what the store still holds
}

// Mutation: keep() not replacing the kept copy (the remount restores the loaded document).
TEST_CASE("table state: a kept copy replaced by keep() is what the next load answers", "[ui][state]") {
    RowsFixture h;
    GatedTableStateStore store{h.owner, R"({"version":1})"};
    TableStates states{sharedOf(store), h.owner, &h.scheduler, 500ms};
    store.release();
    REQUIRE(loaded(h, states, kKey));
    states.keep(kKey, table::StoredTableState{.json = table::writeState(docWithSortOn(7)), .revision = "9"});
    auto const again = loadNow(h, states, kKey);  // no store call
    CHECK(store.loads() == 1);
    REQUIRE(again.has_value());
    CHECK(again->revision == "9");
    CHECK(&states.store() == &store);
}

// Mutation: each table's load sent on its own (five calls). Gate T5.
TEST_CASE("table state: five tables of a screen load in one call, and a second mount in none", "[ui][state]") {
    StoreFixture h;
    REQUIRE(save(h, keyOf("t1"), R"({"version":1,"view":"stored"})").outcome == table::SaveOutcome::Saved);
    TableStates states{sharedOf(h.store), h.owner, &h.scheduler, 500ms};
    auto const mountScreen = [&] {
        std::vector<std::optional<table::StoredTableState>> got;
        for (int at = 1; at <= 5; ++at) {
            states.load(keyOf("t" + std::to_string(at)))
                .then([&got](std::optional<table::StoredTableState> const& state) { got.push_back(state); });
        }
        h.settle();
        return got;
    };
    auto const first = mountScreen();
    CHECK(h.client.count("load") == 1);
    REQUIRE(first.size() == 5);
    REQUIRE(first.front().has_value());
    CHECK(first.front()->json == R"({"version":1,"view":"stored"})");
    CHECK(std::ranges::count_if(first, [](auto const& state) { return state.has_value(); }) == 1);
    auto const second = mountScreen();  // the remount
    CHECK(h.client.count("load") == 1);
    REQUIRE(second.size() == 5);
    REQUIRE(second.front().has_value());
    CHECK(second.front()->revision == first.front()->revision);
    CHECK(h.client.binds == 1);
}

// Mutation: no debounce (fifty saves). Gate T4.
TEST_CASE("table state: fifty changes within a second are one save of the last", "[ui][state]") {
    RowsFixture h;
    GatedTableStateStore store{h.owner, R"({"version":1})"};
    TableStates states{sharedOf(store), h.owner, &h.scheduler, 500ms};
    store.release();
    REQUIRE(loaded(h, states, kKey));
    for (int at = 0; at < 50; ++at) {
        states.changed(kKey, docWithSortOn(at), StateParts::Sort);
        h.advance(10ms);
    }
    h.advance(500ms);
    CHECK(store.saves() == 1);
    CHECK(table::readState(store.lastSaved())->sort == docWithSortOn(49).sort);
    states.changed(kKey, docWithSortOn(50), StateParts::Sort);
    states.flush(kKey);  // an unmount saves at once
    h.settle();
    CHECK(store.saves() == 2);
}

// A change while a save is in flight is one more save after it. Mutation: no save after the one in flight.
TEST_CASE("table state: a change during a save in flight is one more save", "[ui][state]") {
    RowsFixture h;
    GatedTableStateStore store{h.owner, R"({"version":1})"};
    TableStates states{sharedOf(store), h.owner, nullptr, 500ms};  // no scheduler: every change saves at once
    store.release();
    REQUIRE(loaded(h, states, kKey));
    states.changed(kKey, docWithSortOn(1), StateParts::Sort);
    states.changed(kKey, docWithSortOn(2), StateParts::Sort);  // the first save is still in flight
    states.changed(kKey, docWithSortOn(3), StateParts::Sort);
    CHECK(store.saves() == 1);
    h.settle();
    CHECK(store.saves() == 2);
    CHECK(table::readState(store.lastSaved())->sort == docWithSortOn(3).sort);
    REQUIRE(states.kept(kKey).has_value());
    CHECK(states.kept(kKey)->json == store.lastSaved());
}

// A session with a store builds its table states; one without has none. Mutation: no table states built.
TEST_CASE("table state: a session's table states follow its store", "[ui][state]") {
    RowsFixture h;
    CHECK(h.session.tableStates() == nullptr);
    GatedTableStateStore store{h.owner, {}};
    ui::SessionOptions options;
    options.scheduler = &h.scheduler;
    options.tableState = sharedOf(store);
    ui::Session const withStore{h.runtime, std::move(options)};
    REQUIRE(withStore.tableStates() != nullptr);
    CHECK(&withStore.tableStates()->store() == &store);
    CHECK(withStore.options().tableStateTimeout == 300ms);
    CHECK(withStore.options().tableStateDebounce == 500ms);
}

// --- Saved views: kNoDocument, and the shared part ------------------------------------------------------------------

namespace {

table::TableStateKey partOf(std::string part) {
    auto key = kKey;
    key.part = std::move(part);
    return key;
}

table::AddView addNamed(std::string name) {
    return table::AddView{table::SavedView{.name = std::move(name), .layout = {}, .sort = {}, .filters = {}}};
}

std::vector<std::string> viewNames(std::optional<table::StoredTableState> const& stored) {
    REQUIRE(stored.has_value());
    auto const document = table::readViews(stored->json);
    REQUIRE(document.has_value());
    std::vector<std::string> names;
    for (auto const& view : document->views) {
        names.push_back(view.name);
    }
    return names;
}

// The body of a raw `publish` of @p operation on the shared part.
std::string publishBody(table::ViewOperation const& operation, std::string expected) {
    return morph::model::ActionTraits<ui::TableStatePublish>::toJson(ui::TableStatePublish{
        .key = partOf("shared"), .operation = table::writeViewOperation(operation), .expected = std::move(expected)});
}

bool isRevision(std::string const& revision) {
    return revision.size() == 16 && std::ranges::all_of(revision, [](char each) {
               return (each >= '0' && each <= '9') || (each >= 'a' && each <= 'f');
           });
}

}  // namespace

// Mutation: publish written under the caller's person (bob does not see it).
TEST_CASE("shared views: published by one principal, read by another", "[ui][table][state][remote]") {
    StoreFixture h;
    CHECK(h.store.shares());
    auto const shared = partOf("shared");
    auto const published = settled(h, h.store.publish(shared, addNamed("Mine"), std::string{table::kNoDocument}));
    REQUIRE(published.outcome == table::SaveOutcome::Saved);
    CHECK(isRevision(published.revision));
    std::size_t const before = h.prefs.calls().size();
    ServerDispatcher bobClient{h.server, h.owner, "bob"};
    ui::PreferencesTableStateStore bobStore{bobClient, h.owner};
    auto const read = settled(h, bobStore.load(shared));
    REQUIRE(read.has_value());
    CHECK(read->revision == published.revision);
    CHECK(viewNames(read) == std::vector<std::string>{"Mine"});
    auto const calls = h.prefs.calls();
    CHECK(std::ranges::contains(calls, std::string{R"(set "" shared)"}));
    CHECK_FALSE(std::ranges::any_of(calls, [](std::string const& call) { return call.contains(R"("alice")"); }));
    REQUIRE(calls.size() > before);
    CHECK(calls[before] == R"(get "" shared)");  // bob's read
}

// Mutation: the model treating kNoDocument as an ordinary revision in `save` (the first save answers Conflict).
TEST_CASE("table state model: a save with kNoDocument stores over nothing and conflicts over something",
          "[ui][table][state][remote]") {
    StoreFixture h;
    auto const views = partOf("views");
    std::string const first = R"({"version":1,"views":[{"name":"first"}]})";
    auto const saved = settled(h, h.store.save(views, first, std::string{table::kNoDocument}));
    REQUIRE(saved.outcome == table::SaveOutcome::Saved);
    auto const again = settled(
        h, h.store.save(views, R"({"version":1,"views":[{"name":"second"}]})", std::string{table::kNoDocument}));
    CHECK(again.outcome == table::SaveOutcome::Conflict);
    CHECK(again.revision == saved.revision);
    auto const stored = load(h, views);
    REQUIRE(stored.has_value());
    CHECK(stored->json == first);
}

// Mutation: the client sending a publish under the action id `save` (the authoriser, consulted with `save`, admits
// bob's call, and alice's publish is refused as a save of the shared part).
TEST_CASE("shared views: an authoriser that refuses publish refuses it by action id", "[ui][table][state][remote]") {
    StoreFixture h{{}, std::make_shared<NoPublishForBob>()};
    auto const shared = partOf("shared");
    ServerDispatcher bobClient{h.server, h.owner, "bob"};
    ui::PreferencesTableStateStore bobStore{bobClient, h.owner};
    std::size_t const before = h.prefs.setCount();
    CHECK_THROWS(settled(h, bobStore.publish(shared, addNamed("Bob's"), std::string{table::kNoDocument})));
    CHECK(h.prefs.setCount() == before);
    auto const alice = settled(h, h.store.publish(shared, addNamed("Alice's"), std::string{table::kNoDocument}));
    CHECK(alice.outcome == table::SaveOutcome::Saved);
    CHECK(viewNames(load(h, shared)) == std::vector<std::string>{"Alice's"});
}

// Mutation: save of part "shared" accepted.
TEST_CASE("shared views: a user's own save of the shared part is refused", "[ui][table][state][remote]") {
    StoreFixture h;
    auto const instance = h.server.bind();
    std::size_t const before = h.prefs.setCount();
    auto const reply = h.server.call(instance, "save",
                                     morph::model::ActionTraits<ui::TableStateSave>::toJson(
                                         ui::TableStateSave{.key = partOf("shared"),
                                                            .expected = {},
                                                            .upload = {},
                                                            .offset = 0,
                                                            .text = R"({"version":1,"views":[]})",
                                                            .last = true}),
                                     kPerson);
    REQUIRE(reply.kind == "ok");
    CHECK(reply.body.starts_with(R"({"result":{"outcome":"refused",)"));
    CHECK(reply.body.contains(R"("reason":"the shared views are written by publish")"));
    CHECK(h.prefs.setCount() == before);
    // Nor is a user's remove of it.
    CHECK(h.server
              .call(instance, "remove",
                    morph::model::ActionTraits<ui::TableStateRemove>::toJson(
                        ui::TableStateRemove{.key = partOf("shared")}),
                    kPerson)
              .kind != "ok");
}

// Mutation: publish ignoring `expected`.
TEST_CASE("shared views: a publish over a replaced revision is a conflict", "[ui][table][state][remote]") {
    StoreFixture h;
    auto const instance = h.server.bind();
    auto const publish = [&](table::ViewOperation const& operation, std::string expected) {
        auto const reply = h.server.call(instance, "publish", publishBody(operation, std::move(expected)), kPerson);
        REQUIRE(reply.kind == "ok");
        return reply.body;
    };
    auto const resultOf = [](std::string const& body) {
        return morph::model::ActionTraits<ui::TableStatePublish>::resultFromJson(body).result;
    };
    auto const first = resultOf(publish(addNamed("A"), std::string{table::kNoDocument}));
    REQUIRE(first.outcome == table::SaveOutcome::Saved);
    auto const second = resultOf(publish(addNamed("B"), first.revision));
    REQUIRE(second.outcome == table::SaveOutcome::Saved);
    CHECK(second.revision != first.revision);
    std::string const stale = publish(addNamed("C"), first.revision);
    CHECK(stale.starts_with(R"({"result":{"outcome":"conflict","revision":")" + second.revision + "\""));
    CHECK(viewNames(load(h, partOf("shared"))) == std::vector<std::string>{"A", "B"});
    // The shared document has no default, and a caller with no verified person publishes nothing.
    CHECK(resultOf(publish(table::SetDefaultView{"A"}, {})).outcome == table::SaveOutcome::Refused);
    CHECK(h.server.call(instance, "publish", publishBody(addNamed("D"), {}), "").kind != "ok");
}

// Mutation: every state written as segments.  (T2 gates)
TEST_CASE(
    "table state: a full state is one entry and one call; a 20,000-value filter reads back equal through segments",
    "[ui][table][state][remote]") {
    table::StateDocument full;
    table::ColumnLayoutValue layout;
    for (int at = 0; at < 300; ++at) {
        layout.order.push_back(std::format("c{:03}", at));
        layout.widths.emplace(std::format("c{:03}", at), 12 + (at % 20));
    }
    layout.pinned = 1;
    full.layout = layout;
    table::SortChain sort;
    for (int at = 0; at < 8; ++at) {
        sort.push_back(
            table::SortKey{.column = std::format("c{:03}", at * 3), .dir = table::SortDirection::Ascending});
    }
    full.sort = sort;
    table::FilterSpec filters;
    for (int at = 0; at < 32; ++at) {
        table::ColumnFilter filter;
        filter.include.push_back(table::FilterEntry{.contains = "ka"});
        filters.columns.emplace(std::format("c{:03}", 100 + at), std::move(filter));
    }
    full.filters = filters;
    std::string const fullText = table::writeState(full);
    std::string const ticked = stateWithOneOfValues(20'000);
    for (std::string const& part : {std::string{"state"}, std::string{"views"}}) {
        CAPTURE(part);
        StoreFixture h;
        auto const key = partOf(part);
        std::size_t const sets = h.prefs.setCount();
        REQUIRE(save(h, key, fullText).outcome == table::SaveOutcome::Saved);
        INFO("T2: a state of 300 columns, 8 sort keys and 32 filters is " << fullText.size() << " bytes");
        CHECK(h.prefs.setCount() - sets == 1);
        CHECK(h.prefs.pointsUnder(scopeOf(key)) == std::set<std::string>{part});
        auto const stored = load(h, key);
        REQUIRE(stored.has_value());
        CHECK(stored->json == fullText);

        StoreFixture big;
        REQUIRE(save(big, key, ticked).outcome == table::SaveOutcome::Saved);
        INFO("T2: a 20,000-value filter is " << ticked.size() << " bytes in " << segmentsOf(ticked)
                                             << " segments; entries " << big.prefs.pointsUnder(scopeOf(key)).size()
                                             << ", sets " << big.prefs.setCount());
        auto const bigStored = load(big, key);
        REQUIRE(bigStored.has_value());
        CHECK(bigStored->json == ticked);
        CHECK(big.prefs.largestValue() <= std::size_t{12} * 1024);
    }
}

// --- Bounds on what one caller can make the server hold, and one key's problems kept to itself ---------------------

namespace {

// A server whose every task runs on a step executor: a test decides what runs when, so two calls can be interleaved
// at a chosen store call.
struct SteppedServer {
    explicit SteppedServer(TableStateOptions options = {}) {
        ui::registerTableState(prefs, dispatcher, registry, std::move(options));
        server = std::make_shared<morph::backend::RemoteServer>(pool, std::make_shared<prefs_test::TokenAuthorizer>(),
                                                                dispatcher, registry);
    }

    // Sends @p request; @p into receives the reply when the step executor gets to it.
    void post(morph::wire::Envelope const& request, std::optional<morph::wire::Envelope>& into) {
        server->handle(morph::wire::encode(request),
                       [&into](std::string const& text) { into = morph::wire::decode(text); });
    }

    morph::wire::Envelope sendNow(morph::wire::Envelope const& request) {
        std::optional<morph::wire::Envelope> reply;
        post(request, reply);
        pool.runAll();
        REQUIRE(reply.has_value());
        return *reply;
    }

    std::uint64_t bind() {
        auto const reply = sendNow(morph::wire::makeRegister(std::string{ui::kTableStateModel}));
        REQUIRE(reply.kind == "ok");
        return reply.modelId;
    }

    morph::wire::Envelope request(std::uint64_t instance, std::string_view action, std::string body) {
        morph::wire::Envelope out;
        out.kind = "execute";
        out.callId = ++lastCallId;
        out.modelId = instance;
        out.modelType = std::string{ui::kTableStateModel};
        out.actionType = std::string{action};
        out.body = std::move(body);
        out.session.token = kPerson;
        return out;
    }

    morph::testing::StepExecutor pool;
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    std::shared_ptr<ProbePreferencesStore> prefs = std::make_shared<ProbePreferencesStore>(pool);
    std::shared_ptr<morph::backend::RemoteServer> server;
    std::uint64_t lastCallId = 0;
};

std::string saveBody(table::TableStateKey const& key, std::string text, std::string upload = {},
                     std::uint64_t offset = 0, bool last = true) {
    return morph::model::ActionTraits<ui::TableStateSave>::toJson(ui::TableStateSave{.key = key,
                                                                                     .expected = {},
                                                                                     .upload = std::move(upload),
                                                                                     .offset = offset,
                                                                                     .text = std::move(text),
                                                                                     .last = last});
}

std::string loadBody(std::vector<table::TableStateKey> keys) {
    return morph::model::ActionTraits<ui::TableStateLoad>::toJson(ui::TableStateLoad{.keys = std::move(keys)});
}

ui::TableStateLoaded loadedOf(morph::wire::Envelope const& reply) {
    REQUIRE(reply.kind == "ok");
    return morph::model::ActionTraits<ui::TableStateLoad>::resultFromJson(reply.body);
}

table::SaveResult savedOf(morph::wire::Envelope const& reply) {
    REQUIRE(reply.kind == "ok");
    return morph::model::ActionTraits<ui::TableStateSave>::resultFromJson(reply.body).result;
}

// A state of about @p bytes bytes whose view is escaped quotes: four bytes on the wire for each of its bytes.
std::string escapedState(std::size_t bytes) {
    std::string text = R"({"version":1,"view":")";
    while (text.size() + 2 < bytes) {
        text += R"(\")";
    }
    return text + R"("})";
}

// Whether @p text is well-formed UTF-8.
bool wellFormed(std::string_view text) {
    for (std::size_t at = 0; at < text.size();) {
        auto const lead = static_cast<unsigned char>(text[at]);
        std::size_t const length =
            lead < 0x80 ? 1
            // NOLINTBEGIN(readability-avoid-nested-conditional-operator): a flat pick of the UTF-8 sequence length from the lead byte, one range per line
            : (lead >> 5) == 0x6  ? 2
            : (lead >> 4) == 0xE  ? 3
            : (lead >> 3) == 0x1E ? 4
                                  : 0;
        // NOLINTEND(readability-avoid-nested-conditional-operator)
        if (length == 0 || at + length > text.size()) {
            return false;
        }
        for (std::size_t each = at + 1; each < at + length; ++each) {
            if ((static_cast<unsigned char>(text[each]) & 0xC0U) != 0x80U) {
                return false;
            }
        }
        at += length;
    }
    return true;
}

}  // namespace

// Mutation: the usage write unsaturated, `used - replaced + added` (it wraps to nearly 2^64).
TEST_CASE("table state: a quota set over existing states counts a replace from nothing", "[ui][state]") {
    StoreFixture h{quota(100'000)};
    REQUIRE(save(h, kKey, docOf(9'000)).outcome == table::SaveOutcome::Saved);
    h.prefs.erase(usageKey(kPerson));  // the state was stored before the operator set the quota
    REQUIRE(save(h, kKey, docOf(4'000)).outcome == table::SaveOutcome::Saved);
    CHECK(h.prefs.value(usageKey(kPerson)) == R"({"bytes":4000})");
    CHECK(save(h, keyOf("b"), docOf(2'000)).outcome == table::SaveOutcome::Saved);  // nothing is locked out
}

// Mutation: the usage write unsaturated (the replace after the race wraps).
TEST_CASE("table state: two instances racing on the quota undercount, and a later replace does not wrap",
          "[ui][state]") {
    SteppedServer s{quota(100'000)};
    auto const a = s.bind();
    auto const b = s.bind();
    s.prefs->holdSet(s.prefs->setCount() + 1);  // A has read the usage; its head is held
    std::optional<morph::wire::Envelope> replyA;
    s.post(s.request(a, "save", saveBody(keyOf("a"), docOf(2'000))), replyA);
    s.pool.runAll();
    REQUIRE(s.prefs->holding());
    REQUIRE(savedOf(s.sendNow(s.request(b, "save", saveBody(keyOf("b"), docOf(6'000))))).outcome ==
            table::SaveOutcome::Saved);
    CHECK(s.prefs->value(usageKey(kPerson)) == R"({"bytes":6000})");
    s.prefs->releaseHeld();
    s.pool.runAll();
    REQUIRE(replyA.has_value());
    REQUIRE(savedOf(*replyA).outcome == table::SaveOutcome::Saved);
    CHECK(s.prefs->value(usageKey(kPerson)) == R"({"bytes":2000})");  // both read 0: B's bytes went uncounted
    REQUIRE(savedOf(s.sendNow(s.request(b, "save", saveBody(keyOf("b"), docOf(1'000))))).outcome ==
            table::SaveOutcome::Saved);
    CHECK(s.prefs->value(usageKey(kPerson)) == R"({"bytes":1000})");
}

// Mutation: the usage write unsaturated (a usage left short by a refused write wraps on the next replace).
TEST_CASE("table state: a refused usage write leaves a usage the next replace does not wrap", "[ui][state]") {
    StoreFixture h{quota(100'000)};
    REQUIRE(save(h, keyOf("b"), docOf(500)).outcome == table::SaveOutcome::Saved);
    h.prefs.failSetsAfter(h.prefs.setCount() + 1);  // the head is written; the usage is not
    REQUIRE(save(h, kKey, docOf(6'000)).outcome == table::SaveOutcome::Saved);
    h.prefs.healSets();
    CHECK(h.prefs.value(usageKey(kPerson)) == R"({"bytes":500})");
    REQUIRE(save(h, kKey, docOf(1'000)).outcome == table::SaveOutcome::Saved);
    CHECK(h.prefs.value(usageKey(kPerson)) == R"({"bytes":1000})");
}

// Mutation: the quota counting a small document by the text read back (" {}" reads as "{}", and usage drifts to 3).
TEST_CASE("table state: the quota counts a small document by the text its save received", "[ui][state]") {
    StoreFixture h{quota(1'000)};
    REQUIRE(save(h, kKey, " {}").outcome == table::SaveOutcome::Saved);
    CHECK(h.prefs.value(usageKey(kPerson)) == R"({"bytes":3})");
    REQUIRE(save(h, kKey, "{}").outcome == table::SaveOutcome::Saved);
    CHECK(h.prefs.value(usageKey(kPerson)) == R"({"bytes":2})");
}

// Mutation: no cap on a person's open uploads (a fifth starts, and the server holds five).
TEST_CASE("table state: a person's open uploads are capped, a fifth refused until one ends", "[ui][state]") {
    StoreFixture h;
    auto const instance = h.server.bind();
    std::string const head = R"({"version":1,)";
    auto const start = [&](int at) {
        return savedOf(
            h.server.call(instance, "save", saveBody(kKey, head, "u" + std::to_string(at), 0, false), kPerson));
    };
    auto const finish = [&](int at) {
        return h.server.call(instance, "save",
                             saveBody(kKey, R"("view":"x"})", "u" + std::to_string(at), head.size(), true), kPerson);
    };
    for (int at = 1; at <= 4; ++at) {
        CHECK(start(at).outcome == table::SaveOutcome::Saved);
    }
    auto const fifth = start(5);  // no client holds more than four open
    CHECK(fifth.outcome == table::SaveOutcome::Refused);
    CHECK(fifth.reason == ui::kTableStateUploadsBusy);
    CHECK(finish(5).kind != "ok");
    for (int at = 1; at <= 4; ++at) {
        CHECK(finish(at).kind == "ok");
    }
    CHECK(start(5).outcome == table::SaveOutcome::Saved);  // one ended: the fifth starts now
    CHECK(finish(5).kind == "ok");
    CHECK(load(h, kKey)->json == R"({"version":1,"view":"x"})");
}

// Mutation: no expiry of uploads (an upload continues after a minute's silence).
TEST_CASE("table state: an upload abandoned past its expiry is dropped", "[ui][state]") {
    auto const clock = std::make_shared<std::chrono::milliseconds>(0);
    TableStateOptions options;
    options.clock = [clock] { return *clock; };
    StoreFixture h{options};
    CHECK(options.uploadExpiry == 30s);
    auto const instance = h.server.bind();
    std::string const head = R"({"version":1,)";
    REQUIRE(h.server.call(instance, "save", saveBody(kKey, head, "late", 0, false), kPerson).kind == "ok");
    REQUIRE(h.server.call(instance, "save", saveBody(kKey, head, "kept", 0, false), kPerson).kind == "ok");
    *clock += 20s;
    REQUIRE(
        h.server.call(instance, "save", saveBody(kKey, R"("view":"k")", "kept", head.size(), false), kPerson).kind ==
        "ok");  // a part within the expiry keeps its upload
    *clock += 20s;
    CHECK(h.server.call(instance, "save", saveBody(kKey, R"("view":"x"})", "late", head.size(), true), kPerson).kind !=
          "ok");
    CHECK(h.server.call(instance, "save", saveBody(kKey, "}", "kept", head.size() + 10, true), kPerson).kind == "ok");
}

// Mutation: no quota bound on an upload's parts (a part past the quota is kept until the last arrives).
TEST_CASE("table state: an upload past the quota is refused at the part that passes it", "[ui][state]") {
    StoreFixture h{quota(1'000)};
    auto const instance = h.server.bind();
    CHECK(h.server.call(instance, "save", saveBody(kKey, std::string(1'001, ' '), "big", 0, false), kPerson).kind !=
          "ok");
    CHECK(h.server.call(instance, "save", saveBody(kKey, "{}", "big", 1'001, true), kPerson).kind != "ok");
}

// Mutation: no per-key lock (B writes while A is held, and A's head then names a segment B removed).
TEST_CASE("table state: two segmented saves of one key run one after the other", "[ui][state]") {
    SteppedServer s;
    auto const a = s.bind();
    auto const b = s.bind();
    std::string const first = stateWithOneOfValues(400, "a");
    std::string const docA = stateWithOneOfValues(400, "b");
    std::string const docB = stateWithOneOfValues(400, "c");
    REQUIRE(segmentsOf(docA) > 2);
    REQUIRE(savedOf(s.sendNow(s.request(a, "save", saveBody(kKey, first)))).outcome == table::SaveOutcome::Saved);
    s.prefs->holdSet(s.prefs->setCount() + 3);  // A's intent and first segment go; its second segment is held
    std::optional<morph::wire::Envelope> replyA;
    std::optional<morph::wire::Envelope> replyB;
    s.post(s.request(a, "save", saveBody(kKey, docA)), replyA);
    s.pool.runAll();
    REQUIRE(s.prefs->holding());
    std::size_t const calls = s.prefs->calls().size();
    s.post(s.request(b, "save", saveBody(kKey, docB)), replyB);
    s.pool.runAll();
    CHECK_FALSE(replyB.has_value());          // B waits for A's key
    CHECK(s.prefs->calls().size() == calls);  // and has touched nothing
    s.prefs->releaseHeld();
    s.pool.runAll();
    REQUIRE(replyA.has_value());
    REQUIRE(replyB.has_value());
    CHECK(savedOf(*replyA).outcome == table::SaveOutcome::Saved);
    CHECK(savedOf(*replyB).outcome == table::SaveOutcome::Saved);
    auto const loaded = loadedOf(s.sendNow(s.request(a, "load", loadBody({kKey}))));
    REQUIRE(loaded.entries.size() == 1);
    CHECK(loaded.entries.front().error.empty());
    CHECK(loaded.entries.front().text == docB);  // the last save wins, whole
    CHECK(s.prefs->pointsUnder(scopeOf(kKey)) == headAndSegmentsOf(*s.prefs, kKey));
}

// Mutation: a key out of bounds failing the whole load (the good key's change is saved over an empty document).
TEST_CASE("table state: a key out of bounds fails its own load, and its neighbour's stored parts stay",
          "[ui][state]") {
    StoreFixture h;
    REQUIRE(save(h, kKey, R"({"version":1,"filters":{"columns":{"name":{"include":[{"contains":"a"}]}}}})").outcome ==
            table::SaveOutcome::Saved);
    auto const loaded =
        loadedOf(h.server.call(h.server.bind(), "load", loadBody({keyOf(std::string(257, 't')), kKey}), kPerson));
    REQUIRE(loaded.entries.size() == 2);
    CHECK_FALSE(loaded.entries[0].error.empty());
    CHECK(loaded.entries[1].found);

    TableStates states{sharedOf(h.store), h.owner, &h.scheduler, 500ms};
    bool badFailed = false;
    bool goodLoaded = false;
    states.load(keyOf(std::string(257, 't')))
        .then([](std::optional<table::StoredTableState> const&) {})
        .onError([&badFailed](std::exception_ptr const&) { badFailed = true; });
    states.load(kKey).then(
        [&goodLoaded](std::optional<table::StoredTableState> const& state) { goodLoaded = state.has_value(); });
    h.settle();
    CHECK(badFailed);
    REQUIRE(goodLoaded);
    states.changed(kKey, docWithSort("name", table::SortDirection::Descending), StateParts::Sort);
    h.advance(500ms);
    auto const stored = load(h, kKey);
    REQUIRE(stored.has_value());
    CHECK(stored->json.contains(R"("contains":"a")"));
    CHECK(stored->json.contains(R"("column":"name")"));
}

// Mutation: no reply budget (every head in one reply, none deferred).
TEST_CASE("table state: a load whose heads would outgrow its envelope defers the rest", "[ui][state]") {
    StoreFixture h;
    std::string const big = escapedState(800UZ * 1024);
    REQUIRE(ui::detail::wireBytes(big) > std::uint64_t{3} * 800 * 1024);
    REQUIRE(save(h, keyOf("t1"), big).outcome == table::SaveOutcome::Saved);
    REQUIRE(save(h, keyOf("t2"), big).outcome == table::SaveOutcome::Saved);
    REQUIRE(save(h, keyOf("t3"), "{}").outcome == table::SaveOutcome::Saved);
    auto const reply =
        h.server.call(h.server.bind(), "load", loadBody({keyOf("t1"), keyOf("t2"), keyOf("t3")}), kPerson);
    CHECK(morph::wire::encode(reply).size() < morph::wire::kMaxEnvelopeBytes);
    auto const loaded = loadedOf(reply);
    REQUIRE(loaded.entries.size() == 3);
    CHECK(loaded.entries[0].text == big);
    CHECK(loaded.entries[1].deferred);
    CHECK(loaded.entries[2].deferred);

    std::vector<std::string> got;
    for (auto const* id : {"t1", "t2", "t3"}) {
        h.store.load(keyOf(id)).then([&got](std::optional<table::StoredTableState> const& state) {
            got.push_back(state ? state->json : std::string{"none"});
        });
    }
    std::size_t const before = h.client.count("load");
    h.settle();
    CHECK(h.client.count("load") - before == 2);  // the second call answers what the first deferred
    CHECK(got.size() == 3);
    CHECK(std::ranges::count(got, big) == 2);
    CHECK(std::ranges::count(got, std::string{"{}"}) == 1);
}

// Mutation: the client sending a turn's loads in one call however many (the server refuses 300 keys).
TEST_CASE("table state: a turn of more loads than one call takes is split", "[ui][state]") {
    StoreFixture h;
    std::size_t answered = 0;
    for (int at = 0; at < 300; ++at) {
        h.store.load(keyOf("t" + std::to_string(at)))
            .then([&answered](std::optional<table::StoredTableState> const&) { ++answered; })
            .onError([](std::exception_ptr const&) {});
    }
    h.settle();
    CHECK(answered == 300);
    CHECK(h.client.count("load") == 2);
}

// Mutation: segments cut by byte, not by UTF-8 sequence (a segment ends inside the micro sign).
TEST_CASE("table state: a segment never ends inside a UTF-8 sequence", "[ui][state]") {
    std::string text(29, 'a');
    text += "\xC2\xB5";  // straddles the first segment's 30-byte budget
    text += std::string(27, 'b');
    text += "\xF0\x9F\x98\x80";  // a four-byte character across the second
    text += "tail";
    auto const segments = ui::detail::segmentText(text, 32);
    REQUIRE(segments.size() > 2);
    std::string whole;
    for (auto const& segment : segments) {
        CHECK(segment.size() <= 32);
        std::string decoded;
        REQUIRE_FALSE(glz::read_json(decoded, segment));
        CHECK(wellFormed(decoded));
        whole += decoded;
    }
    CHECK(whole == text);
}

// Mutation: a document of exactly segmentBytes written as segments (`>=` for `>`).
TEST_CASE("table state: a document of exactly 12 KiB is its head, one byte more is segments", "[ui][state]") {
    StoreFixture h;
    REQUIRE(save(h, kKey, docOf(12UZ * 1024)).outcome == table::SaveOutcome::Saved);
    CHECK(h.prefs.pointsUnder(scopeOf(kKey)) == std::set<std::string>{"state"});
    CHECK(load(h, kKey)->json == docOf(12UZ * 1024));
    REQUIRE(save(h, kKey, docOf((12UZ * 1024) + 1)).outcome == table::SaveOutcome::Saved);
    CHECK(headOf(h.prefs, kKey).segments == std::optional<std::uint64_t>{2});
    CHECK(load(h, kKey)->json == docOf((12UZ * 1024) + 1));
}

// Mutation: no refusal of a `.` in `part` ("state.pending" is stored, over the intent point of "state").
TEST_CASE("table state: a part holding a dot is refused", "[ui][state]") {
    StoreFixture h;
    CHECK_THROWS(save(h, partOf("state.pending"), "{}"));
    CHECK_FALSE(h.prefs.has(pendingPoint(kKey)));
}

// Mutation: the shared views not counted (no usage entry for the person "", and the large publish is saved).
TEST_CASE("shared views: a publish over the operator's quota is refused as a save is", "[ui][table][state][remote]") {
    StoreFixture h{quota(400)};
    auto const shared = partOf("shared");
    REQUIRE(settled(h, h.store.publish(shared, addNamed("Open"), std::string{table::kNoDocument})).outcome ==
            table::SaveOutcome::Saved);
    auto const stored = load(h, shared);
    REQUIRE(stored.has_value());
    CHECK(h.prefs.value(usageKey("")) == std::format(R"({{"bytes":{}}})", stored->json.size()));
    auto const over = settled(h, h.store.publish(shared, addNamed(std::string(400, 'v')), {}));
    CHECK(over.outcome == table::SaveOutcome::Refused);
    CHECK(over.reason == "over the table-state quota");
    CHECK(viewNames(load(h, shared)) == std::vector<std::string>{"Open"});
    CHECK(save(h, kKey, docOf(300)).outcome == table::SaveOutcome::Saved);  // alice's own account is apart
}

// --- Reads overtaken by a save, a lock handed to a waiter that is gone, and five large saves at once ----------------

namespace {

// A coroutine that starts at once and is destroyed by its owner, for driving the per-key locks by hand.
struct Started {
    struct promise_type {
        Started get_return_object() { return Started{std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { std::terminate(); }
    };
    std::coroutine_handle<promise_type> handle;
};

// Takes the lock of @p name, sets @p held, and waits until destroyed.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the test keeps `locks` and `held` alive past the coroutine's suspension, and the coroutine only reads them
Started holdLock(ui::detail::TableStateLocks& locks, std::string name, bool& held) {
    auto const lease = co_await locks.lock(std::move(name));
    held = true;
    co_await std::suspend_always{};
}

std::string readBody(table::TableStateKey const& key, std::string revision) {
    return morph::model::ActionTraits<ui::TableStateRead>::toJson(
        ui::TableStateRead{.key = key, .revision = std::move(revision), .offset = 0});
}

}  // namespace

// Mutation: no second read of the head on a missing segment (the load answers the key's error).
TEST_CASE("table state: a load overtaken by a save answers the new document", "[ui][state]") {
    SteppedServer s;
    auto const writer = s.bind();
    auto const reader = s.bind();
    std::string const first = stateWithOneOfValues(400, "a");
    std::string const next = stateWithOneOfValues(400, "b");
    REQUIRE(savedOf(s.sendNow(s.request(writer, "save", saveBody(kKey, first)))).outcome == table::SaveOutcome::Saved);
    s.prefs->holdGet(s.prefs->getCount() + 2);  // the load has read the head; its first segment read is held
    std::optional<morph::wire::Envelope> reply;
    s.post(s.request(reader, "load", loadBody({kKey})), reply);
    s.pool.runAll();
    REQUIRE(s.prefs->holdingGet());
    REQUIRE(savedOf(s.sendNow(s.request(writer, "save", saveBody(kKey, next)))).outcome ==
            table::SaveOutcome::Saved);  // removes the segments the load's head named
    s.prefs->releaseHeldGet();
    s.pool.runAll();
    REQUIRE(reply.has_value());
    auto const loaded = loadedOf(*reply);
    REQUIRE(loaded.entries.size() == 1);
    CHECK(loaded.entries.front().error.empty());
    CHECK(loaded.entries.front().found);
    CHECK(loaded.entries.front().text == next);
}

// Mutation: no second read of the head on a missing segment in `read` (the read fails instead of answering rB).
TEST_CASE("table state: a read overtaken by a save answers the new revision, to start again", "[ui][state]") {
    SteppedServer s;
    auto const writer = s.bind();
    auto const reader = s.bind();
    REQUIRE(savedOf(s.sendNow(s.request(writer, "save", saveBody(kKey, stateWithOneOfValues(400, "a"))))).outcome ==
            table::SaveOutcome::Saved);
    std::string const before = headOf(*s.prefs, kKey).revision;
    s.prefs->holdGet(s.prefs->getCount() + 2);  // the read has checked the head; its first segment read is held
    std::optional<morph::wire::Envelope> reply;
    s.post(s.request(reader, "read", readBody(kKey, before)), reply);
    s.pool.runAll();
    REQUIRE(s.prefs->holdingGet());
    REQUIRE(savedOf(s.sendNow(s.request(writer, "save", saveBody(kKey, stateWithOneOfValues(400, "b"))))).outcome ==
            table::SaveOutcome::Saved);
    s.prefs->releaseHeldGet();
    s.pool.runAll();
    REQUIRE(reply.has_value());
    REQUIRE(reply->kind == "ok");
    auto const chunk = morph::model::ActionTraits<ui::TableStateRead>::resultFromJson(reply->body);
    CHECK(chunk.revision == headOf(*s.prefs, kKey).revision);
    CHECK(chunk.revision != before);
    CHECK(chunk.text.empty());
}

// Mutation: a waiter handed the lock and destroyed before it runs keeps the lock (the third never takes it).
TEST_CASE("table state: a lock handed to a waiter destroyed before it runs passes on", "[ui][state]") {
    ui::detail::TableStateLocks locks;
    morph::testing::StepExecutor resumer;
    bool first = false;
    bool second = false;
    bool third = false;
    Started const holder = holdLock(locks, "k", first);
    REQUIRE(first);
    Started const waiter = [&] {
        // Queued while running for the step executor, so its turn is posted there rather than run inline.
        ::core::async::ExecutorScope const scope{resumer.coreExecutor()};
        return holdLock(locks, "k", second);
    }();
    CHECK_FALSE(second);
    holder.handle.destroy();  // the lease is released: the lock is handed to the waiter, whose turn is queued
    REQUIRE(resumer.pending() > 0);
    waiter.handle.destroy();  // destroyed before its turn ran
    Started const late = holdLock(locks, "k", third);
    CHECK(third);
    late.handle.destroy();
}

// Mutation: no upper bound on `partBytes` (a model with 2 MiB parts is built).
TEST_CASE("table state: a part size over 1 MiB is refused", "[ui][state]") {
    morph::exec::ThreadPoolExecutor pool{1};
    auto const store = std::make_shared<ui::MemoryPreferencesStore>(pool);
    TableStateOptions options;
    options.partBytes = ui::kTableStatePartBytes * 2;
    CHECK_THROWS(ui::TableStateModel{store, options});
    options.partBytes = ui::kTableStatePartBytes;
    CHECK_NOTHROW(ui::TableStateModel{store, options});
}

// Mutation: a save refused for the open uploads delivered as its result (the fifth answers Refused).
TEST_CASE("table state: five large saves at once are all stored", "[ui][state]") {
    StoreFixture h;
    ui::PreferencesTableStateStore store{h.client, h.owner, &h.scheduler, 300ms};
    std::vector<std::string> docs;
    std::vector<std::optional<table::SaveResult>> results(5);
    for (int at = 0; at < 5; ++at) {
        docs.push_back(stateWithOneOfValues(16'000, std::to_string(at)));
        REQUIRE(docs.back().size() > ui::kTableStatePartBytes);  // two parts each
    }
    for (std::size_t at = 0; at < 5; ++at) {
        store.save(keyOf("t" + std::to_string(at)), docs[at])
            .then([&results, at](table::SaveResult const& result) { results[at] = result; })
            .onError([](std::exception_ptr const&) {});
    }
    h.settle();
    CHECK(std::ranges::count_if(results, [](auto const& result) { return result.has_value(); }) == 4);
    h.advance(300ms);  // the refused one is sent again
    for (std::size_t at = 0; at < 5; ++at) {
        CAPTURE(at);
        REQUIRE(results[at].has_value());
        CHECK(results[at]->outcome == table::SaveOutcome::Saved);
        CHECK(load(h, keyOf("t" + std::to_string(at)))->json == docs[at]);
    }
}

// Mutation: the session not handing its scheduler to the store (the refused save is sent again on the owner's next
// turn, so all five are stored within the first settle).
TEST_CASE("table state: a session's store sends a busy save again after tableStateTimeout", "[ui][state]") {
    StoreFixture h;
    auto const store = std::make_shared<ui::PreferencesTableStateStore>(h.client, h.owner);  // no timers of its own
    ui::Session const session{h.runtime, ui::SessionOptions{.scheduler = &h.scheduler,
                                                            .clock = [&h] { return h.scheduler.now(); },
                                                            .keepUnused = std::chrono::minutes{5},
                                                            .catalog = {},
                                                            .classify = {},
                                                            .developmentTools = ui::DevelopmentTools::Off,
                                                            .watcher = {},
                                                            .maxInlineFileBytes = ui::kDefaultInlineFileBytes,
                                                            .tableSettle = 80ms,
                                                            .tableAwaitedRefetch = 250ms,
                                                            .tableState = store,
                                                            .tableStateTimeout = 300ms,
                                                            .tableStateDebounce = 500ms,
                                                            .application = "lims"}};
    REQUIRE(session.tableStates() != nullptr);
    std::vector<std::optional<table::SaveResult>> results(5);
    for (std::size_t at = 0; at < 5; ++at) {
        std::string doc = stateWithOneOfValues(16'000, std::to_string(at));
        REQUIRE(doc.size() > ui::kTableStatePartBytes);  // two parts each: an upload the server counts as open
        session.tableStates()
            ->store()
            .save(keyOf("t" + std::to_string(at)), std::move(doc))
            .then([&results, at](table::SaveResult const& result) { results[at] = result; })
            .onError([](std::exception_ptr const&) {});
    }
    auto const stored = [&results] {
        return std::ranges::count_if(results, [](auto const& result) { return result.has_value(); });
    };
    h.settle();
    CHECK(stored() == 4);  // the fifth was refused busy and waits
    h.advance(299ms);
    CHECK(stored() == 4);
    h.advance(1ms);  // tableStateTimeout after the refusal
    CHECK(stored() == 5);
    for (auto const& result : results) {
        REQUIRE(result.has_value());
        CHECK(result->outcome == table::SaveOutcome::Saved);
    }
}

// --- Saved views ---------------------------------------------------------------------------------------------------

namespace {

using morph::ui::TableViews;
using morph::ui::testing::LiteralTableStateStore;
using morph::ui::testing::RecordingTableStateStore;
using morph::ui::testing::StoreCall;

// A view of a name, sorted by @p column.
table::SavedView viewNamed(std::string name, std::string column = "name") {
    return table::SavedView{
        .name = std::move(name),
        .layout = {},
        .sort = {table::SortKey{.column = std::move(column), .dir = table::SortDirection::Ascending}},
        .filters = {}};
}

// A window: a session's table states over a shared store, and one table's views.
struct ViewsWindow {
    ViewsWindow(RowsFixture& h, table::ITableStateStore& store)
        : states{sharedOf(store), h.owner, &h.scheduler, 500ms}, views{states, kKey, h.owner} {}
    TableStates states;
    TableViews views;
};

// The calls of a part, as `op expected outcome` lines: what a test compares.
std::vector<std::string> callsOf(std::vector<StoreCall> const& calls, std::string_view part, std::size_t from = 0) {
    std::vector<std::string> out;
    for (std::size_t at = from; at < calls.size(); ++at) {
        StoreCall const& call = calls.at(at);
        if (call.part != part) {
            continue;
        }
        std::string const outcome =
            call.op == "load" ? ""
            // NOLINTNEXTLINE(readability-avoid-nested-conditional-operator): a flat pick over the call's outcome, one case per line
            : call.outcome == table::SaveOutcome::Saved ? " Saved"
            // NOLINTNEXTLINE(readability-avoid-nested-conditional-operator): a flat pick over the call's outcome, one case per line
            : call.outcome == table::SaveOutcome::Conflict ? " Conflict"
                                                           : " Refused";
        out.push_back(call.op + (call.op == "load" ? "" : " expected=" + call.expected) + outcome);
    }
    return out;
}

table::TableStateKey viewsKey() {
    table::TableStateKey key = kKey;
    key.part = "views";
    return key;
}

}  // namespace

// Mutation: a views save with no expected revision (the second window's save overwrites the first's view). The
// state part's last-save-wins clause of the brief is 9c's, tested there.
TEST_CASE("views: views are not lost between windows", "[ui][table][state]") {
    RowsFixture h;
    RecordingTableStateStore store{h.owner};
    auto a = std::make_unique<ViewsWindow>(h, store);
    auto b = std::make_unique<ViewsWindow>(h, store);
    REQUIRE(settled(h, a->views.load()).has_value());
    REQUIRE(settled(h, b->views.load()).has_value());
    std::size_t const before = store.calls.size();
    REQUIRE(settled(h, a->views.change(table::AddView{viewNamed("A")})).has_value());
    REQUIRE(settled(h, b->views.change(table::AddView{viewNamed("B")})).has_value());
    CHECK(callsOf(store.calls, "views", before) ==
          std::vector<std::string>{"save expected=- Saved", "save expected=- Conflict", "load",
                                   "save expected=" + store.calls.at(before).revision + " Saved"});
    std::string const rA = store.calls.at(before).revision;
    std::string const rB = store.calls.back().revision;
    CHECK(store.calls.at(before + 1).revision == rA);  // the conflict carries A's revision
    CHECK(rB != rA);
    CHECK(viewNames(settled(h, store.load(viewsKey()))) == std::vector<std::string>{"A", "B"});
    // New sessions: each reads the document of revision rB, then each adds a view.
    a = std::make_unique<ViewsWindow>(h, store);
    b = std::make_unique<ViewsWindow>(h, store);
    REQUIRE(settled(h, a->views.load()).has_value());
    REQUIRE(settled(h, b->views.load()).has_value());
    std::size_t const again = store.calls.size();
    REQUIRE(settled(h, a->views.change(table::AddView{viewNamed("C")})).has_value());
    REQUIRE(settled(h, b->views.change(table::AddView{viewNamed("D")})).has_value());
    std::string const rC = store.calls.at(again).revision;
    CHECK(callsOf(store.calls, "views", again) == std::vector<std::string>{"save expected=" + rB + " Saved",
                                                                           "save expected=" + rB + " Conflict", "load",
                                                                           "save expected=" + rC + " Saved"});
    CHECK(viewNames(settled(h, store.load(viewsKey()))) == std::vector<std::string>{"A", "B", "C", "D"});
}

// Mutation: TableViews loading through the store directly rather than through TableStates (one call per part).
// Gate T5, extended to the views parts.
TEST_CASE("views: a screen's state, views and shared parts load in one call, and a second mount in none",
          "[ui][table][state]") {
    StoreFixture h;
    REQUIRE(h.store.shares());
    TableStates states{sharedOf(h.store), h.owner, &h.scheduler, 500ms};
    std::vector<std::unique_ptr<TableViews>> held;
    auto const mountScreen = [&] {
        held.clear();
        int answered = 0;
        for (int at = 1; at <= 5; ++at) {
            states.load(keyOf("t" + std::to_string(at)))
                .then([&answered](std::optional<table::StoredTableState> const&) { ++answered; });
            held.push_back(std::make_unique<TableViews>(states, keyOf("t" + std::to_string(at)), h.owner));
            held.back()->load().then([&answered](TableViews::Result const& result) {
                CHECK(result.has_value());
                ++answered;
            });
        }
        h.settle();
        return answered;
    };
    CHECK(mountScreen() == 10);
    REQUIRE(h.client.count("load") == 1);
    auto const load = std::ranges::find(h.client.calls, std::string{"load"});
    std::string const& body = h.client.bodies.at(static_cast<std::size_t>(load - h.client.calls.begin()));
    auto const parts = [&body](std::string_view part) {
        std::size_t count = 0;
        std::string const needle = R"("part":")" + std::string{part} + "\"";
        for (auto at = body.find(needle); at != std::string::npos; at = body.find(needle, at + 1)) {
            ++count;
        }
        return count;
    };
    CHECK(parts("state") == 5);
    CHECK(parts("views") == 5);
    CHECK(parts("shared") == 5);
    CHECK(mountScreen() == 10);  // the remount
    CHECK(h.client.count("load") == 1);

    RowsFixture p;
    RecordingTableStateStore privateStore{p.owner};
    TableStates privateStates{sharedOf(privateStore), p.owner, &p.scheduler, 500ms};
    TableViews views{privateStates, kKey, p.owner};
    REQUIRE(settled(p, views.load()).has_value());
    CHECK(std::ranges::none_of(privateStore.calls, [](StoreCall const& call) { return call.part == "shared"; }));
}

// Mutation: the retry after a Conflict resending kNoDocument (the second Conflict reports the views unsaved).
TEST_CASE("views: a store that ignores kNoDocument still saves the first view", "[ui][table][state]") {
    RowsFixture h;
    LiteralTableStateStore store{h.owner};
    TableStates states{sharedOf(store), h.owner, &h.scheduler, 500ms};
    TableViews views{states, kKey, h.owner};
    REQUIRE(settled(h, views.load()).has_value());
    std::size_t const before = store.calls.size();
    CHECK(settled(h, views.change(table::AddView{viewNamed("A")})).has_value());
    CHECK(callsOf(store.calls, "views", before) ==
          std::vector<std::string>{"save expected=- Conflict", "load", "save expected= Saved"});
    CHECK(store.calls.at(before).revision.empty());  // the conflict over nothing names ""
    CHECK(viewNames(store.stored("views")) == std::vector<std::string>{"A"});
    CHECK(views.own().views.size() == 1);
}

// Review Focus 5. Mutation: change() on Conflict saving its own document with the new revision instead of applying
// the operation to the reloaded one (window 2's view "C" is lost, or two views are named "C"). The menu's rule that
// Save view… under a taken name is a `ReplaceView`, and its mutation, are Task 14c's.
TEST_CASE("views: a taken name is replaced on save and refused on rename", "[ui][table][state]") {
    RowsFixture h;
    RecordingTableStateStore store{h.owner};
    table::ViewsDocument seeded;
    seeded.views = {viewNamed("A"), viewNamed("B")};
    REQUIRE(settled(h, store.save(viewsKey(), table::writeViews(seeded, table::ViewsPart::Own))).outcome ==
            table::SaveOutcome::Saved);
    std::string const r0 = store.calls.back().revision;
    ViewsWindow one{h, store};
    REQUIRE(settled(h, one.views.load()).has_value());
    std::size_t const before = store.calls.size();
    REQUIRE(settled(h, one.views.change(table::ReplaceView{viewNamed("A", "weight")})).has_value());
    CHECK(callsOf(store.calls, "views", before) == std::vector<std::string>{"save expected=" + r0 + " Saved"});
    std::string const r1 = store.calls.back().revision;
    CHECK(one.views.own().views.size() == 2);
    CHECK(one.views.own().views.front().sort.front().column == "weight");
    std::size_t const renaming = store.calls.size();
    CHECK(settled(h, one.views.change(table::RenameView{.from = "B", .to = "A"})) ==
          TableViews::Result{std::unexpected(std::string{"A view named A exists"})});
    CHECK(store.calls.size() == renaming);  // no save
    // Window 2 reads r1 and saves "C"; window 1, holding r1 and no "C", renames "B" to "C".
    ViewsWindow two{h, store};
    REQUIRE(settled(h, two.views.load()).has_value());
    REQUIRE(settled(h, two.views.change(table::AddView{viewNamed("C")})).has_value());
    std::string const r2 = store.calls.back().revision;
    std::size_t const late = store.calls.size();
    CHECK(settled(h, one.views.change(table::RenameView{.from = "B", .to = "C"})) ==
          TableViews::Result{std::unexpected(std::string{"A view named C exists"})});
    CHECK(callsOf(store.calls, "views", late) ==
          std::vector<std::string>{"save expected=" + r1 + " Conflict", "load"});
    CHECK(store.calls.at(late).revision == r2);
    auto const stored = settled(h, store.load(viewsKey()));
    CHECK(viewNames(stored) == std::vector<std::string>{"A", "B", "C"});
    REQUIRE(stored.has_value());
    CHECK(stored->revision == r2);
}

// Mutation: the lowering's `views->load()` calls deleted (no load of part "views" or "shared" at mount).
TEST_CASE("views: a mounted table loads its views with its state, and a second mount reads them kept",
          "[ui][table][state]") {
    using morph::ui::testing::kModelTable;
    using morph::ui::testing::kThree;
    using morph::ui::testing::replyDescribe;
    using morph::ui::testing::rowsDoc;
    using morph::ui::testing::storeExecutor;
    using morph::ui::testing::TableHarness;
    auto const parts = [](std::vector<StoreCall> const& calls) {
        std::multiset<std::string> loaded;
        for (StoreCall const& call : calls) {
            if (call.op == "load") {
                loaded.insert(call.part);
            }
        }
        return loaded;
    };
    for (bool const model : {false, true}) {
        CAPTURE(model);
        auto const store = std::make_shared<RecordingTableStateStore>(storeExecutor(), table::Sharing::Shared);
        TableHarness h{store};
        auto const mountOne = [&] {
            auto screen = h.mount(model ? std::string{kModelTable} : rowsDoc(kThree));
            if (model) {
                replyDescribe(h);
            }
            h.settle();
            return screen;
        };
        auto first = mountOne();
        CHECK(parts(store->calls) == std::multiset<std::string>{"shared", "state", "views"});
        first.reset();
        h.settle();
        std::size_t const before = store->calls.size();
        auto const second = mountOne();
        CHECK(parts({store->calls.begin() + static_cast<std::ptrdiff_t>(before), store->calls.end()}).empty());
    }
}

// --- Saved views in the table menu
// -------------------------------------------------------------------------------------

namespace {

using morph::ui::HeaderClick;
using morph::ui::MenuEntry;
using morph::ui::testing::answerPages;
using morph::ui::testing::chooseMenu;
using morph::ui::testing::elementValue;
using morph::ui::testing::entryOf;
using morph::ui::testing::labelsOf;
using morph::ui::testing::menuOf;
using morph::ui::testing::queryOf;
using morph::ui::testing::statusOf;
using morph::ui::testing::storeExecutor;
using morph::ui::testing::TableHarness;

// A view of @p name sorted by one key, hiding @p hidden, over the columns @p order.
table::SavedView viewSorted(std::string name, std::vector<std::string> order, std::string column,
                            table::SortDirection direction, std::vector<std::string> hidden = {}) {
    table::ColumnLayoutValue layout;
    layout.order = std::move(order);
    layout.hidden = std::move(hidden);
    return table::SavedView{.name = std::move(name),
                            .layout = std::move(layout),
                            .sort = {table::SortKey{.column = std::move(column), .dir = direction}},
                            .filters = {}};
}

// kNine's columns.
std::vector<std::string> nineColumns() { return {"c1", "c2", "c3", "c4", "c5", "c6", "c7", "c8", "c9"}; }

// The user's own views document.
std::string ownViews(std::vector<table::SavedView> views, std::optional<std::string> defaultView = {},
                     bool onMount = false) {
    table::ViewsDocument document;
    document.views = std::move(views);
    document.defaultView = std::move(defaultView);
    document.defaultOnMount = onMount;
    return table::writeViews(document, table::ViewsPart::Own);
}

// Stores @p json as @p part of a store that keeps one table's state, and settles.
std::string seed(TableHarness& h, RecordingTableStateStore& store, std::string part, std::string json) {
    table::TableStateKey key = kKey;
    key.part = std::move(part);
    std::optional<table::SaveResult> saved;
    store.save(key, std::move(json)).thenDetached([&saved](table::SaveResult const& result) { saved = result; });
    h.settle();
    REQUIRE(saved.has_value());
    REQUIRE(saved->outcome == table::SaveOutcome::Saved);
    return saved->revision;
}

// What a store that keeps one table's state holds as @p part.
std::optional<table::StoredTableState> storedPart(TableHarness& h, table::ITableStateStore& store, std::string part) {
    table::TableStateKey key = kKey;
    key.part = std::move(part);
    std::optional<std::optional<table::StoredTableState>> got;
    store.load(key).thenDetached([&got](std::optional<table::StoredTableState> const& stored) { got = stored; });
    h.settle();
    REQUIRE(got.has_value());
    return *got;
}

// The "Views" submenu of a table's menu, as a user opening it sees it.
std::vector<MenuEntry> viewsOf(TableHarness& h, int table) {
    return entryOf(menuOf(h, table, std::nullopt), "Views").items;
}

// Names a view in the open view name dialog and presses Save.
void nameView(TableHarness& h, std::string name) {
    h.backend.edit(h.byTestId("TextInput", "table_view_name"), std::move(name));
    h.settle();
    h.backend.click(h.byTestId("Button", "table_view_save"));
    h.settle();
}

// The first query a model-backed table sent for rows.
table::TableQuery firstQuery(TableHarness const& h) {
    for (std::size_t index = 0; index < h.dispatcher.calls.size(); ++index) {
        if (h.dispatcher.calls[index].action == "Table.samples") {
            return queryOf(h, index);
        }
    }
    FAIL("no query of Table.samples");
    return {};
}

}  // namespace

// Mutation: Save view… with a taken name sending AddView (refused as NameTaken, the sort change lost).
TEST_CASE("views: Save view... under a taken name replaces that view", "[ui][table][state]") {
    auto const store = std::make_shared<RecordingTableStateStore>(storeExecutor());
    TableHarness h{store};
    std::string const r0 = seed(h, *store, "views",
                                ownViews({viewSorted("A", nineColumns(), "c1", table::SortDirection::Ascending),
                                          viewSorted("B", nineColumns(), "c2", table::SortDirection::Ascending)}));
    auto const screen = h.mount(morph::ui::testing::kNine);
    int const table = h.backend.all("Table").front();
    h.backend.clickHeader(table, 3, HeaderClick::Plain);  // c4 ascending
    h.settle();
    std::size_t const before = store->calls.size();
    chooseMenu(h, table, std::nullopt, {"Views", "Save view…"});
    nameView(h, "A");
    CHECK(callsOf(store->calls, "views", before) == std::vector<std::string>{"save expected=" + r0 + " Saved"});
    std::string const r1 = store->calls.back().revision;
    auto const stored = storedPart(h, *store, "views");
    REQUIRE(stored.has_value());
    CHECK(stored->revision == r1);
    auto const views = table::readViews(stored->json);
    REQUIRE(views.has_value());
    REQUIRE(views->views.size() == 2);
    CHECK(views->views.front().name == "A");
    CHECK(views->views.front().sort ==
          table::SortChain{table::SortKey{.column = "c4", .dir = table::SortDirection::Ascending}});
    CHECK(statusOf(h, table).message == "Saved view A");
    // The arrangement is the view it was saved as: checked, and not modified.
    CHECK(entryOf(viewsOf(h, table), "A").checked == std::optional<bool>{true});
}

// Mutation: the modified mark compared against the unfitted view.
TEST_CASE("views: applying a view records its name, and a change marks it modified", "[ui][table][state]") {
    auto const store = std::make_shared<RecordingTableStateStore>(storeExecutor());
    TableHarness h{store};
    table::SavedView open = viewSorted("Open orders", nineColumns(), "c2", table::SortDirection::Descending, {"c3"});
    table::ColumnFilter gone;
    gone.include.push_back(table::FilterEntry{.eq = "1"});
    open.filters.columns.emplace("gone", gone);  // a column the table does not have
    table::ColumnFilter atLeastTwo;
    atLeastTwo.include.push_back(table::FilterEntry{.ge = "2"});
    open.filters.columns.emplace("c1", atLeastTwo);  // a column it has
    seed(h, *store, "views", ownViews({open}));
    auto const screen = h.mount(morph::ui::testing::kNine);
    int const table = h.backend.all("Table").front();
    chooseMenu(h, table, std::nullopt, {"Views", "Open orders"});
    CHECK(elementValue(*screen, "t", "sort") == R"([{"column":"c2","direction":"desc"}])");
    CHECK(elementValue(*screen, "t", "visibleColumns") == R"(["c1","c2","c4","c5","c6","c7","c8","c9"])");
    CHECK_FALSE(elementValue(*screen, "t", "filters").contains("gone"));  // applied without that filter
    CHECK(elementValue(*screen, "t", "filters").contains(R"("c1")"));     // and with the other
    CHECK(elementValue(*screen, "t", "viewCount") == "2");
    h.advance(500ms);
    auto const state = storedPart(h, *store, "state");
    REQUIRE(state.has_value());
    auto const read = table::readState(state->json);
    REQUIRE(read.has_value());
    CHECK(read->view == std::optional<std::string>{"Open orders"});
    MenuEntry const applied = entryOf(viewsOf(h, table), "Open orders");
    CHECK(applied.checked == std::optional<bool>{true});
    h.backend.clickHeader(table, 0, HeaderClick::Plain);
    h.settle();
    std::vector<std::string> const labels = labelsOf(viewsOf(h, table));
    CHECK(std::ranges::contains(labels, std::string{"Open orders (modified)"}));
    CHECK_FALSE(std::ranges::contains(labels, std::string{"Open orders"}));
}

// Mutation: defaultOnMount applying the default view's layout too.
TEST_CASE("views: the default view at mount, and Start from the default view", "[ui][table][state]") {
    table::SavedView const open =
        viewSorted("Open orders", nineColumns(), "c2", table::SortDirection::Descending, {"c3"});
    SECTION("no state stored: the default view is applied") {
        auto const store = std::make_shared<RecordingTableStateStore>(storeExecutor());
        TableHarness h{store};
        // Its filter is a range on a column nothing has put in use yet, whose kind the rows learn at mount.
        table::SavedView filtered = open;
        table::ColumnFilter atLeastTwo;
        atLeastTwo.include.push_back(table::FilterEntry{.ge = "2"});
        filtered.filters.columns.emplace("c1", atLeastTwo);
        seed(h, *store, "views", ownViews({filtered}, "Open orders"));
        auto const screen = h.mount(morph::ui::testing::kNine);
        CHECK(elementValue(*screen, "t", "sort") == R"([{"column":"c2","direction":"desc"}])");
        CHECK(elementValue(*screen, "t", "visibleColumns") == R"(["c1","c2","c4","c5","c6","c7","c8","c9"])");
        CHECK(elementValue(*screen, "t", "viewCount") == "2");
        // "Start from the default view" is the views document's `defaultOnMount`, and checks its entry.
        int const table = h.backend.all("Table").front();
        CHECK(entryOf(viewsOf(h, table), "Start from the default view").checked == std::optional<bool>{false});
        chooseMenu(h, table, std::nullopt, {"Views", "Start from the default view"});
        auto const stored = storedPart(h, *store, "views");
        REQUIRE(stored.has_value());
        auto const views = table::readViews(stored->json);
        REQUIRE(views.has_value());
        CHECK(views->defaultOnMount);
        CHECK(entryOf(viewsOf(h, table), "Start from the default view").checked == std::optional<bool>{true});
    }
    SECTION("a stored state and defaultOnMount: the default's sort and filters, the stored layout") {
        auto const store = std::make_shared<RecordingTableStateStore>(storeExecutor());
        TableHarness h{store};
        seed(h, *store, "views", ownViews({open}, "Open orders", true));
        seed(h, *store, "state",
             R"({"version":1,"sort":[{"column":"c1","direction":"asc"}],)"
             R"("layout":{"order":["c1","c2","c3","c4","c5","c6","c7","c8","c9"],"hidden":["c4"]}})");
        auto const screen = h.mount(morph::ui::testing::kNine);
        CHECK(elementValue(*screen, "t", "sort") == R"([{"column":"c2","direction":"desc"}])");
        CHECK(elementValue(*screen, "t", "visibleColumns") == R"(["c1","c2","c3","c5","c6","c7","c8","c9"])");
    }
    SECTION("a model-backed table's first query is the default view's") {
        auto const store = std::make_shared<RecordingTableStateStore>(storeExecutor());
        TableHarness h{store};
        seed(h, *store, "views",
             ownViews({viewSorted("Open orders", {"id", "name", "c3", "c4"}, "name", table::SortDirection::Descending,
                                  {"c3"})},
                      "Open orders"));
        auto const screen = h.mount(morph::ui::testing::kModelTable);
        morph::ui::testing::replyDescribe(h, morph::ui::testing::samplesDescription(4));
        table::TableQuery const query = firstQuery(h);
        CHECK(query.sort ==
              table::SortChain{table::SortKey{.column = "name", .dir = table::SortDirection::Descending}});
        REQUIRE(query.columns.has_value());
        CHECK_FALSE(std::ranges::contains(*query.columns, std::string{"c3"}));
        CHECK(std::ranges::contains(*query.columns, std::string{"c4"}));
    }
}

// Ruling: each kind of table that keeps a state wires the "Views" submenu. Mutation: a collection's table not handing
// its views to its configuring (its table menu has no "Views").
TEST_CASE("views: the Views submenu on a rows table, a model table and a collection", "[ui][table][state]") {
    struct Kind {
        std::string name;
        std::vector<std::string> order;
    };
    for (Kind const& kind : {Kind{.name = "rows", .order = {"id", "name", "weight"}},
                             Kind{.name = "model", .order = {"id", "name", "c3"}},
                             Kind{.name = "collection", .order = {"id", "name", "weight"}}}) {
        CAPTURE(kind.name);
        auto const store = std::make_shared<RecordingTableStateStore>(storeExecutor());
        TableHarness h{store};
        h.extraViews.insert_or_assign("Lims_ListSamples", std::string{morph::ui::testing::kMixedSamplesView});
        seed(h, *store, "views", ownViews({viewSorted("Mine", kind.order, "name", table::SortDirection::Descending)}));
        std::unique_ptr<morph::ui::Screen> screen;
        int table = 0;
        if (kind.name == "rows") {
            screen = h.mount(morph::ui::testing::rowsDoc(morph::ui::testing::kThree));
            table = h.byTestId("Table", "list");
        } else if (kind.name == "model") {
            screen = h.mount(morph::ui::testing::kModelTable);
            morph::ui::testing::replyDescribe(h);
            answerPages(h, 3);
            table = h.byTestId("Table", "list");
        } else {
            screen = h.mount(morph::ui::testing::kSamplesCollection);
            h.reply(h.dispatcher.last("Lims_ListSamples"),
                    R"({"rows":[{"id":1,"name":"beta","weight":3},{"id":2,"name":"Alpha","weight":1},)"
                    R"({"id":3,"name":"gamma","weight":2}]})");
            table = h.byTestId("Table", "collection_rows");
        }
        std::vector<std::string> const labels = labelsOf(viewsOf(h, table));
        CHECK(labels == std::vector<std::string>{"Mine", "Save view…", "Rename view…", "Remove view", "Set as default",
                                                 "Start from the default view"});
        chooseMenu(h, table, std::nullopt, {"Views", "Mine"});
        if (kind.name == "model") {
            answerPages(h, 3);
        }
        CHECK(h.backend.prop(table, "sort") == "1:Descending");
        // A second view, then a rename onto the first's name, refused and changing nothing, then its removal.
        chooseMenu(h, table, std::nullopt, {"Views", "Save view…"});
        nameView(h, "Second");
        CHECK(statusOf(h, table).message == "Saved view Second");
        chooseMenu(h, table, std::nullopt, {"Views", "Rename view…"});
        CHECK(h.backend.prop(h.byTestId("TextInput", "table_view_name"), "text") == "Second");
        nameView(h, "Mine");
        CHECK(statusOf(h, table).message == "A view named Mine exists");
        CHECK(entryOf(viewsOf(h, table), "Second").checked == std::optional<bool>{true});
        chooseMenu(h, table, std::nullopt, {"Views", "Remove view"});
        auto const stored = storedPart(h, *store, "views");
        REQUIRE(stored.has_value());
        CHECK(viewNames(stored) == std::vector<std::string>{"Mine"});
        CHECK(labelsOf(viewsOf(h, table)).front() == "Mine");
    }
}

// Ruling: a `saved` point naming a table over `rows` that keeps a state reads and writes that state's `view`.
// Mutation: the rows table left out of `tablePoint` for a `saved` point (the choice is a plain preference: no save).
TEST_CASE("views: a saved point naming a rows table edits its state's view", "[ui][table][state]") {
    auto const store = std::make_shared<RecordingTableStateStore>(storeExecutor());
    TableHarness h{store};
    seed(h, *store, "views",
         ownViews({viewSorted("Mine", {"id", "name", "weight"}, "name", table::SortDirection::Descending)}));
    std::string text = morph::ui::testing::rowsDoc(morph::ui::testing::kThree);
    text.insert(text.find(R"("state":)"), R"("customize":{"v":{"kind":"saved","of":"list"}},)");
    auto screen = h.mount(text);
    int const table = h.byTestId("Table", "list");
    chooseMenu(h, table, std::nullopt, {"Views", "Mine"});
    CHECK(morph::ui::testing::readJson(*screen, R"({"pref":"v"})") == R"("Mine")");
    CHECK(screen->setPreference("v", morph::ui::Value{std::string{"Other"}}));
    CHECK_FALSE(screen->setPreference("v", morph::ui::Value{std::int64_t{7}}));
    h.advance(500ms);
    auto const state = storedPart(h, *store, "state");
    REQUIRE(state.has_value());
    auto const read = table::readState(state->json);
    REQUIRE(read.has_value());
    CHECK(read->view == std::optional<std::string>{"Other"});
}

// Mutation: the shared part saved through `save` instead of `publish` (an authoriser cannot tell it from own state).
TEST_CASE(
    "shared views on a table: publish, read by another person, Save as my view; none over a store that does "
    "not share",
    "[ui][table][state][remote]") {
    StateServer server{TableStateOptions{}, std::make_shared<NoPublishForBob>()};
    table::TableStateKey const shared{
        .application = "lims", .model = "", .table = "list", .qualifier = "samples", .part = "shared"};
    std::string const doc = morph::ui::testing::rowsDoc(morph::ui::testing::kThree);
    // The revision a session keeps for the shared part: what its load or its publish answered.
    auto const keptRevision = [&shared](TableHarness& h) {
        std::optional<std::optional<table::StoredTableState>> got;
        h.session.tableStates()->load(shared).thenDetached(
            [&got](std::optional<table::StoredTableState> const& stored) { got = stored; });
        h.settle();
        REQUIRE(got.has_value());
        REQUIRE(got->has_value());
        return (*got)->revision;
    };

    ServerDispatcher aliceClient{server, storeExecutor(), "alice"};
    TableHarness alice{std::make_shared<ui::PreferencesTableStateStore>(aliceClient, storeExecutor())};
    auto const aliceScreen = alice.mount(doc);
    int const aliceTable = alice.byTestId("Table", "list");
    alice.backend.clickHeader(aliceTable, 1, HeaderClick::Plain);  // name ascending
    alice.settle();
    chooseMenu(alice, aliceTable, std::nullopt, {"Views", "Publish view…"});
    nameView(alice, "Team");
    CHECK(statusOf(alice, aliceTable).message == "Published view Team");
    CHECK(aliceClient.count("publish") == 1);
    for (std::size_t at = 0; at < aliceClient.calls.size(); ++at) {
        if (aliceClient.calls.at(at) == "save") {
            CHECK_FALSE(aliceClient.bodies.at(at).contains(R"("part":"shared")"));
        }
    }
    std::string const published = keptRevision(alice);
    CHECK(isRevision(published));

    ServerDispatcher bobClient{server, storeExecutor(), "bob"};
    TableHarness bob{std::make_shared<ui::PreferencesTableStateStore>(bobClient, storeExecutor())};
    auto const bobScreen = bob.mount(doc);
    int const bobTable = bob.byTestId("Table", "list");
    CHECK(keptRevision(bob) == published);
    std::vector<MenuEntry> const views = viewsOf(bob, bobTable);
    CHECK(std::ranges::any_of(views,
                              [](MenuEntry const& entry) { return entry.heading && entry.label == "Shared views"; }));
    CHECK(std::ranges::contains(labelsOf(views), std::string{"Team"}));
    CHECK_FALSE(std::ranges::contains(labelsOf(views), std::string{"Save as my view"}));
    chooseMenu(bob, bobTable, std::nullopt, {"Views", "Team"});
    CHECK(elementValue(*bobScreen, "list", "sort") == R"([{"column":"name","direction":"asc"}])");
    chooseMenu(bob, bobTable, std::nullopt, {"Views", "Save as my view"});
    CHECK(statusOf(bob, bobTable).message == "Saved view Team");
    table::TableStateKey own = shared;
    own.part = "views";
    std::optional<std::optional<table::StoredTableState>> mine;
    bob.session.tableStates()->store().load(own).thenDetached(
        [&mine](std::optional<table::StoredTableState> const& stored) { mine = stored; });
    bob.settle();
    REQUIRE(mine.has_value());
    CHECK(viewNames(*mine) == std::vector<std::string>{"Team"});
    // The authoriser refuses bob's publish.
    chooseMenu(bob, bobTable, std::nullopt, {"Views", "Publish view…"});
    nameView(bob, "Bob's");
    CHECK(statusOf(bob, bobTable).message.starts_with("This view could not be published:"));

    // Over a store that does not share, nothing offers publishing and no shared part is read.
    {
        TableHarness quiet{std::make_shared<table::MemoryTableStateStore>(storeExecutor())};
        auto const screen = quiet.mount(doc);
        std::vector<std::string> const labels = labelsOf(viewsOf(quiet, quiet.byTestId("Table", "list")));
        CHECK_FALSE(std::ranges::contains(labels, std::string{"Publish view…"}));
        CHECK_FALSE(std::ranges::contains(labels, std::string{"Save as my view"}));
    }
    auto const recording = std::make_shared<RecordingTableStateStore>(storeExecutor());
    TableHarness counted{recording};
    auto const screen = counted.mount(doc);
    CHECK_FALSE(std::ranges::contains(labelsOf(viewsOf(counted, counted.byTestId("Table", "list"))),
                                      std::string{"Publish view…"}));
    CHECK(std::ranges::none_of(recording->calls, [](StoreCall const& call) { return call.part == "shared"; }));
}

// Review Focus 5, a name taken from another window after load. Mutation: `TableViews` deciding add or replace once,
// against the views loaded, and not at each attempt (the retry after the Conflict sends the same AddView, refused as
// "A view named A exists").
TEST_CASE("views: Save view... under a name another window took after load replaces it", "[ui][table][state]") {
    auto const store = std::make_shared<RecordingTableStateStore>(storeExecutor());
    TableHarness h{store};
    auto const screen = h.mount(morph::ui::testing::kNine);
    int const table = h.backend.all("Table").front();
    // Another window, loaded after this table, adds "A".
    ViewsWindow other{h, *store};
    auto const done = [&h](morph::async::Completion<TableViews::Result> completion) {
        std::optional<TableViews::Result> result;
        completion.thenDetached([&result](TableViews::Result const& got) { result = got; });
        h.settle();
        return result.has_value() && result->has_value();
    };
    REQUIRE(done(other.views.load()));
    REQUIRE(done(
        other.views.change(table::AddView{viewSorted("A", nineColumns(), "c1", table::SortDirection::Ascending)})));
    std::string const taken = store->calls.back().revision;
    h.backend.clickHeader(table, 3, HeaderClick::Plain);  // c4 ascending
    h.settle();
    std::size_t const before = store->calls.size();
    chooseMenu(h, table, std::nullopt, {"Views", "Save view…"});
    nameView(h, "A");
    CHECK(callsOf(store->calls, "views", before) ==
          std::vector<std::string>{"save expected=- Conflict", "load", "save expected=" + taken + " Saved"});
    CHECK(statusOf(h, table).message == "Saved view A");
    auto const stored = storedPart(h, *store, "views");
    REQUIRE(stored.has_value());
    auto const views = table::readViews(stored->json);
    REQUIRE(views.has_value());
    REQUIRE(views->views.size() == 1);
    CHECK(views->views.front().sort ==
          table::SortChain{table::SortKey{.column = "c4", .dir = table::SortDirection::Ascending}});
}

// Mutation: the default view applied at mount not taken as the table's state (`TableStates::start` not called): the
// first change saves only its own part, and the next mount starts from the default no more.
TEST_CASE("views: a change after the default view at mount saves on top of it", "[ui][table][state]") {
    auto const store = std::make_shared<RecordingTableStateStore>(storeExecutor());
    TableHarness h{store};
    seed(h, *store, "views",
         ownViews({viewSorted("Open orders", nineColumns(), "c2", table::SortDirection::Descending, {"c3"})},
                  "Open orders"));
    {
        auto const screen = h.mount(morph::ui::testing::kNine);
        int const table = h.backend.all("Table").front();
        chooseMenu(h, table, 4, {"Hide column"});  // c5
        h.advance(500ms);
    }
    h.settle();
    auto const state = storedPart(h, *store, "state");
    REQUIRE(state.has_value());
    auto const read = table::readState(state->json);
    REQUIRE(read.has_value());
    CHECK(read->view == std::optional<std::string>{"Open orders"});
    CHECK(read->sort == std::optional<table::SortChain>{table::SortChain{
                            table::SortKey{.column = "c2", .dir = table::SortDirection::Descending}}});
    auto const screen = h.mount(morph::ui::testing::kNine);
    CHECK(elementValue(*screen, "t", "sort") == R"([{"column":"c2","direction":"desc"}])");
    CHECK(elementValue(*screen, "t", "visibleColumns") == R"(["c1","c2","c4","c6","c7","c8","c9"])");
}

namespace {

// A harness whose store is the preferences-backed one over @p server, as @p person.
struct PersonTable {
    PersonTable(StateServer& server, std::string person)
        : client{server, storeExecutor(), std::move(person)},
          h{std::make_shared<ui::PreferencesTableStateStore>(client, storeExecutor())} {}

    ServerDispatcher client;
    TableHarness h;
};

// The shared views a fresh client of @p server reads.
table::ViewsDocument sharedViews(StateServer& server) {
    RowsFixture f;
    ServerDispatcher reader{server, f.owner, "reader"};
    ui::PreferencesTableStateStore store{reader, f.owner};
    table::TableStateKey const key{
        .application = "lims", .model = "", .table = "list", .qualifier = "samples", .part = "shared"};
    auto const stored = settled(f, store.load(key));
    REQUIRE(stored.has_value());
    auto read = table::readViews(stored->json);
    REQUIRE(read.has_value());
    return std::move(*read);
}

}  // namespace

// Mutation: "Publish view…" always publishing an AddView (`publishView` never choosing ReplaceView): the second
// publish is refused, "This view could not be published: A view named Team exists".
TEST_CASE("shared views on a table: publishing again under a taken name replaces that view",
          "[ui][table][state][remote]") {
    StateServer server{TableStateOptions{}};
    std::string const doc = morph::ui::testing::rowsDoc(morph::ui::testing::kThree);
    PersonTable alice{server, "alice"};
    auto const screen = alice.h.mount(doc);
    int const table = alice.h.byTestId("Table", "list");
    alice.h.backend.clickHeader(table, 1, HeaderClick::Plain);  // name ascending
    alice.h.settle();
    chooseMenu(alice.h, table, std::nullopt, {"Views", "Publish view…"});
    nameView(alice.h, "Team");
    REQUIRE(statusOf(alice.h, table).message == "Published view Team");
    alice.h.backend.clickHeader(table, 1, HeaderClick::Plain);  // name descending
    alice.h.settle();
    chooseMenu(alice.h, table, std::nullopt, {"Views", "Publish view…"});
    nameView(alice.h, "Team");
    CHECK(statusOf(alice.h, table).message == "Published view Team");
    table::ViewsDocument const shared = sharedViews(server);
    REQUIRE(shared.views.size() == 1);
    CHECK(shared.views.front().name == "Team");
    CHECK(shared.views.front().sort ==
          table::SortChain{table::SortKey{.column = "name", .dir = table::SortDirection::Descending}});
}

// Review Focus 5 for the shared views: a name another person published after this table loaded. Mutation: `TableViews`
// deciding add or replace once, against the views loaded (the retry after the Conflict sends the same AddView).
TEST_CASE("shared views on a table: publishing under a name published elsewhere after load replaces it",
          "[ui][table][state][remote]") {
    StateServer server{TableStateOptions{}};
    std::string const doc = morph::ui::testing::rowsDoc(morph::ui::testing::kThree);
    PersonTable alice{server, "alice"};
    auto const aliceScreen = alice.h.mount(doc);
    int const aliceTable = alice.h.byTestId("Table", "list");
    PersonTable bob{server, "bob"};
    auto const bobScreen = bob.h.mount(doc);
    int const bobTable = bob.h.byTestId("Table", "list");
    chooseMenu(bob.h, bobTable, std::nullopt, {"Views", "Publish view…"});
    nameView(bob.h, "Team");
    REQUIRE(statusOf(bob.h, bobTable).message == "Published view Team");
    alice.h.backend.clickHeader(aliceTable, 1, HeaderClick::Plain);  // name ascending
    alice.h.settle();
    chooseMenu(alice.h, aliceTable, std::nullopt, {"Views", "Publish view…"});
    nameView(alice.h, "Team");
    CHECK(statusOf(alice.h, aliceTable).message == "Published view Team");
    table::ViewsDocument const shared = sharedViews(server);
    REQUIRE(shared.views.size() == 1);
    CHECK(shared.views.front().sort ==
          table::SortChain{table::SortKey{.column = "name", .dir = table::SortDirection::Ascending}});
}
