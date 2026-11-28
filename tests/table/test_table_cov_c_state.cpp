// SPDX-License-Identifier: Apache-2.0
//
// morph::table state documents: what a save writes back of a state that was fitted, how a stale state is fitted
// to a description, and the store's refusals.

#include <catch2/catch_test_macros.hpp>
#include <expected>
#include <morph/table/state_store.hpp>
#include <morph/testing/step_executor.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace morph::table;

namespace {

TableColumn column(std::string const& id, ColumnKind kind, bool sortable = true) {
    return TableColumn{.info = {.id = id, .kind = kind, .comparator = {}, .format = {}, .level = {}},
                       .label = id,
                       .group = {},
                       .sortable = sortable,
                       .filterable = true,
                       .shown = true};
}

TableDescription sampleDescription() {
    return describe(
        TableDescription{.id = "samples",
                         .label = "Samples",
                         .columns = {column("id", ColumnKind::Integer), column("name", ColumnKind::Text),
                                     column("alias", ColumnKind::Text), column("locked", ColumnKind::Text, false)},
                         .key = "id",
                         .groups = {ColumnGroupInfo{.id = "names", .label = "Names", .columns = {"name", "alias"}}},
                         .levels = {},
                         .units = {},
                         .capabilities = {},
                         .revision = {}});
}

StateDocument stored(std::string_view json) { return readState(json).value(); }

morph::testing::StepExecutor& executor() {
    static morph::testing::StepExecutor instance;
    return instance;
}

template <typename Call>
SaveResult settled(Call&& call) {
    std::optional<SaveResult> delivered;
    auto completion = std::forward<Call>(call)();
    completion.thenDetached([&delivered](SaveResult const& result) { delivered = result; });
    executor().runAll();
    REQUIRE(delivered.has_value());
    return *delivered;
}

}  // namespace

TEST_CASE("a state written with what it carried places each carried column and key back", "[covc][table][state]") {
    StateDocument document;
    document.layout = ColumnLayoutValue{.order = {"id", "name"}, .hidden = {"name"}};
    document.carried.columns = {
        CarriedState::Column{.id = "c1", .after = "id", .hidden = true, .width = 5, .units = {{"mass", "g"}}},
        CarriedState::Column{.id = "c2", .after = "nowhere"},
        CarriedState::Column{.id = "c3", .after = {}},
    };
    document.carried.groups = {ColumnGroupInfo{.id = "u", .label = "U", .columns = {"c1", "c2"}}};
    document.sort = SortChain{SortKey{.column = "id"}, SortKey{.column = "name"}};
    document.carried.sort = {{1, SortKey{.column = "gone", .dir = SortDirection::Descending}},
                             {0, SortKey{.column = "older"}},
                             {9, SortKey{.column = "last"}}};
    document.carried.filters["gone"] =
        ColumnFilter{.combine = Combine::Any, .include = {FilterEntry{.eq = "x"}}, .exclude = {}};
    document.carried.groupFilters = {GroupFilter{.columns = {"c1", "c2"}}};

    auto const back = readState(writeState(document));
    REQUIRE(back);
    REQUIRE(back->layout);
    CHECK(back->layout->order == std::vector<std::string>{"c3", "id", "c1", "name", "c2"});
    CHECK(back->layout->hidden == std::vector<std::string>{"c1", "name"});
    CHECK(back->layout->widths.at("c1") == 5);
    CHECK(back->layout->units.at("c1").at("mass") == "g");
    REQUIRE(back->layout->groups.size() == 1);
    REQUIRE(back->sort);
    std::vector<std::string> sorted;
    for (auto const& key : *back->sort) {
        sorted.push_back(key.column);
    }
    CHECK(sorted == std::vector<std::string>{"older", "gone", "id", "name", "last"});
    REQUIRE(back->filters);
    CHECK(back->filters->columns.contains("gone"));
    CHECK(back->filters->groups.size() == 1);
}

TEST_CASE("a state with only carried parts still writes the parts it carried", "[covc][table][state]") {
    StateDocument onlyGroups;
    onlyGroups.carried.groups = {ColumnGroupInfo{.id = "u", .label = "U", .columns = {"a", "b"}}};
    CHECK(readState(writeState(onlyGroups))->layout->groups.size() == 1);
    StateDocument onlyGroupFilter;
    onlyGroupFilter.carried.groupFilters = {GroupFilter{.columns = {"a", "b"}}};
    CHECK(readState(writeState(onlyGroupFilter))->filters->groups.size() == 1);
    StateDocument const nothing;
    auto const empty = readState(writeState(nothing));
    REQUIRE(empty);
    CHECK_FALSE(empty->layout);
    CHECK_FALSE(empty->sort);
    CHECK_FALSE(empty->filters);
}

TEST_CASE("a state of another version is refused with the versions it knows", "[covc][table][state]") {
    auto const older = readState(R"({"version":0})");
    REQUIRE_FALSE(older);
    CHECK(older.error().contains("version 0 has no migration to version 1"));
    auto const newer = readState(R"({"version":7})");
    REQUIRE_FALSE(newer);
    CHECK(newer.error().contains("version 7; this reads up to 1"));
    auto const wrongShape = readState(R"({"version":1,"layout":5})");
    REQUIRE_FALSE(wrongShape);
    CHECK(wrongShape.error().contains("does not read"));
    auto const notJson = readState("[");
    REQUIRE_FALSE(notJson);
    CHECK(notJson.error().contains("not JSON"));
}

TEST_CASE("fitting a state reports what it carries and what it drops, key by key", "[covc][table][state]") {
    using W = FitDrop::What;
    auto const description = sampleDescription();
    auto const fit = [&](std::string_view json) {
        FitReport report;
        auto fitted = fitState(stored(json), description, report);
        return std::pair{std::move(fitted), std::move(report)};
    };

    SECTION("layout columns that are gone keep their width and units, once each") {
        auto const [fitted, report] = fit(
            R"({"version":1,"layout":{"order":["id","old","old","name"],"hidden":["old"],"widths":{"old":4},
                "units":{"old":{"mass":"g"}}}})");
        REQUIRE(fitted.carried.columns.size() == 1);
        CHECK(fitted.carried.columns[0].id == "old");
        CHECK(fitted.carried.columns[0].after == "id");
        CHECK(fitted.carried.columns[0].hidden);
        CHECK(fitted.carried.columns[0].width == 4);
        CHECK(fitted.carried.columns[0].units.at("mass") == "g");
        CHECK(report.dropped == std::vector<FitDrop>{{W::Column, "old", true}});
    }
    SECTION("a unit no dimension lists is dropped, and a column left with none is dropped whole") {
        auto const [fitted, report] =
            fit(R"({"version":1,"layout":{"order":["id","name"],"units":{"id":{"ratio":"zz"}}}})");
        CHECK(fitted.layout->units.empty());
        CHECK(report.dropped == std::vector<FitDrop>{{W::Unit, "id/ratio", false}});
    }
    SECTION("a sort key on a missing, unsortable or repeated column") {
        auto const [fitted, report] = fit(
            R"({"version":1,"sort":[{"column":"gone","direction":"asc"},{"column":"locked","direction":"asc"},
                {"column":"name","direction":"asc"},{"column":"name","direction":"desc"}]})");
        REQUIRE(fitted.sort);
        CHECK(fitted.sort->size() == 1);
        CHECK(fitted.carried.sort.size() == 1);
        CHECK(report.dropped == std::vector<FitDrop>{{W::SortKey, "gone", true},
                                                     {W::SortKey, "locked", false},
                                                     {W::SortKey, "name", false}});
    }
    SECTION("a filter on a declared group is kept, dropped for not compiling, or carried for a missing group") {
        auto const [fitted, report] = fit(
            R"({"version":1,"filters":{"groups":[{"group":"names","include":[{"contains":"x"}]},
                {"group":"names","include":[{}]},{"group":"missing","include":[{"contains":"x"}]}]}})");
        CHECK(fitted.filters->groups.size() == 1);
        CHECK(fitted.carried.groupFilters.size() == 1);
        CHECK(report.dropped ==
              std::vector<FitDrop>{{W::GroupFilter, "names", false}, {W::GroupFilter, "missing", true}});
    }
    SECTION("a filter on listed columns that no user group owns") {
        auto const [fitted, report] = fit(
            R"({"version":1,"filters":{"groups":[{"columns":["name","alias"],"include":[{"contains":"x"}]},
                {"columns":["name","alias"],"include":[{}]},{"columns":["name","gone"],"include":[{"contains":"x"}]}]}})");
        CHECK(fitted.filters->groups.size() == 1);
        CHECK(fitted.carried.groupFilters.size() == 1);
        CHECK(report.dropped ==
              std::vector<FitDrop>{{W::GroupFilter, "name,alias", false}, {W::GroupFilter, "name,gone", true}});
    }
    SECTION("a filter of a user group is dropped with the group, or when it does not compile") {
        auto const [fitted, report] = fit(
            R"({"version":1,"layout":{"order":["id","name","alias"],"groups":[
                  {"id":"u1","label":"One","columns":["name","alias"]},{"id":"u2","label":"Two","columns":["id","name"]}]},
                "filters":{"groups":[{"columns":["id","name"],"include":[{}]}]}})");
        CHECK(fitted.filters->groups.empty());
        CHECK(report.dropped.size() >= 1);
    }
    SECTION("a column filter that does not compile is dropped, and one that does is kept") {
        auto const [fitted, report] = fit(
            R"({"version":1,"filters":{"columns":{"name":{"include":[{"contains":"x"}]},"alias":{"include":[{}]}}}})");
        CHECK(fitted.filters->columns.size() == 1);
        CHECK(fitted.filters->columns.contains("name"));
        CHECK(report.dropped == std::vector<FitDrop>{{W::Filter, "alias", false}});
    }
}

TEST_CASE("a views document is published only to the shared part, over a document that reads",
          "[covc][table][state]") {
    MemoryTableStateStore store{executor(), Sharing::Shared};
    TableStateKey const own{.application = "a", .model = "m", .table = "t", .qualifier = "q", .part = "views"};
    TableStateKey const shared{.application = "a", .model = "m", .table = "t", .qualifier = "q", .part = "shared"};
    AddView const add{SavedView{.name = "v", .layout = {}, .sort = {}, .filters = {}}};

    auto const toOwn = settled([&] { return store.publish(own, add, {}); });
    CHECK(toOwn.outcome == SaveOutcome::Refused);
    CHECK(toOwn.reason == "views are published to the shared part");

    auto const first = settled([&] { return store.publish(shared, add, std::string{kNoDocument}); });
    CHECK(first.outcome == SaveOutcome::Saved);
    auto const stale = settled([&] { return store.publish(shared, add, "99"); });
    CHECK(stale.outcome == SaveOutcome::Conflict);
    auto const taken = settled([&] { return store.publish(shared, add, first.revision); });
    CHECK(taken.outcome == SaveOutcome::Refused);
    CHECK(taken.reason == "A view named v exists");
    auto const missing = settled([&] { return store.publish(shared, RemoveView{.name = "nope"}, first.revision); });
    CHECK(missing.reason == "No view named nope");
    auto const create = settled([&] { return store.save(shared, "{", {}); });
    REQUIRE(create.outcome == SaveOutcome::Saved);
    auto const garbled = settled([&] { return store.publish(shared, add, create.revision); });
    CHECK(garbled.outcome == SaveOutcome::Refused);
    CHECK(garbled.reason.contains("a views document does not read"));
    CHECK(garbled.revision == create.revision);
    CHECK(settled([&] { return store.save(shared, "{}", std::string{kNoDocument}); }).outcome ==
          SaveOutcome::Conflict);
}

TEST_CASE("a private store refuses to publish", "[covc][table][state]") {
    MemoryTableStateStore store{executor()};
    CHECK_FALSE(store.shares());
    TableStateKey const shared{.application = "a", .model = "m", .table = "t", .qualifier = "q", .part = "shared"};
    auto const result = settled([&] { return store.publish(shared, SetDefaultOnMount{.on = true}, {}); });
    CHECK(result.outcome == SaveOutcome::Refused);
    CHECK(result.reason == "this store does not share views");
}

TEST_CASE("column ids are joined by commas for a report", "[covc][table][state]") {
    CHECK(detail::joinedColumns({}).empty());
    CHECK(detail::joinedColumns({"a"}) == "a");
    CHECK(detail::joinedColumns({"a", "b", "c"}) == "a,b,c");
    CHECK(detail::sameColumns({"a", "b"}, {"b", "a"}));
    CHECK_FALSE(detail::sameColumns({"a", "b"}, {"b", "c"}));
}
