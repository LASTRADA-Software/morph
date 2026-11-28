// SPDX-License-Identifier: Apache-2.0

// The client side of persisted table state against a server that answers badly (a load that names the wrong number
// of keys, a document that keeps changing, a part that ends early, a store that is always busy) and the kept states
// of a session over a store whose loads and saves a test settles.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <morph/core/completion.hpp>
#include <morph/core/registry.hpp>
#include <morph/table/sort.hpp>
#include <morph/table/state_store.hpp>
#include <morph/ui/table_state.hpp>
#include <morph/ui/table_state_model.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ui_table_support.hpp"

namespace {

using namespace std::chrono_literals;
using morph::ui::StateParts;
using morph::ui::TableStates;
using morph::ui::testing::RowsFixture;
namespace table = morph::table;
namespace ui = morph::ui;

// NOLINTNEXTLINE(cert-err58-cpp,bugprone-throwing-static-initialization): a test table; an allocation failure at start-up ends the run either way.
table::TableStateKey const kKey{
    .application = "lims", .model = "Lims_SampleModel", .table = "samples", .qualifier = "samples", .part = "state"};

// What a completion settled with, once the fixture has run what was posted.
template <typename T>
struct Outcome {
    std::optional<T> value;
    std::string failure;
    bool failed = false;
};

template <typename T>
Outcome<T> watch(RowsFixture& h, morph::async::Completion<T> completion) {
    auto out = std::make_shared<Outcome<T>>();
    completion.then([out](T const& got) { out->value = got; }).onError([out](std::exception_ptr const& error) {
        out->failed = true;
        try {
            std::rethrow_exception(error);
        } catch (std::exception const& caught) {
            out->failure = caught.what();
        }
    });
    h.settle();
    return *out;
}

std::string loadedJson(std::vector<ui::TableStateHead> entries) {
    return morph::model::ActionTraits<ui::TableStateLoad>::resultToJson(
        ui::TableStateLoaded{.entries = std::move(entries)});
}

std::string chunkJson(std::string revision, std::string text) {
    return morph::model::ActionTraits<ui::TableStateRead>::resultToJson(
        ui::TableStateChunk{.revision = std::move(revision), .text = std::move(text)});
}

std::string savedJson(table::SaveOutcome outcome, std::string reason = {}) {
    return morph::model::ActionTraits<ui::TableStateSave>::resultToJson(ui::TableStateSaved{
        .result = table::SaveResult{.outcome = outcome, .revision = "r", .reason = std::move(reason)}});
}

ui::TableStateHead found(std::string revision, std::uint64_t size, std::string text) {
    return ui::TableStateHead{.found = true,
                              .revision = std::move(revision),
                              .size = size,
                              .text = std::move(text),
                              .error = {},
                              .deferred = false};
}

// A store whose loads and saves a test settles.
class ScriptedStore final : public table::ITableStateStore {
public:
    explicit ScriptedStore(morph::exec::IExecutor& owner) : _owner{&owner} {}

    [[nodiscard]] morph::async::Completion<std::optional<table::StoredTableState>> load(
        table::TableStateKey const& /*key*/) override {
        auto [completion, promise] =
            morph::async::Completion<std::optional<table::StoredTableState>>::makeSettleable(_owner);
        loads.push_back(std::move(promise));
        return std::move(completion);
    }

    [[nodiscard]] morph::async::Completion<table::SaveResult> save(table::TableStateKey const& /*key*/,
                                                                   std::string json,
                                                                   std::string /*expected*/ = {}) override {
        auto [completion, promise] = morph::async::Completion<table::SaveResult>::makeSettleable(_owner);
        saved.push_back(std::move(json));
        saves.push_back(std::move(promise));
        return std::move(completion);
    }

    [[nodiscard]] morph::async::Completion<bool> remove(table::TableStateKey const& /*key*/) override {
        auto [completion, promise] = morph::async::Completion<bool>::makeSettleable(_owner);
        promise.resolve(false);
        return std::move(completion);
    }

    [[nodiscard]] morph::async::Completion<table::SaveResult> publish(table::TableStateKey const& /*key*/,
                                                                      table::ViewOperation const& /*operation*/,
                                                                      std::string /*expected*/) override {
        return refusePublish(*_owner);
    }

    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): a test double the test steers directly.
    std::vector<morph::async::Completion<std::optional<table::StoredTableState>>::Promise> loads;
    std::vector<morph::async::Completion<table::SaveResult>::Promise> saves;
    std::vector<std::string> saved;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

private:
    morph::exec::IExecutor* _owner;
};

table::StateDocument sortedBy(std::string column) {
    table::StateDocument document;
    document.sort =
        table::SortChain{table::SortKey{.column = std::move(column), .dir = table::SortDirection::Ascending}};
    return document;
}

table::StoredTableState storedSortedBy(std::string column) {
    return table::StoredTableState{.json = table::writeState(sortedBy(std::move(column))), .revision = "1"};
}

}  // namespace

TEST_CASE("cov h state client: a load that cannot be read rejects every key it asked for", "[covh][ui][state]") {
    struct Case {
        char const* what;
        std::string reply;  // empty: the call fails
        std::string message;
    };
    for (Case const& item : std::vector<Case>{
             {"an answer for fewer keys", loadedJson({}), "answered 0 entries for 1 keys"},
             {"an answer that is no load", "not json", ""},
             {"a call that fails", "", "it failed"},
             {"a key the server refused",
              loadedJson({ui::TableStateHead{.found = false,
                                             .revision = {},
                                             .size = 0,
                                             .text = {},
                                             .error = "key out of bounds",
                                             .deferred = false}}),
              "key out of bounds"},
         }) {
        INFO(item.what);
        RowsFixture h;
        ui::PreferencesTableStateStore store{h.dispatcher, h.owner};
        auto const outcome = [&] {
            auto completion = store.load(kKey);
            h.settle();
            REQUIRE(h.dispatcher.count("load") == 1);
            if (item.reply.empty()) {
                h.dispatcher.fail(h.dispatcher.last("load"));
            } else {
                h.dispatcher.reply(h.dispatcher.last("load"), item.reply);
            }
            return watch(h, std::move(completion));
        }();
        CHECK(outcome.failed);
        CHECK(outcome.failure.contains(item.message));
    }
}

TEST_CASE("cov h state client: a document that keeps changing, or ends early, fails its load", "[covh][ui][state]") {
    {
        RowsFixture h;
        ui::PreferencesTableStateStore store{h.dispatcher, h.owner};
        auto completion = store.load(kKey);
        std::size_t answered = 0;
        for (int round = 0; round < 12; ++round) {
            h.settle();
            if (h.dispatcher.count("load") > answered) {
                ++answered;
                h.dispatcher.reply(h.dispatcher.last("load"), loadedJson({found("r1", 10, "abc")}));
                h.settle();
                h.dispatcher.reply(h.dispatcher.last("read"), chunkJson("r2", "xyz"));
            }
        }
        auto const outcome = watch(h, std::move(completion));
        CHECK(outcome.failed);
        CHECK(outcome.failure.contains("kept changing"));
        CHECK(h.dispatcher.count("load") == 9);
    }
    {
        RowsFixture h;
        ui::PreferencesTableStateStore store{h.dispatcher, h.owner};
        auto completion = store.load(kKey);
        h.settle();
        h.dispatcher.reply(h.dispatcher.last("load"), loadedJson({found("r1", 10, "abc")}));
        h.settle();
        h.dispatcher.reply(h.dispatcher.last("read"), chunkJson("r1", ""));
        auto const outcome = watch(h, std::move(completion));
        CHECK(outcome.failed);
        CHECK(outcome.failure.contains("ended before its size"));
    }
    {
        RowsFixture h;
        ui::PreferencesTableStateStore store{h.dispatcher, h.owner};
        auto completion = store.load(kKey);
        h.settle();
        h.dispatcher.reply(h.dispatcher.last("load"), loadedJson({found("r1", 6, "abc")}));
        h.settle();
        h.dispatcher.reply(h.dispatcher.last("read"), chunkJson("r1", "def"));
        auto const outcome = watch(h, std::move(completion));
        REQUIRE(outcome.value.has_value());
        REQUIRE(outcome.value->has_value());
        CHECK((*outcome.value)->json == "abcdef");
    }
}

TEST_CASE("cov h state client: a store that is always busy is asked until the bound, with or without a timer",
          "[covh][ui][state]") {
    RowsFixture h;
    ui::PreferencesTableStateStore store{h.dispatcher, h.owner};
    auto completion = store.save(kKey, R"({"version":1})");
    std::size_t sent = 0;
    for (int round = 0; round < 200; ++round) {
        h.settle();
        if (h.dispatcher.count("save") > sent) {
            sent = h.dispatcher.count("save");
            h.dispatcher.reply(h.dispatcher.last("save"),
                               savedJson(table::SaveOutcome::Refused, std::string{ui::kTableStateUploadsBusy}));
        }
    }
    auto const outcome = watch(h, std::move(completion));
    REQUIRE(outcome.value.has_value());
    CHECK(outcome.value->outcome == table::SaveOutcome::Refused);
    CHECK(outcome.value->reason == ui::kTableStateUploadsBusy);
    CHECK(h.dispatcher.count("save") == 101);
}

TEST_CASE("cov h state client: a save whose answer cannot be read rejects it", "[covh][ui][state]") {
    RowsFixture h;
    ui::PreferencesTableStateStore store{h.dispatcher, h.owner};
    auto completion = store.save(kKey, R"({"version":1})");
    h.settle();
    h.dispatcher.reply(h.dispatcher.last("save"), "nonsense");
    auto const outcome = watch(h, std::move(completion));
    CHECK(outcome.failed);
}

TEST_CASE("cov h state session: a load that fails and is asked again settles from what the second load finds",
          "[covh][ui][state]") {
    RowsFixture h;
    auto store = std::make_shared<ScriptedStore>(h.owner);
    TableStates states{store, h.owner, nullptr, 500ms};
    auto first = watch(h, states.load(kKey));
    REQUIRE(store->loads.size() == 1);
    store->loads.at(0).reject(std::make_exception_ptr(std::runtime_error{"down"}));
    h.settle();
    CHECK_FALSE(states.kept(kKey).has_value());
    auto second = states.load(kKey);
    h.settle();
    REQUIRE(store->loads.size() == 2);
    store->loads.at(1).resolve(storedSortedBy("name"));
    auto const outcome = watch(h, std::move(second));
    REQUIRE(outcome.value.has_value());
    REQUIRE(outcome.value->has_value());
    states.changed(kKey, table::StateDocument{}, StateParts::Layout);
    h.settle();
    REQUIRE(store->saved.size() == 1);
    CHECK(store->saved.front().contains("\"name\""));  // the base is what the second load found
}

TEST_CASE("cov h state session: a load that fails again, or while changes wait, still saves them",
          "[covh][ui][state]") {
    RowsFixture h;
    auto store = std::make_shared<ScriptedStore>(h.owner);
    TableStates states{store, h.owner, nullptr, 500ms};
    states.changed(kKey, sortedBy("name"), StateParts::Sort);  // starts the load, holds the change
    h.settle();
    REQUIRE(store->loads.size() == 1);
    CHECK(store->saved.empty());
    store->loads.at(0).reject(std::make_exception_ptr(std::runtime_error{"down"}));
    h.settle();
    REQUIRE(store->saved.size() == 1);  // the held change is saved over nothing
    CHECK(store->saved.front().contains("\"name\""));
    store->saves.at(0).resolve(
        table::SaveResult{.outcome = table::SaveOutcome::Refused, .revision = {}, .reason = "no"});
    h.settle();
    auto again = states.load(kKey);  // the entry is still not known: asks again
    h.settle();
    REQUIRE(store->loads.size() == 2);
    store->loads.at(1).reject(std::make_exception_ptr(std::runtime_error{"still down"}));
    auto const outcome = watch(h, std::move(again));
    CHECK(outcome.failed);
    CHECK(outcome.failure.contains("still down"));
}

TEST_CASE("cov h state session: keeping a state during its load, and starting one that is not settled",
          "[covh][ui][state]") {
    RowsFixture h;
    auto store = std::make_shared<ScriptedStore>(h.owner);
    TableStates states{store, h.owner, nullptr, 500ms};
    auto pending = states.load(kKey);
    h.settle();
    REQUIRE(store->loads.size() == 1);
    states.start(kKey, sortedBy("c3"));  // not settled: nothing to start
    states.start(
        table::TableStateKey{.application = "lims", .model = "m", .table = "other", .qualifier = "q", .part = "state"},
        sortedBy("c3"));
    states.keep(kKey, storedSortedBy("name"));  // the load is still out
    CHECK(states.kept(kKey).has_value());
    store->loads.at(0).resolve(storedSortedBy("id"));
    auto const outcome = watch(h, std::move(pending));
    REQUIRE(outcome.value.has_value());
    REQUIRE(outcome.value->has_value());
    CHECK((*outcome.value)->json.contains("\"name\""));  // what was kept stays
}

TEST_CASE(
    "cov h state session: flushing a clean state saves nothing, and a refused save with no handler is kept quiet",
    "[covh][ui][state]") {
    RowsFixture h;
    auto store = std::make_shared<ScriptedStore>(h.owner);
    TableStates states{store, h.owner, nullptr, 500ms};
    states.flush(kKey);  // an unknown key
    states.keep(kKey, storedSortedBy("name"));
    states.flush(kKey);  // known, settled, clean
    h.settle();
    CHECK(store->saved.empty());
    states.changed(kKey, sortedBy("id"), StateParts::Sort);
    h.settle();
    REQUIRE(store->saved.size() == 1);
    store->saves.at(0).resolve(
        table::SaveResult{.outcome = table::SaveOutcome::Refused, .revision = {}, .reason = "no"});
    h.settle();
    CHECK(states.kept(kKey)->json.contains("\"name\""));  // the refused save leaves the kept copy
    states.flush(kKey);
    h.settle();
    CHECK(store->saved.size() == 1);
}
