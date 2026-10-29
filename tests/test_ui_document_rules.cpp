// SPDX-License-Identifier: Apache-2.0

// The loader's rules for a chart, a table, a head and a text's spans: each document breaks one rule and is refused at
// the key that breaks it, and each near miss beside it loads.

#include <catch2/catch_test_macros.hpp>
#include <morph/ui/document.hpp>
#include <string>
#include <string_view>

using morph::ui::loadDocumentText;

namespace {

// A document of @p view, with @p top keys before it.
std::string document(std::string_view view, std::string_view top = "") {
    return std::string{R"({"vocab": "ui/1", "id": "r", )"} + std::string{top} + R"( "view": )" + std::string{view} +
           "}";
}

// Why @p text is refused, as `path: reason`; `loads` when it is not.
std::string refusal(std::string const& text) {
    auto const loaded = loadDocumentText(text);
    return loaded ? std::string{"loads"} : loaded.error().path + ": " + loaded.error().reason;
}

// A chart of one series, with @p extra keys on the chart and on its series.
std::string chart(std::string_view extra, std::string_view seriesExtra = "", std::string_view lines = "[]") {
    return document(std::string{R"({"kind": "chart", "summary": "s", "series": [{"points": {"lit": []},
        "y": {"row": "v"})"} +
                    std::string{seriesExtra} + "}], \"lines\": " + std::string{lines} + std::string{extra} + "}");
}

// A table over rows with one sortable column `a` and one plain column `b`, with @p extra keys.
std::string rowsTable(std::string_view extra, std::string_view top = "") {
    return document(std::string{R"({"kind": "table", "rows": {"lit": []}, "columns": [
        {"id": "a", "label": "A", "sortable": true}, {"id": "b", "label": "B"}])"} +
                        std::string{extra} + "}",
                    top);
}

constexpr std::string_view kModels = R"("models": {"sample": {"type": "Lims_SampleModel"}},)";

// A model-backed table with @p extra keys.
std::string modelTable(std::string_view extra) {
    return document(
        std::string{R"({"kind": "table", "model": "sample", "action": "Lims_ListResults")"} + std::string{extra} + "}",
        kModels);
}

}  // namespace

// Mutation: drop the line count check (seventeen lines load).
TEST_CASE("ui document rules: a chart's lines, places and literal ends", "[ui][document][chart]") {
    std::string lines = "[";
    for (int index = 0; index < 17; ++index) {
        lines += std::string{index == 0 ? "" : ","} + R"({"value": 1})";
    }
    lines += ']';
    CHECK(refusal(chart("", "", lines)) == "/view/lines: a chart draws at most 16 reference lines");
    CHECK(refusal(chart(R"(, "yPlaces": 18)")) == "loads");
    CHECK(refusal(chart(R"(, "yPlaces": 19)")) == "/view/yPlaces: `yPlaces` is a literal integer from 0 to 18");
    CHECK(refusal(chart(R"(, "yPlaces": -1)")) == "/view/yPlaces: `yPlaces` is a literal integer from 0 to 18");
    CHECK(refusal(chart(R"(, "yPlaces": "two")")) == "/view/yPlaces: `yPlaces` is a literal integer from 0 to 18");
    CHECK(refusal(document(R"({"kind": "chart", "summary": "s", "yPlaces": {"ref": "n"},
        "series": [{"points": {"lit": []}, "y": {"row": "v"}}]})",
                           R"("state": {"n": {"init": 1}},)")) ==
          "/view/yPlaces: `yPlaces` is a literal integer from 0 to 18");
    CHECK(refusal(chart(R"(, "yMin": 10, "yMax": 5)")) == "/view/yMin: `yMin` is above `yMax`");
    CHECK(refusal(chart(R"(, "yMin": 5, "yMax": 10)")) == "loads");
    // Ends that are not both literal numbers are checked when they are evaluated.
    CHECK(refusal(chart(R"(, "yMin": "ten", "yMax": 5)")) == "loads");
    CHECK(refusal(chart(R"(, "yMin": 10)")) == "loads");
}

// Mutation: let a literal tone name anything (`loud` loads).
TEST_CASE("ui document rules: a literal tone on a chart's series or line names one of the five",
          "[ui][document][chart]") {
    CHECK(refusal(chart("", R"(, "tone": "loud")")) ==
          "/view/series/0/tone: `tone` is `neutral`, `info`, `ok`, `warn` or `err`");
    CHECK(refusal(chart("", R"(, "tone": 3)")) ==
          "/view/series/0/tone: `tone` is `neutral`, `info`, `ok`, `warn` or `err`");
    CHECK(refusal(chart("", R"(, "tone": null)")) == "loads");
    CHECK(refusal(chart("", R"(, "tone": "warn")")) == "loads");
    CHECK(refusal(chart("", "", R"([{"value": 1, "tone": "loud"}])")) ==
          "/view/lines/0/tone: `tone` is `neutral`, `info`, `ok`, `warn` or `err`");
}

// Mutation: skip checkLiteralSort (each malformed sort loads).
TEST_CASE("ui document rules: a literal sort names sortable columns, each once, with a direction",
          "[ui][document][table]") {
    CHECK(refusal(rowsTable(R"(, "sort": {"lit": [{"column": "a", "direction": "desc"}]})")) == "loads");
    CHECK(refusal(rowsTable(R"(, "sort": null)")) == "loads");
    CHECK(refusal(rowsTable(R"(, "sort": {"lit": "a"})")).starts_with("/view/sort: a table's sort is a list of keys"));
    CHECK(refusal(rowsTable(R"(, "sort": {"lit": [{"column": "a", "direction": "up"}]})"))
              .starts_with("/view/sort: a sort key is"));
    CHECK(refusal(rowsTable(R"(, "sort": {"lit": [{"column": "a"}]})")).starts_with("/view/sort: a sort key is"));
    CHECK(refusal(rowsTable(R"(, "sort": {"lit": [{"column": 1, "direction": "asc"}]})"))
              .starts_with("/view/sort: a sort key is"));
    CHECK(refusal(rowsTable(R"(, "sort": {"lit": [{"column": "b", "direction": "asc"}]})")) ==
          "/view/sort: the sort names `b`, which is not a sortable column");
    CHECK(refusal(rowsTable(
              R"(, "sort": {"lit": [{"column": "a", "direction": "asc"}, {"column": "a", "direction": "desc"}]})")) ==
          "/view/sort: the sort names `a` twice");
}

// Mutation: drop the model-backed checks (each of these tables loads).
TEST_CASE("ui document rules: a model-backed table's engine sorts, groups, pages and selects",
          "[ui][document][table]") {
    CHECK(refusal(modelTable("")) == "loads");
    CHECK(refusal(document(R"({"kind": "table", "model": "sample", "action": "Lims_ListResults",
        "columns": [{"id": "a", "label": "A", "sortable": true}]})",
                           kModels))
              .starts_with("/view/columns/0/sortable: a model-backed table's engine sorts"));
    CHECK(refusal(modelTable(R"(, "group": {"by": {"row": "a"}, "heading": {"kind": "text", "text": "g"}})"))
              .starts_with("/view/group: a model-backed table's engine shapes its rows"));
    CHECK(
        refusal(modelTable(R"(, "pageSize": 10)")).starts_with("/view/pageSize: a model-backed table's engine pages"));
    CHECK(refusal(modelTable(R"(, "page": 1)")).starts_with("/view/page: a model-backed table's engine pages"));
    CHECK(refusal(modelTable(R"(, "selectionMode": "multiple", "selection": {"lit": []})"))
              .starts_with("/view/selection: a model-backed table's engine owns its selection"));
}

// Mutation: accept any literal pageSize (0 loads).
TEST_CASE("ui document rules: a table's page holds a positive whole number of rows", "[ui][document][table]") {
    CHECK(refusal(rowsTable(R"(, "pageSize": 10, "page": 0)")) == "loads");
    CHECK(refusal(rowsTable(R"(, "pageSize": null)")) == "loads");
    CHECK(refusal(rowsTable(R"(, "pageSize": 0)")) == "/view/pageSize: a page holds a positive whole number of rows");
    CHECK(refusal(rowsTable(R"(, "pageSize": "ten")")) ==
          "/view/pageSize: a page holds a positive whole number of rows");
    CHECK(refusal(rowsTable(R"(, "page": 1)")).starts_with("/view/page: `page` picks a page of `pageSize` rows"));
}

// Mutation: accept a select column on any selection mode (`single` loads).
TEST_CASE("ui document rules: a select column and a bound selection need the modes that draw them",
          "[ui][document][table]") {
    CHECK(refusal(rowsTable(R"(, "selectColumn": true, "selectionMode": "multiple")")) == "loads");
    CHECK(refusal(rowsTable(R"(, "selectColumn": true, "selectionMode": "single")"))
              .starts_with("/view/selectColumn: a select column draws a table's selection of several rows"));
    CHECK(refusal(rowsTable(R"(, "selectColumn": true)")).starts_with("/view/selectColumn: a select column draws"));
    CHECK(refusal(rowsTable(R"(, "selectionMode": "none", "selection": {"lit": []})"))
              .starts_with("/view/selection: a table with `selection` selects rows"));
    CHECK(
        refusal(rowsTable(R"(, "selection": {"lit": []})")).starts_with("/view/selection: a table with `selection`"));
    CHECK(refusal(rowsTable(R"(, "selectionMode": {"ref": "mode"}, "selection": {"lit": []})",
                            R"("state": {"mode": {"init": "single"}},)")) == "loads");
}

// Mutation: drop the literal `selected` check (a tab id no tab has loads).
TEST_CASE("ui document rules: a literal selected tab names a tab", "[ui][document]") {
    constexpr std::string_view kTabs = R"({"kind": "tabs", "selected": %, "tabs": [
        {"id": "one", "label": "One", "children": [{"kind": "spacer"}]},
        {"id": "two", "label": "Two", "children": [{"kind": "spacer"}]}]})";
    auto const tabs = [kTabs](std::string_view selected) {
        std::string view{kTabs};
        view.replace(view.find('%'), 1, selected);
        return document(view);
    };
    CHECK(refusal(tabs(R"("two")")) == "loads");
    CHECK(refusal(tabs("null")) == "loads");
    CHECK(refusal(tabs(R"("three")")) == "/view/selected: `selected` names no tab of this node");
    CHECK(refusal(tabs("2")) == "/view/selected: `selected` names no tab of this node");
}

// Mutation: let a search take a select's keys (`options` on a search loads).
TEST_CASE("ui document rules: a head tool takes its own kind's keys and a positive width", "[ui][document]") {
    auto const tool = [](std::string_view keys) {
        return document(R"({"kind": "spacer"})",
                        std::string{R"("tools": [{"id": "t", "label": "T", )"} + std::string{keys} + "}],");
    };
    CHECK(refusal(tool(R"("kind": "search", "width": 12)")) == "loads");
    CHECK(refusal(tool(R"("kind": "search", "options": {"lit": ["a"]})")) ==
          "/tools/0/options: `options` is a select tool's");
    CHECK(refusal(tool(R"("kind": "select", "options": {"lit": ["a"]}, "placeholder": "p")")) ==
          "/tools/0/placeholder: `placeholder` is a search tool's");
    CHECK(refusal(tool(R"("kind": "search", "width": 0)")) ==
          "/tools/0/width: `width` is a literal whole number of units, 1 or more");
    CHECK(refusal(tool(R"("kind": "search", "width": "wide")")) ==
          "/tools/0/width: `width` is a literal whole number of units, 1 or more");
}

// Mutation: let a submenu take an id (it loads).
TEST_CASE("ui document rules: a head menu's submenu and separator take no id", "[ui][document]") {
    CHECK(refusal(document(R"({"kind": "spacer"})", R"("menu": [{"id": "export", "label": "Export",
        "items": [{"id": "csv", "label": "CSV"}]}],)")) ==
          "/menu/0/id: a submenu is opened, not chosen, and takes no `id`");
    CHECK(refusal(document(R"({"kind": "spacer"})", R"("menu": [{"id": "line", "separator": true}],)")) ==
          "/menu/0/id: a separator is a line between entries and takes no other key");
}

// Mutation: skip the literal spans check (a number for spans loads).
TEST_CASE("ui document rules: literal spans are a list of runs, each a string or a record of text and flags",
          "[ui][document]") {
    auto const text = [](std::string_view spans) {
        return document(R"({"kind": "text", "spans": )" + std::string{spans} + "}");
    };
    CHECK(refusal(text(R"({"lit": ["Lot ", {"text": "L-1", "mono": true}]})")) == "loads");
    CHECK(refusal(text(R"({"lit": 7})")) == "/view/spans: `spans` is a list of runs");
    CHECK(refusal(text(R"({"lit": [7]})")) == "/view/spans: a run is a string or a record with `text`");
    CHECK(refusal(text(R"({"lit": [{"strong": true}]})")) == "/view/spans: a run's record gives its `text`");
    CHECK(refusal(text(R"({"lit": [{"text": "x", "loud": true}]})")) ==
          "/view/spans: a run takes `text`, `strong` and `mono`, not `loud`");
    CHECK(refusal(text(R"({"lit": [{"text": "x", "strong": 1}]})")) ==
          "/view/spans: a run's `strong` is true or false");
    CHECK(refusal(text(R"({"list": [{"lit": [1]}]})")) ==
          "/view/spans/list/0: a run is a string or a record with `text`");
    CHECK(refusal(text(R"({"list": [{"obj": {"text": "x", "loud": true}}]})")) ==
          "/view/spans/list/0/obj/loud: a run takes `text`, `strong` and `mono`, not `loud`");
}

// Mutation: drop the read-only content check (an input inside an entry then loads).
TEST_CASE("ui document rules: only a list entry holds content, and only display kinds, at any depth",
          "[ui][document]") {
    auto const entry = [](std::string_view style, std::string_view child) {
        return document(std::string{R"({"kind": "button", "label": "Sample", "style": ")"} + std::string{style} +
                        R"(", "selected": false, "children": [)" + std::string{child} + "]}");
    };
    CHECK(refusal(entry("item", R"({"kind": "column", "children": [{"kind": "text", "text": "S-1"}]})")) == "loads");
    CHECK(refusal(entry("push", R"({"kind": "text", "text": "S-1"})")) ==
          "/view/children: only a list entry holds content; a push button has a label");
    CHECK(refusal(entry("item", R"({"kind": "column", "children": [{"kind": "textInput"}]})")) ==
          "/view/children/0/children/0: an entry's content is read-only; put the control beside the entry");
    CHECK(refusal(entry("item", R"({"kind": "switch", "on": 1, "cases": [{"when": 1,
        "children": [{"kind": "checkbox", "label": "c"}]}]})")) ==
          "/view/children/0/cases/0/children/0: an entry's content is read-only; put the control beside the entry");
}

// Mutation: let a text area take an emphasis (`prominent` then loads).
TEST_CASE("ui document rules: a text area takes no emphasis", "[ui][document]") {
    CHECK(refusal(document(R"({"kind": "textInput", "mode": "multiline", "emphasis": "normal"})")) == "loads");
    CHECK(refusal(document(R"({"kind": "textInput", "mode": "multiline", "emphasis": "prominent"})")) ==
          "/view/emphasis: a multiline input's size is its own; it takes no emphasis");
    CHECK(refusal(document(R"({"kind": "textInput", "emphasis": "prominent"})")) == "loads");
}

// Mutation: skip a key-value item's spans (a run without text then loads).
TEST_CASE("ui document rules: a key-value item's spans and a run record's flags are checked as written",
          "[ui][document]") {
    CHECK(refusal(document(R"({"kind": "keyValue", "items": {"list": [{"ref": "x"},
        {"obj": {"label": "Lot", "spans": {"lit": [{"strong": true}]}}}]}})",
                           R"("state": {"x": {"init": null}},)")) ==
          "/view/items/list/1/obj/spans: a run's record gives its `text`");
    CHECK(refusal(document(R"({"kind": "text", "spans": {"list": [{"obj": {"text": "x", "strong": 1}}]}})")) ==
          "/view/spans/list/0/obj/strong: a run's `strong` is true or false");
    CHECK(refusal(document(R"({"kind": "text", "spans": {"list": [{"obj": {"text": "x", "strong": {"ref": "s"}}}]}})",
                           R"("state": {"s": {"init": true}},)")) == "loads");
}

// Mutation: let a separator take a label (it then loads).
TEST_CASE("ui document rules: a menu's separator is true and alone, and its trigger keys need the button style",
          "[ui][document]") {
    auto const menu = [](std::string_view extra, std::string_view entry) {
        return document(std::string{R"({"kind": "menu", "items": [{"label": "One"}, )"} + std::string{entry} + "]" +
                        std::string{extra} + "}");
    };
    CHECK(refusal(menu("", R"({"separator": true})")) == "loads");
    CHECK(refusal(menu("", R"({"separator": false})")) ==
          "/view/items/1/separator: `separator` is `true`; leave it out for an entry");
    CHECK(refusal(menu("", R"({"separator": true, "label": "x"})")) ==
          "/view/items/1: a separator is a line between entries and takes no other key");
    CHECK(refusal(menu("", R"({"separator": true, "visible": false})")) ==
          "/view/items/1: a separator is a line between entries and takes no other key");
    CHECK(refusal(menu(R"(, "emphasis": "ghost")", R"({"label": "Two"})")) ==
          "/view/emphasis: `emphasis` is a button-style menu's trigger; give the menu `style: button`");
    CHECK(refusal(menu(R"(, "style": "button", "a11y": {"name": "More"})", R"({"label": "Two"})")) == "loads");
    CHECK(refusal(menu(R"(, "style": "button")", R"({"label": "Two"})"))
              .starts_with("/view: a button-style menu has a `label` or an `a11y` name"));
}

// Mutation: let the app document's formatting take any key (`time` then loads).
TEST_CASE("ui document rules: the app document's formatting holds a date and a date-and-time pattern",
          "[ui][document]") {
    auto const app = [](std::string_view formatting) {
        return std::string{R"({"vocab": "ui/1", "id": "app", "formatting": )"} + std::string{formatting} + "}";
    };
    CHECK(refusal(app(R"({"date": "dd-MMM-yyyy", "dateTime": "dd-MMM-yyyy HH:mm"})")) == "loads");
    CHECK(refusal(app(R"({"time": "HH:mm"})")) == "/formatting/time: `formatting` takes `date` and `dateTime`");
    CHECK(refusal(app(R"({"date": 7})")) == "/formatting/date: a date pattern is a string, such as `dd-MMM-yyyy`");
    CHECK(refusal(app(R"({"date": "dd-MM-yyyy HH:mm"})")) == "/formatting/date: a date pattern holds no time token");
}

// A known defect, kept failing so it is not lost: the loader reads `"type": "any"` as no type at all, so a state
// declared `any` with an init takes the init's type, and a `set` of another type is refused. The specification
// says only a state *without* a type takes its init's type. When the loader keeps a declared `any`, this passes and
// the `[!shouldfail]` tag comes off.
TEST_CASE("ui document rules: a state declared any keeps any beside its init", "[ui][document][!shouldfail]") {
    CHECK(refusal(document(R"({"kind": "button", "label": "x", "onClick": [{"set": "s", "value": "two"}]})",
                           R"("state": {"s": {"type": "any", "init": 2}},)")) == "loads");
}

// Mutation: drop "display" or "small" from the loader's list of roles (the document is refused).
TEST_CASE("ui document rules: a text's role may be display or small, and no other new name", "[ui][document]") {
    CHECK(refusal(document(R"({"kind": "text", "text": "7", "role": "display"})")) == "loads");
    CHECK(refusal(document(R"({"kind": "text", "text": "x", "role": "small"})")) == "loads");
    CHECK(refusal(document(R"({"kind": "text", "text": "x", "role": "tiny"})")) ==
          "/view/role: `tiny` is not one of the values of `role`");
}
