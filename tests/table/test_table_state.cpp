// SPDX-License-Identifier: Apache-2.0

// The table-state store interface, the memory store, and the state document: reading, fitting to a description
// and writing back what the description lacks.

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

TableColumn stateColumn(std::string const& id, ColumnKind kind, std::string group = {}) {
    return TableColumn{.info = {.id = id, .kind = kind, .comparator = {}, .format = {}, .level = {}},
                       .label = id,
                       .group = std::move(group),
                       .sortable = true,
                       .filterable = true,
                       .shown = true};
}

// `id` (the key), `name`, `alias`, `weight`, each sortable, filterable and shown; one declared group
// `g1 = {name, alias}`. No column `gone`, `a` or `b`, and no group `g2`. `weight` holds measures, and the
// description lists the units `g` and `kg` of dimension `mass`.
TableDescription describedSamples() {
    auto const unit = [](std::string const& id, morph::math::Rational toBase) {
        return UnitInfo{.id = id, .symbol = id, .dimension = "mass", .toBase = toBase, .decimals = 0};
    };
    return describe(TableDescription{
        .id = "samples",
        .label = "Samples",
        .columns = {stateColumn("id", ColumnKind::Integer), stateColumn("name", ColumnKind::Text, "g1"),
                    stateColumn("alias", ColumnKind::Text, "g1"), stateColumn("weight", ColumnKind::Measure)},
        .key = "id",
        .groups = {ColumnGroupInfo{.id = "g1", .label = "Names", .columns = {"name", "alias"}}},
        .levels = {},
        .units = {unit("kg", morph::math::Rational{1, morph::math::DecimalPlaces{0}}),
                  unit("g", morph::math::Rational{morph::math::Numerator{1}, morph::math::Denominator{1000},
                                                  morph::math::DecimalPlaces{3}})},
        .capabilities = {},
        .revision = {}});
}

StateDocument userGroups() {
    return readState(
               R"({"version":1,"layout":{"order":["id","name","alias","weight"],"groups":[{"id":"u1","label":"One","columns":["name","gone"]},{"id":"u2","label":"Two","columns":["name","alias"]}]},"filters":{"columns":{},"groups":[{"columns":["name","gone"],"include":[{"contains":"x"}]},{"columns":["name","alias"],"include":[{"contains":"y"}]}]}})")
        .value();
}

// NOLINTNEXTLINE(bugprone-throwing-static-initialization, cert-err58-cpp, cppcoreguidelines-avoid-non-const-global-variables): the suite's one manual executor; every case drains it and none shares it across threads
morph::testing::StepExecutor executor;

SaveResult saveNow(ITableStateStore& store, TableStateKey const& key, std::string json, std::string expected = {}) {
    std::optional<SaveResult> delivered;
    auto completion = store.save(key, std::move(json), std::move(expected));
    completion.thenDetached([&delivered](SaveResult const& result) { delivered = result; });
    executor.runAll();
    REQUIRE(delivered.has_value());
    return *delivered;
}

std::optional<StoredTableState> loadNow(ITableStateStore& store, TableStateKey const& key) {
    std::optional<std::optional<StoredTableState>> delivered;
    auto completion = store.load(key);
    completion.thenDetached([&delivered](std::optional<StoredTableState> const& result) { delivered = result; });
    executor.runAll();
    REQUIRE(delivered.has_value());
    return *delivered;
}

SaveResult publishNow(ITableStateStore& store, TableStateKey const& key, ViewOperation const& operation,
                      std::string expected) {
    std::optional<SaveResult> delivered;
    auto completion = store.publish(key, operation, std::move(expected));
    completion.thenDetached([&delivered](SaveResult const& result) { delivered = result; });
    executor.runAll();
    REQUIRE(delivered.has_value());
    return *delivered;
}

AddView addNamed(std::string name) {
    return AddView{SavedView{.name = std::move(name), .layout = {}, .sort = {}, .filters = {}}};
}

}  // namespace

// Mutation: fitState returning the document unchanged (the sort, filter and hidden CHECKs fail).
// Mutation: listsUnit returning false (the listed display unit `mass` -> `g` is dropped).
TEST_CASE("table state: stale state is fitted", "[table][state]") {
    TableDescription const d = describedSamples();
    StateDocument const stored = readState(R"({"version":1,
      "layout":{"order":["id","gone","name","alias","weight"],"hidden":["id","name","alias","weight"],"pinned":2,
                "widths":{"gone":9},"units":{"weight":{"mass":"g","ratio":"zz"}}},
      "sort":[{"column":"gone","direction":"asc"},{"column":"name","direction":"desc"}],
      "filters":{"columns":{"gone":{"include":[{"eq":"x"}]},"weight":{"include":[{"gt":"abc"}]}},
                 "groups":[{"group":"g2","include":[{"eq":"x"}]}]}})")
                                     .value();
    REQUIRE(stored.sort->at(1).dir == SortDirection::Descending);  // "direction" was read, not ignored
    FitReport report;
    StateDocument const fitted = fitState(stored, d, report);
    CHECK(fitted.layout->order == std::vector<std::string>{"id", "name", "alias", "weight"});
    CHECK(fitted.layout->hidden.size() < 4);  // every column was hidden: one shows
    CHECK(*fitted.sort == SortChain{{.column = "name", .dir = SortDirection::Descending}});
    CHECK(fitted.filters->columns.empty());
    CHECK(fitted.filters->groups.empty());
    REQUIRE(fitted.layout->units.contains("weight"));
    CHECK_FALSE(fitted.layout->units.at("weight").contains("ratio"));  // unknown unit "zz" dropped
    CHECK(fitted.layout->units.at("weight").at("mass") == "g");        // a unit the description lists is kept
    using W = FitDrop::What;
    CHECK(report.dropped == std::vector<FitDrop>{{W::Column, "gone", true},
                                                 {W::SortKey, "gone", true},
                                                 {W::Filter, "gone", true},
                                                 {W::Filter, "weight", false},
                                                 {W::GroupFilter, "g2", true},
                                                 {W::Unit, "weight/ratio", false}});
    CHECK(fitted.carried.columns == std::vector<CarriedState::Column>{{.id = "gone", .after = "id", .width = 9}});
    std::string const written = writeState(fitted);
    CHECK(written.contains(R"("direction":"desc")"));  // the stored name, not the wire's "dir"
    CHECK_FALSE(written.contains(R"("dir":)"));
    StateDocument const again = readState(written).value();
    CHECK(again.layout->order == std::vector<std::string>{"id", "gone", "name", "alias", "weight"});  // its place kept
    CHECK(again.sort->at(0).column == "gone");  // carried key back at index 0
    CHECK(again.sort->at(1) == SortKey{.column = "name", .dir = SortDirection::Descending});
    CHECK(again.filters->columns.contains("gone"));
    CHECK_FALSE(again.filters->columns.contains("weight"));  // did not compile: gone at the next save
    REQUIRE(again.filters->groups.size() == 1);
    CHECK(again.filters->groups.front().group == "g2");
    FitReport second;  // reading it again carries the same
    CHECK(fitState(again, d, second).carried == fitted.carried);
}

// Mutation: fitState compiling stored filters with no unit registry (the measure filter does not compile and is
// dropped).
TEST_CASE("table state: a stored measure filter is fitted with the description's units", "[table][state][measure]") {
    StateDocument const stored =
        readState(R"({"version":1,"filters":{"columns":{"weight":{"include":[{"gt":"250 g"}]}},"groups":[]}})")
            .value();
    FitReport report;
    StateDocument const fitted = fitState(stored, describedSamples(), report);
    REQUIRE(fitted.filters.has_value());
    CHECK(fitted.filters->columns.contains("weight"));
    CHECK(report.dropped.empty());
}

// Mutation: fitState looking up a user group's filter by `group` id (an empty id finds nothing: u3's filter, whose
// group is dropped for having one column, stays in `filters`).
TEST_CASE("table state: a user group and its filter are fitted together", "[table][state]") {
    FitReport report;
    StateDocument const fitted = fitState(userGroups(), describedSamples(), report);
    REQUIRE(fitted.layout->groups.size() == 1);
    CHECK(fitted.layout->groups.front().id == "u2");
    REQUIRE(fitted.filters->groups.size() == 1);  // u2's, matched by its columns
    CHECK(fitted.filters->groups.front().columns == std::vector<std::string>{"name", "alias"});
    REQUIRE(fitted.carried.groups.size() == 1);
    CHECK(fitted.carried.groups.front().id == "u1");
    REQUIRE(fitted.carried.groupFilters.size() == 1);  // u1's, carried with its group
    CHECK(fitted.carried.groupFilters.front().group.empty());
    CHECK(fitted.carried.groupFilters.front().columns == std::vector<std::string>{"name", "gone"});

    // A user group left with one column, every column present, is gone, and its filter with it.
    StateDocument const single =
        readState(
            R"({"version":1,"layout":{"groups":[{"id":"u3","label":"Three","columns":["weight"]}]},
        "filters":{"groups":[{"columns":["weight"],"include":[{"isEmpty":true}]}]}})")
            .value();
    FitReport dropped;
    StateDocument const alone = fitState(single, describedSamples(), dropped);
    CHECK(alone.layout->groups.empty());
    CHECK(alone.filters->groups.empty());
    CHECK(alone.carried.groupFilters.empty());
}

// The parser's half of a stored document of an unknown version. Mutation: readState accepting an unknown version.
// Mutation: `writeState` rebuilding `hidden` by walking `order` alone (`c3` is dropped).
TEST_CASE("table state: a hidden column the stored order does not name is written back", "[table][state]") {
    auto const read =
        readState(R"({"version":1,"layout":{"order":["c2","c1"],"hidden":["c3","c1"],"pinned":0,"widths":{}}})");
    REQUIRE(read.has_value());
    auto const again = readState(writeState(*read));
    REQUIRE(again.has_value());
    REQUIRE(again->layout.has_value());
    CHECK(again->layout->order == std::vector<std::string>{"c2", "c1"});
    CHECK(again->layout->hidden == std::vector<std::string>{"c1", "c3"});  // `order`'s first, then the rest as listed
}

TEST_CASE("table state: a document that is not JSON or of an unknown version is refused", "[table][state]") {
    CHECK_FALSE(readState("{not json").has_value());
    CHECK_FALSE(readState(R"({"version":2,"sort":[]})").has_value());
    CHECK(readState(R"({"version":1})").has_value());
}

// Mutation: save ignoring `expected` (the third save answers Saved).
TEST_CASE("memory table state store: a save with a stale revision is a conflict", "[table][state]") {
    MemoryTableStateStore store{executor};
    TableStateKey const key{.application = "lims",
                            .model = "Lims_SampleModel",
                            .table = "samples",
                            .qualifier = "samples",
                            .part = "views"};
    CHECK_FALSE(loadNow(store, key).has_value());
    SaveResult const first = saveNow(store, key, R"({"a":1})");
    CHECK(first.outcome == SaveOutcome::Saved);
    CHECK(first.revision == "1");
    SaveResult const second = saveNow(store, key, R"({"b":1})", "1");
    CHECK(second.outcome == SaveOutcome::Saved);
    CHECK(second.revision == "2");
    SaveResult const stale = saveNow(store, key, R"({"c":1})", "1");
    CHECK(stale.outcome == SaveOutcome::Conflict);
    CHECK(stale.revision == "2");
    CHECK(loadNow(store, key)->json == R"({"b":1})");                        // the conflict wrote nothing
    CHECK(saveNow(store, key, R"({"d":1})").outcome == SaveOutcome::Saved);  // no `expected`: replaces
}

// Mutation: AddView over a taken name appending a second view of that name.
TEST_CASE("views document: operations apply to what is stored, and names stay unique", "[table][state]") {
    ViewsDocument doc;
    doc = *applyViewOperation(doc, addNamed("Open orders"), ViewsPart::Own);
    doc = *applyViewOperation(doc, SetDefaultView{"Open orders"}, ViewsPart::Own);
    CHECK(applyViewOperation(doc, addNamed("Open orders"), ViewsPart::Own) == std::unexpected(ViewError::NameTaken));
    doc = *applyViewOperation(doc, addNamed("Late"), ViewsPart::Own);
    CHECK(applyViewOperation(doc, RenameView{"Late", "Open orders"}, ViewsPart::Own) ==
          std::unexpected(ViewError::NameTaken));
    doc = *applyViewOperation(doc, RemoveView{"Open orders"}, ViewsPart::Own);
    CHECK_FALSE(doc.defaultView);
    CHECK(applyViewOperation(doc, SetDefaultOnMount{true}, ViewsPart::Shared) ==
          std::unexpected(ViewError::NotShared));
    CHECK(
        writeViews(doc, ViewsPart::Shared) ==
        R"({"version":1,"views":[{"name":"Late","layout":{"order":[],"hidden":[],"pinned":0,"widths":{},"groups":[],"units":{}},"sort":[],"filters":{"columns":{},"groups":[]}}]})");
    auto const unknown = readViews(R"({"version":2,"views":[]})");
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().code == TableErrorCode::InvalidSpec);
}

// Mutation: a view operation written with the wrong member (`{"name": …}` for remove) does not read back.
TEST_CASE("views document: what is written reads back, and so does every operation", "[table][state]") {
    ViewsDocument doc;
    doc = *applyViewOperation(
        doc,
        AddView{SavedView{.name = "Late",
                          .layout = {},
                          .sort = SortChain{SortKey{.column = "name", .dir = SortDirection::Descending}},
                          .filters = {}}},
        ViewsPart::Own);
    doc = *applyViewOperation(doc, SetDefaultView{"Late"}, ViewsPart::Own);
    doc = *applyViewOperation(doc, SetDefaultOnMount{true}, ViewsPart::Own);
    std::string const own = writeViews(doc, ViewsPart::Own);
    CHECK(own.starts_with(R"({"version":1,"default":"Late","defaultOnMount":true,"views":[{"name":"Late",)"));
    CHECK(own.contains(R"("sort":[{"column":"name","direction":"desc"}])"));
    auto const back = readViews(own);
    REQUIRE(back.has_value());
    CHECK(writeViews(*back, ViewsPart::Own) == own);
    CHECK(back->defaultView == std::optional<std::string>{"Late"});
    CHECK(back->defaultOnMount);

    for (ViewOperation const& operation :
         {ViewOperation{addNamed("A")}, ViewOperation{ReplaceView{SavedView{.name = "A"}}},
          ViewOperation{RenameView{.from = "A", .to = "B"}}, ViewOperation{RemoveView{"B"}},
          ViewOperation{SetDefaultView{"B"}}, ViewOperation{SetDefaultView{std::nullopt}},
          ViewOperation{SetDefaultOnMount{false}}}) {
        std::string const text = writeViewOperation(operation);
        CAPTURE(text);
        auto const read = readViewOperation(text);
        REQUIRE(read.has_value());
        CHECK(writeViewOperation(*read) == text);
        CHECK(read->index() == operation.index());
    }
    CHECK(writeViewOperation(RemoveView{"B"}) == R"({"remove":"B"})");
    CHECK(writeViewOperation(SetDefaultView{std::nullopt}) == R"({"default":null})");
    CHECK_FALSE(readViewOperation(R"({"remove":"B","add":{"name":"C"}})").has_value());
    CHECK_FALSE(readViewOperation(R"({"frobnicate":1})").has_value());
}

// Mutation: MemoryTableStateStore treating kNoDocument as an ordinary revision (the first save answers Conflict).
// Mutation: the constructor's Sharing defaulting to Shared (the default store offers publishing).
TEST_CASE("memory table state store: kNoDocument stores only over nothing, and a default store does not share",
          "[table][state]") {
    MemoryTableStateStore store{executor};
    CHECK_FALSE(store.shares());
    TableStateKey const key{.application = "lims", .model = "", .table = "t", .qualifier = "s", .part = "views"};
    SaveResult const first = saveNow(store, key, R"({"version":1,"views":[]})", std::string{kNoDocument});
    REQUIRE(first.outcome == SaveOutcome::Saved);
    SaveResult const again = saveNow(store, key, R"({"version":1,"views":[]})", std::string{kNoDocument});
    CHECK(again.outcome == SaveOutcome::Conflict);
    CHECK(again.revision == first.revision);
    CHECK(loadNow(store, key)->revision == first.revision);
    SaveResult const refused = publishNow(store, key, addNamed("X"), std::string{kNoDocument});
    CHECK(refused.outcome == SaveOutcome::Refused);
    CHECK(refused.reason == "this store does not share views");
    MemoryTableStateStore shared{executor, Sharing::Shared};
    CHECK(shared.shares());
    auto sharedKey = key;
    sharedKey.part = "shared";
    SaveResult const published = publishNow(shared, sharedKey, addNamed("X"), std::string{kNoDocument});
    REQUIRE(published.outcome == SaveOutcome::Saved);
    CHECK(publishNow(shared, sharedKey, addNamed("Y"), std::string{kNoDocument}).revision ==
          published.revision);  // Conflict
    SaveResult const second = publishNow(shared, sharedKey, addNamed("Y"), published.revision);
    REQUIRE(second.outcome == SaveOutcome::Saved);
    auto const stored = readViews(loadNow(shared, sharedKey)->json);
    REQUIRE(stored.has_value());
    REQUIRE(stored->views.size() == 2);
    CHECK(stored->views[1].name == "Y");
    CHECK(publishNow(shared, sharedKey, SetDefaultView{"X"}, {}).outcome == SaveOutcome::Refused);
}
