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

table::TableStateKey const kKey = keyOf("samples");

// A server offering `morph.tableState` over a probe store; its token is the verified principal.
struct StateServer {
    explicit StateServer(TableStateOptions options) {
        ui::registerTableState(prefs, dispatcher, registry, options);
        server = std::make_shared<morph::backend::RemoteServer>(pool, std::make_shared<prefs_test::TokenAuthorizer>(),
                                                                dispatcher, registry);
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

    std::vector<std::string> calls;
    int binds = 0;

private:
    class Channel final : public ui::ModelChannel {
    public:
        Channel(ServerDispatcher& owner, std::uint64_t instance) : _dispatcher{&owner}, _instance{instance} {}

        [[nodiscard]] morph::async::Completion<std::string> execute(std::string const& actionId,
                                                                    std::string const& bodyJson,
                                                                    ui::CallOptions /*options*/) override {
            _dispatcher->calls.push_back(actionId);
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

// A session's owner and timers, and the preferences-backed store over an in-process server.
struct StoreFixture : RowsFixture {
    explicit StoreFixture(TableStateOptions options = {}) : server{options} {}

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

std::size_t segmentsOf(std::string const& text) { return ui::detail::segmentText(text, 12 * 1024).size(); }

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
    CHECK(h.prefs.largestValue() <= 12 * 1024);  // every entry within segmentBytes, a segment's quotes included

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
    auto withModel = [](std::string model) {
        auto key = kKey;
        key.model = std::move(model);
        return key;
    };
    auto withTable = [](std::string tableId) {
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
    for (std::string const stored : {std::string{"{not json"}, std::string{R"({"version":2,"junk":true})"}}) {
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
    ui::Session withStore{h.runtime, std::move(options)};
    REQUIRE(withStore.tableStates() != nullptr);
    CHECK(&withStore.tableStates()->store() == &store);
    CHECK(withStore.options().tableStateTimeout == 300ms);
    CHECK(withStore.options().tableStateDebounce == 500ms);
}
