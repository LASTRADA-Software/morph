// SPDX-License-Identifier: Apache-2.0

// The session's kept table states over a store a test steers: loads and saves are answered, refused or failed one at
// a time, so the order in which a change, a load and a save meet is the test's.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <morph/core/completion.hpp>
#include <morph/table/engine.hpp>
#include <morph/table/sort.hpp>
#include <morph/table/state_store.hpp>
#include <morph/ui/table_state.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ui_table_support.hpp"

namespace {

namespace table = morph::table;
using namespace std::chrono_literals;
using morph::ui::StateParts;
using morph::ui::TableStates;
using morph::ui::testing::RowsFixture;

using LoadPromise = morph::async::Completion<std::optional<table::StoredTableState>>::Promise;
using SavePromise = morph::async::Completion<table::SaveResult>::Promise;

table::TableStateKey keyOf(std::string tableId) {
    return table::TableStateKey{
        .application = "lims", .model = "M", .table = std::move(tableId), .qualifier = "q", .part = "state"};
}

// Every call waits for the test to answer it.
class SteeredStore final : public table::ITableStateStore {
public:
    explicit SteeredStore(morph::exec::IExecutor& owner) : _owner{&owner} {}

    morph::async::Completion<std::optional<table::StoredTableState>> load(table::TableStateKey const&) override {
        auto [completion, promise] =
            morph::async::Completion<std::optional<table::StoredTableState>>::makeSettleable(_owner);
        loads.push_back(std::move(promise));
        return std::move(completion);
    }
    morph::async::Completion<table::SaveResult> save(table::TableStateKey const&, std::string json,
                                                     std::string) override {
        auto [completion, promise] = morph::async::Completion<table::SaveResult>::makeSettleable(_owner);
        saved.push_back(std::move(json));
        saves.push_back(std::move(promise));
        return std::move(completion);
    }
    morph::async::Completion<bool> remove(table::TableStateKey const&) override {
        auto [completion, promise] = morph::async::Completion<bool>::makeSettleable(_owner);
        promise.resolve(false);
        return std::move(completion);
    }
    morph::async::Completion<table::SaveResult> publish(table::TableStateKey const&, table::ViewOperation const&,
                                                        std::string) override {
        auto [completion, promise] = morph::async::Completion<table::SaveResult>::makeSettleable(_owner);
        promise.resolve(table::SaveResult{.outcome = table::SaveOutcome::Refused, .revision = {}, .reason = "no"});
        return std::move(completion);
    }

    void answerLoad(std::optional<std::string> json) {
        REQUIRE_FALSE(loads.empty());
        auto promise = std::move(loads.front());
        loads.erase(loads.begin());
        promise.resolve(json ? std::optional{table::StoredTableState{.json = *json, .revision = "r0"}} : std::nullopt);
    }
    void failLoad() {
        REQUIRE_FALSE(loads.empty());
        auto promise = std::move(loads.front());
        loads.erase(loads.begin());
        promise.reject(std::make_exception_ptr(std::runtime_error{"store down"}));
    }
    void answerSave(table::SaveOutcome outcome, std::string reason = {}) {
        REQUIRE_FALSE(saves.empty());
        auto promise = std::move(saves.front());
        saves.erase(saves.begin());
        promise.resolve(table::SaveResult{.outcome = outcome, .revision = "r1", .reason = std::move(reason)});
    }
    void failSave() {
        REQUIRE_FALSE(saves.empty());
        auto promise = std::move(saves.front());
        saves.erase(saves.begin());
        promise.reject(std::make_exception_ptr(std::runtime_error{"disk full"}));
    }

    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): a test double the test steers directly.
    std::vector<LoadPromise> loads;
    std::vector<SavePromise> saves;
    std::vector<std::string> saved;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

private:
    morph::exec::IExecutor* _owner;
};

table::StateDocument sortedBy(std::string column) {
    table::StateDocument document;
    document.sort = table::SortChain{{.column = std::move(column), .dir = table::SortDirection::Ascending}};
    return document;
}

table::StateDocument layoutOf(std::vector<std::string> order) {
    table::StateDocument document;
    document.layout = table::ColumnLayoutValue{};
    document.layout->order = std::move(order);
    return document;
}

table::StateDocument read(std::string const& json) {
    auto const parsed = table::readState(json);
    REQUIRE(parsed.has_value());
    return *parsed;
}

struct Fixture {
    RowsFixture h;
    std::shared_ptr<SteeredStore> store = std::make_shared<SteeredStore>(h.owner);
    TableStates states{store, h.owner, nullptr, 500ms};
    std::vector<std::string> failures;
    Fixture() {
        states.onSaveFailed([this](table::TableStateKey const&, table::SaveResult const& result) {
            failures.push_back(result.reason);
        });
    }
};
}  // namespace

TEST_CASE("cov table states: a null store is refused", "[ui][state][cov]") {
    RowsFixture h;
    CHECK_THROWS_AS((TableStates{nullptr, h.owner, nullptr, 500ms}), std::invalid_argument);
}

TEST_CASE("cov table states: loads of one key share a store call and later ones read the kept copy",
          "[ui][state][cov]") {
    Fixture f;
    auto const key = keyOf("a");
    CHECK_FALSE(f.states.kept(key).has_value());
    std::vector<std::string> got;
    auto first = f.states.load(key);
    first.then([&got](std::optional<table::StoredTableState> const& stored) {
        got.push_back(stored ? stored->json : "none");
    });
    auto second = f.states.load(key);
    second.then([&got](std::optional<table::StoredTableState> const& stored) {
        got.push_back(stored ? stored->json : "none");
    });
    f.h.settle();
    REQUIRE(f.store->loads.size() == 1);
    f.store->answerLoad(table::writeState(sortedBy("x")));
    f.h.settle();
    CHECK(got.size() == 2);
    REQUIRE(f.states.kept(key).has_value());
    auto third = f.states.load(key);
    third.then([&got](std::optional<table::StoredTableState> const&) { got.emplace_back("third"); });
    f.h.settle();
    CHECK(f.store->loads.empty());
    CHECK(got.back() == "third");
}

TEST_CASE("cov table states: a change before the load answers is merged over what is stored", "[ui][state][cov]") {
    Fixture f;
    auto const key = keyOf("a");
    auto pending = f.states.load(key);
    pending.then([](std::optional<table::StoredTableState> const&) {});
    f.states.changed(key, sortedBy("mine"), StateParts::Sort);
    CHECK(f.store->saves.empty());  // not settled: nothing to merge over yet
    CHECK(f.store->loads.size() == 1);
    f.store->answerLoad(table::writeState(layoutOf({"b", "a"})));
    f.h.settle();
    REQUIRE(f.store->saved.size() == 1);
    auto const saved = read(f.store->saved.front());
    REQUIRE(saved.sort.has_value());
    CHECK(saved.sort->front().column == "mine");
    REQUIRE(saved.layout.has_value());
    CHECK(saved.layout->order == std::vector<std::string>{"b", "a"});
}

TEST_CASE("cov table states: a change to an unknown key starts its load, one to a kept key saves at once",
          "[ui][state][cov]") {
    Fixture f;
    auto const unknown = keyOf("u");
    f.states.changed(unknown, sortedBy("x"), StateParts::Sort);
    CHECK(f.store->loads.size() == 1);
    f.states.changed(unknown, sortedBy("y"), StateParts::Sort);
    CHECK(f.store->loads.size() == 1);  // the load in flight is the one answer
    f.store->answerLoad(std::nullopt);
    f.h.settle();
    REQUIRE(f.store->saved.size() == 1);
    CHECK(read(f.store->saved.front()).sort->front().column == "y");

    auto const kept = keyOf("k");
    f.states.keep(kept, table::StoredTableState{.json = table::writeState(layoutOf({"z"})), .revision = "r9"});
    f.states.changed(kept, sortedBy("q"), StateParts::Sort);
    CHECK(f.store->loads.empty());
    REQUIRE(f.store->saved.size() == 2);
    auto const saved = read(f.store->saved.back());
    CHECK(saved.layout->order == std::vector<std::string>{"z"});
    CHECK(saved.sort->front().column == "q");
}

TEST_CASE("cov table states: a failed load rejects its waiters and a later change still saves", "[ui][state][cov]") {
    Fixture f;
    auto const key = keyOf("a");
    std::string error;
    auto pending = f.states.load(key);
    pending.then([](std::optional<table::StoredTableState> const&) {});
    pending.onError([&error](std::exception_ptr const& failure) {
        try {
            std::rethrow_exception(failure);
        } catch (std::exception const& caught) {
            error = caught.what();
        }
    });
    f.store->failLoad();
    f.h.settle();
    CHECK(error == "store down");
    // Nothing was stored or saved: the next load that succeeds settles the key over what is stored.
    auto again = f.states.load(key);
    again.then([](std::optional<table::StoredTableState> const&) {});
    f.h.settle();
    REQUIRE(f.store->loads.size() == 1);
    f.store->answerLoad(table::writeState(layoutOf({"s"})));
    f.h.settle();
    f.states.changed(key, sortedBy("x"), StateParts::Sort);
    REQUIRE(f.store->saved.size() == 1);
    auto const saved = read(f.store->saved.front());
    CHECK(saved.sort->front().column == "x");
    CHECK(saved.layout->order == std::vector<std::string>{"s"});
    CHECK(f.states.kept(key).has_value());
}

TEST_CASE("cov table states: a refused or failed save reaches the handler and keeps the previous copy",
          "[ui][state][cov]") {
    Fixture f;
    auto const key = keyOf("a");
    f.states.keep(key, table::StoredTableState{.json = table::writeState(layoutOf({"z"})), .revision = "r0"});
    f.states.changed(key, sortedBy("x"), StateParts::Sort);
    f.store->answerSave(table::SaveOutcome::Refused, "quota");
    f.h.settle();
    CHECK(f.failures == std::vector<std::string>{"quota"});
    CHECK(f.states.kept(key)->revision == "r0");
    f.states.changed(key, sortedBy("y"), StateParts::Sort);
    f.store->failSave();
    f.h.settle();
    REQUIRE(f.failures.size() == 2);
    CHECK(f.failures.back().contains("disk full"));
    f.states.changed(key, sortedBy("w"), StateParts::Sort);
    f.store->answerSave(table::SaveOutcome::Saved);
    f.h.settle();
    CHECK(f.failures.size() == 2);
    CHECK(f.states.kept(key)->revision == "r1");
    CHECK(read(f.states.kept(key)->json).sort->front().column == "w");
}

TEST_CASE("cov table states: a change during a save in flight is one more save after it", "[ui][state][cov]") {
    Fixture f;
    auto const key = keyOf("a");
    f.states.keep(key, table::StoredTableState{.json = {}, .revision = "r0"});
    f.states.changed(key, sortedBy("x"), StateParts::Sort);
    REQUIRE(f.store->saved.size() == 1);
    f.states.changed(key, sortedBy("y"), StateParts::Sort);
    CHECK(f.store->saved.size() == 1);  // the first is still out
    f.store->answerSave(table::SaveOutcome::Saved);
    f.h.settle();
    REQUIRE(f.store->saved.size() == 2);
    CHECK(read(f.store->saved.back()).sort->front().column == "y");
}

TEST_CASE("cov table states: changes within the debounce are one save, and flush does not wait for it",
          "[ui][state][cov]") {
    RowsFixture h;
    auto store = std::make_shared<SteeredStore>(h.owner);
    TableStates states{store, h.owner, &h.scheduler, 500ms};
    auto const key = keyOf("a");
    states.keep(key, table::StoredTableState{.json = {}, .revision = "r0"});
    states.changed(key, sortedBy("x"), StateParts::Sort);
    h.advance(300ms);
    states.changed(key, sortedBy("y"), StateParts::Sort);
    h.advance(300ms);
    CHECK(store->saved.empty());  // the second change restarted the wait
    h.advance(300ms);
    REQUIRE(store->saved.size() == 1);
    CHECK(read(store->saved.back()).sort->front().column == "y");
    store->answerSave(table::SaveOutcome::Saved);
    h.settle();
    states.changed(key, sortedBy("z"), StateParts::Sort);
    states.flush(key);
    REQUIRE(store->saved.size() == 2);
    CHECK(read(store->saved.back()).sort->front().column == "z");
    states.flush(keyOf("never"));  // an unknown key has nothing to flush
    CHECK(store->saved.size() == 2);
    h.advance(1000ms);
    CHECK(store->saved.size() == 2);  // the flushed change's timer was cancelled
}

TEST_CASE("cov table states: start replaces the settled document only while nothing is unsaved", "[ui][state][cov]") {
    Fixture f;
    auto const key = keyOf("a");
    f.states.start(key, layoutOf({"ignored"}));  // an unknown key keeps nothing
    f.states.keep(key, table::StoredTableState{.json = {}, .revision = "r0"});
    f.states.start(key, layoutOf({"first"}));  // keep() settled the key: taken
    f.states.changed(key, sortedBy("x"), StateParts::Sort);
    REQUIRE(f.store->saved.size() == 1);
    CHECK(read(f.store->saved.back()).layout->order == std::vector<std::string>{"first"});
    f.store->answerSave(table::SaveOutcome::Saved);
    f.h.settle();
    f.states.start(key, layoutOf({"started"}));
    f.states.changed(key, sortedBy("y"), StateParts::Sort);
    REQUIRE(f.store->saved.size() == 2);
    auto const saved = read(f.store->saved.back());
    CHECK(saved.layout->order == std::vector<std::string>{"started"});
    CHECK(saved.sort->front().column == "y");
    // A document handed over while a change waits to be saved is ignored.
    f.states.changed(key, sortedBy("w"), StateParts::Sort);  // save in flight: dirty
    f.states.start(key, layoutOf({"late"}));
    f.store->answerSave(table::SaveOutcome::Saved);
    f.h.settle();
    REQUIRE(f.store->saved.size() == 3);
    CHECK(read(f.store->saved.back()).layout->order == std::vector<std::string>{"started"});
}

TEST_CASE("cov table states: each part of a change takes only its own members", "[ui][state][cov]") {
    Fixture f;
    auto const key = keyOf("a");
    f.states.keep(key, table::StoredTableState{.json = {}, .revision = "r0"});
    table::StateDocument full = layoutOf({"a", "b"});
    full.sort = table::SortChain{{.column = "a", .dir = table::SortDirection::Descending}};
    full.filters = table::FilterSpec{};
    full.filters->columns["a"] = table::ColumnFilter{};
    full.view = "mine";
    f.states.changed(key, full, StateParts::Layout | StateParts::View);
    REQUIRE(f.store->saved.size() == 1);
    auto const first = read(f.store->saved.back());
    CHECK(first.layout.has_value());
    CHECK(first.view == "mine");
    CHECK_FALSE(first.sort.has_value());
    CHECK_FALSE(first.filters.has_value());
    f.store->answerSave(table::SaveOutcome::Saved);
    f.h.settle();
    f.states.changed(key, full, StateParts::Sort | StateParts::Filters);
    REQUIRE(f.store->saved.size() == 2);
    auto const second = read(f.store->saved.back());
    CHECK(second.sort.has_value());
    CHECK(second.filters.has_value());
    CHECK(second.layout.has_value());  // kept from before
}

TEST_CASE("cov table states: keep over a settled key leaves its document alone", "[ui][state][cov]") {
    Fixture f;
    auto const key = keyOf("a");
    f.states.keep(key, table::StoredTableState{.json = {}, .revision = "r0"});
    f.states.changed(key, sortedBy("x"), StateParts::Sort);
    f.store->answerSave(table::SaveOutcome::Saved);
    f.h.settle();
    f.states.keep(key, table::StoredTableState{.json = table::writeState(layoutOf({"other"})), .revision = "r7"});
    CHECK(f.states.kept(key)->revision == "r7");
    f.states.changed(key, sortedBy("y"), StateParts::Sort);
    REQUIRE(f.store->saved.size() == 2);
    CHECK_FALSE(read(f.store->saved.back()).layout.has_value());  // the settled document, not the kept text
}
