// SPDX-License-Identifier: Apache-2.0
//
// formView on RecordingBackend: submit modes, chrome, grid columns, rules, object and object-array slots, slots by
// kind, display units, reseeding, accessibility and translation, asserted on mounted widgets.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <morph/forms/engine/form_session.hpp>
#include <morph/forms/engine/form_view.hpp>
#include <morph/forms/engine/overrides.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/view.hpp>
#include <morph/util/datetime.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "forms_engine_support.hpp"

namespace {

using morph::forms::FieldKind;
using morph::forms::FieldView;
using morph::forms::FormSession;
using morph::forms::FormViewOptions;
using morph::test::formsengine::Harness;
using morph::ui::testing::RecordingBackend;
namespace ui = morph::ui;

/// A mounted form: the session first, the mount after it, so the mount goes first.
struct Mounted {
    Harness harness;
    RecordingBackend backend;
    std::unique_ptr<FormSession> form;
    std::unique_ptr<ui::Mounted> view;

    explicit Mounted(std::string_view schema, FormViewOptions options = {},
                     morph::forms::FormSessionOptions sessionOptions = {}) {
        form = harness.session("Probe", schema, std::move(sessionOptions));
        view = std::make_unique<ui::Mounted>(harness.rt, backend, morph::forms::formView(*form, std::move(options)));
        harness.settle();
    }

    ~Mounted() { view.reset(); }
    Mounted(Mounted const&) = delete;
    Mounted& operator=(Mounted const&) = delete;
    Mounted(Mounted&&) = delete;
    Mounted& operator=(Mounted&&) = delete;

    [[nodiscard]] int widget(std::string_view kind, std::string_view testId) const {
        auto const found = backend.find(kind, "testId", testId);
        INFO("no " << kind << " with testId " << testId << "\n" << backend.dump());
        REQUIRE(found.has_value());
        return *found;
    }

    [[nodiscard]] bool has(std::string_view kind, std::string_view testId) const {
        return backend.find(kind, "testId", testId).has_value();
    }

    void edit(std::string_view field, std::string text) {
        backend.edit(widget("TextInput", "field_" + std::string{field}), std::move(text));
        harness.settle();
    }
};

constexpr std::string_view kKinds = R"({"properties":{
  "name":{"type":"string","x-order":0},
  "notes":{"type":["string","null"],"x-widget":"textarea","x-order":1},
  "count":{"type":"integer","x-order":2},
  "flag":{"type":"boolean","x-order":3},
  "grade":{"type":"string","enum":["Low","High"],"x-order":4},
  "level":{"type":"string","enum":["A","B"],"x-widget":"radio","x-order":5},
  "when":{"type":["string","null"],"format":"date-time","x-order":6},
  "day":{"type":["string","null"],"format":"date","x-order":7},
  "volume":{"type":["integer","null"],"x-widget":"slider","x-min":0,"x-max":10,"x-step":2,"x-order":8},
  "mass":{"type":["object","null"],"x-decimalPlaces":3,"ExtUnits":{"unitAscii":"kg","unitUnicode":"kg"},
          "x-unitAlternatives":[{"id":"g","display":"g","decimals":1,"num":1,"den":1000}],"x-order":9},
  "since":{"type":["integer","null"],"x-widget":"epochDays","x-order":10}},
  "required":["name","count"]})";

}  // namespace

TEST_CASE("formView: each kind draws its control", "[forms-engine][view]") {
    Mounted const mounted{kKinds};
    CHECK(mounted.has("TextInput", "field_name"));
    CHECK(mounted.backend.prop(mounted.widget("TextInput", "field_notes"), "mode") == "Multiline");
    CHECK(mounted.has("TextInput", "field_count"));
    CHECK(mounted.has("Checkbox", "field_flag"));
    int const grade = mounted.widget("Select", "field_grade");
    CHECK(mounted.backend.prop(grade, "options") == R"(["\"Low\"":Low,"\"High\"":High])");
    CHECK(mounted.backend.prop(mounted.widget("Select", "field_level"), "style") == "Radio");
    CHECK(mounted.backend.prop(mounted.widget("DateTimeInput", "field_when"), "mode") == "DateTime");
    CHECK(mounted.backend.prop(mounted.widget("DateTimeInput", "field_day"), "mode") == "Date");
    CHECK(mounted.backend.prop(mounted.widget("DateTimeInput", "field_since"), "mode") == "Date");
    int const volume = mounted.widget("Slider", "field_volume");
    CHECK(mounted.backend.prop(volume, "range") == "0..10/2");
    CHECK(mounted.has("TextInput", "field_mass"));
    CHECK(mounted.has("Select", "unit_mass"));
}

// Mutation: drop the unit from the number input (the unit is drawn nowhere, and the input reports none).
TEST_CASE("formView: a number's input shows its unit and knows its places; a unit choice keeps its select",
          "[forms-engine][view]") {
    constexpr std::string_view kSchema = R"({"properties":{
      "density":{"type":"number","ExtUnits":{"unitAscii":"kg","unitUnicode":"kg"},"x-displayDecimals":2,"x-order":0},
      "count":{"type":"integer","ExtUnits":{"unitAscii":"pcs","unitUnicode":"pcs"},"x-order":1},
      "note":{"type":"string","x-order":2}}})";
    Mounted const mounted{kSchema};
    int const density = mounted.widget("TextInput", "field_density");
    CHECK(mounted.backend.prop(density, "unit") == "kg");
    CHECK(mounted.backend.prop(density, "decimals") == "2");
    CHECK_FALSE(mounted.backend.find("Text", "text", "kg").has_value());  // no muted text beside the input
    int const count = mounted.widget("TextInput", "field_count");
    CHECK(mounted.backend.prop(count, "unit") == "pcs");
    CHECK(mounted.backend.prop(count, "decimals") == "0");
    int const note = mounted.widget("TextInput", "field_note");
    CHECK_FALSE(mounted.backend.hasProp(note, "unit"));
    CHECK_FALSE(mounted.backend.hasProp(note, "decimals"));

    // A Quantity with alternatives keeps its unit select, and its input names no unit of its own.
    Mounted const kinds{kKinds};
    CHECK(kinds.has("Select", "unit_mass"));
    CHECK_FALSE(kinds.backend.hasProp(kinds.widget("TextInput", "field_mass"), "unit"));
}

TEST_CASE("formView: a multiline input with a unit or places is refused when it mounts", "[forms-engine][view]") {
    morph::test::formsengine::Harness harness;
    RecordingBackend backend;
    CHECK_THROWS_AS((ui::Mounted{harness.rt, backend,
                                 ui::textInput({.mode = ui::TextInputMode::Multiline, .unit = std::string{"kg"}})}),
                    std::logic_error);
    CHECK_THROWS_AS(
        (ui::Mounted{harness.rt, backend, ui::textInput({.mode = ui::TextInputMode::Multiline, .decimals = 2})}),
        std::logic_error);
}

TEST_CASE("formView: labels fall back to the wire key, follow x-order and come from translations",
          "[forms-engine][view]") {
    constexpr std::string_view kSchema = R"({"properties":{
      "zeta":{"type":"string","x-order":0},
      "alpha":{"type":"string","title":"Alpha","x-order":1}},"required":["zeta"]})";
    morph::forms::FormSessionOptions options{};
    options.bcp47 = "de";
    options.translations = [](std::string_view key, std::string_view locale) -> std::optional<std::string> {
        if (key == "Probe.alpha.label" && locale == "de") {
            return "Alfa";
        }
        return std::nullopt;
    };
    Mounted const mounted{kSchema, {}, std::move(options)};
    int const zeta = mounted.widget("Text", "label_zeta");
    int const alpha = mounted.widget("Text", "label_alpha");
    CHECK(mounted.backend.prop(zeta, "text") == "zeta");   // the wire key; the control marks it required
    CHECK(mounted.backend.prop(alpha, "text") == "Alfa");  // the translation beats the schema's title
    CHECK(zeta < alpha);                                   // drawn in x-order
    CHECK(mounted.backend.prop(mounted.widget("TextInput", "field_alpha"), "a11yName") == "Alfa");
}

TEST_CASE("formView: mounted controls drive the session", "[forms-engine][view]") {
    Mounted mounted{kKinds};
    mounted.edit("name", "Ada");
    mounted.edit("count", "9007199254740993");
    mounted.backend.toggle(mounted.widget("Checkbox", "field_flag"));
    mounted.harness.settle();
    mounted.backend.choose(mounted.widget("Select", "field_grade"), ui::Key{std::string{R"("High")"}});
    mounted.harness.settle();
    mounted.edit("mass", "2650.5");
    REQUIRE(mounted.form->body().has_value());
    CHECK(mounted.form->body()->contains(R"("name":"Ada")"));
    CHECK(mounted.form->body()->contains(R"("count":9007199254740993)"));
    CHECK(mounted.form->body()->contains(R"("flag":true)"));
    CHECK(mounted.form->body()->contains(R"("grade":"High")"));
    CHECK(mounted.form->body()->contains(R"("mass":{"num":2650500,"den":1000,"dp":3})"));
    // Switching the unit converts the draft exactly and keeps the value.
    mounted.backend.choose(mounted.widget("Select", "unit_mass"), ui::Key{std::int64_t{1}});
    mounted.harness.settle();
    CHECK(mounted.backend.prop(mounted.widget("TextInput", "field_mass"), "text") == "2650500.0");
    CHECK(mounted.form->body()->contains(R"("mass":{"num":26505000,"den":10000,"dp":3})"));
    // The session drives the controls back: a prefill shows in them.
    mounted.form->prefill(R"({"name":"Grace","count":1,"grade":"Low"})");
    mounted.harness.settle();
    CHECK(mounted.backend.prop(mounted.widget("TextInput", "field_name"), "text") == "Grace");
    CHECK(mounted.form->field("grade").text().peek() == R"("Low")");
}

TEST_CASE(
    "formView: explicit mode adds a Submit button bound to readiness; the cell shows help, the control a live "
    "required mark",
    "[forms-engine][view]") {
    constexpr std::string_view kSchema = R"({"x-submitMode":"explicit","properties":{
      "label":{"type":"string","description":"What it is called","x-order":0},
      "note":{"type":["string","null"],"x-order":1},
      "flag":{"type":["boolean","null"],"x-order":2}},"required":["label"],
      "x-rules":[{"kind":"requiredWhen","fields":["note"],"when":{"kind":"equals","fields":["flag"],"value":true}}]})";
    Mounted mounted{kSchema};
    int const submit = mounted.widget("Button", "form_submit");
    CHECK(mounted.backend.prop(submit, "enabled") == "false");
    CHECK(mounted.backend.find("Text", "text", "What it is called").has_value());
    CHECK(mounted.backend.prop(mounted.widget("Text", "label_note"), "text") == "note");
    mounted.backend.toggle(mounted.widget("Checkbox", "field_flag"));
    mounted.harness.settle();
    // requiredWhen, live: the control is marked, the label keeps the field's label.
    CHECK(mounted.backend.prop(mounted.widget("Text", "label_note"), "text") == "note");
    CHECK(mounted.backend.prop(mounted.widget("TextInput", "field_note"), "required") == "true");
    mounted.edit("label", "x");
    mounted.edit("note", "n");
    CHECK(mounted.backend.prop(submit, "enabled") == "true");
    CHECK(mounted.harness.server.submits().empty());  // explicit: typing submits nothing
    mounted.backend.click(submit);
    mounted.harness.settle();
    REQUIRE(mounted.harness.server.submits().size() == 1);
    CHECK(mounted.harness.server.lastBody() == R"({"label":"x","note":"n","flag":true})");
    int const result = mounted.widget("Text", "form_result");
    CHECK(mounted.backend.prop(result, "visible") == "true");
    CHECK(mounted.backend.prop(result, "text") == R"(ok: {"ok":true})");
}

TEST_CASE("formView: a failed submission shows its error; an automatic form has no Submit button",
          "[forms-engine][view]") {
    constexpr std::string_view kSchema = R"({"properties":{"a":{"type":"integer","x-order":0}},"required":["a"]})";
    Mounted mounted{kSchema};
    CHECK_FALSE(mounted.has("Button", "form_submit"));
    mounted.harness.server.replyWith(
        [](morph::test::formsengine::Call const&) { return std::optional<std::string>{}; });
    mounted.edit("a", "1");
    mounted.harness.server.rejectSubmit(0, "refused");
    mounted.harness.settle();
    int const result = mounted.widget("Text", "form_result");
    CHECK(mounted.backend.prop(result, "text") == "error: refused");
    CHECK(mounted.backend.prop(result, "role") == "Error");
}

TEST_CASE("formView: a field's error shows on its control", "[forms-engine][view]") {
    Mounted mounted{kKinds};
    int const count = mounted.widget("TextInput", "field_count");
    CHECK(mounted.backend.prop(count, "errors") == "[]");
    mounted.edit("count", "4x");
    CHECK(mounted.backend.prop(count, "errors") == "[Not a valid value]");
    mounted.edit("count", "4");
    CHECK(mounted.backend.prop(count, "errors") == "[]");
}

TEST_CASE("formView: visibility and read-only follow the rules on the mounted cells", "[forms-engine][view]") {
    constexpr std::string_view kSchema = R"({"properties":{
      "promo":{"type":["string","null"],"x-order":0},
      "discount":{"type":["integer","null"],"x-order":1},
      "total":{"type":["integer","null"],"x-order":2},
      "secret":{"type":["string","null"],"x-hidden":true,"x-order":3}},"required":[],
      "x-rules":[{"kind":"visibleWhen","fields":["discount"],"when":{"kind":"engaged","fields":["promo"]}},
                 {"kind":"readonlyWhen","fields":["total"],"when":{"kind":"engaged","fields":["promo"]}}]})";
    Mounted mounted{kSchema};
    int const discount = mounted.widget("Column", "cell_discount");
    int const total = mounted.widget("TextInput", "field_total");
    CHECK(mounted.backend.prop(discount, "visible") == "false");
    CHECK(mounted.backend.prop(total, "readonly") == "false");
    CHECK(mounted.backend.prop(mounted.widget("Column", "cell_secret"), "visible") == "false");
    mounted.edit("promo", "SPRING");
    CHECK(mounted.backend.prop(discount, "visible") == "true");
    CHECK(mounted.backend.prop(total, "readonly") == "true");
}

TEST_CASE("formView: sections, tab runs, accordions and the implicit group, each on its grid",
          "[forms-engine][view]") {
    constexpr std::string_view kSchema = R"({"x-layout":{"groups":[
        {"title":"Main","kind":"section"},{"title":"First","kind":"tab"},{"title":"Second","kind":"tab"},
        {"title":"More","kind":"accordion"}]},
      "properties":{
        "a":{"type":"string","x-section":0,"x-colspan":2,"x-order":0},
        "b":{"type":"string","x-section":0,"x-colspan":9,"x-order":1},
        "c":{"type":"string","x-section":1,"x-order":2},
        "d":{"type":"string","x-section":2,"x-order":3},
        "e":{"type":"string","x-section":3,"x-order":4},
        "f":{"type":"string","x-order":5}},"required":[]})";
    Mounted const mounted{kSchema};
    auto const main = mounted.backend.find("Panel", "title", "Main");
    REQUIRE(main.has_value());
    CHECK(mounted.backend.prop(*main, "collapsible") != "true");
    std::vector<int> const tabs = mounted.backend.all("Tabs");
    REQUIRE(tabs.size() == 1);  // the two tab groups share one bar
    CHECK(mounted.backend.prop(tabs.front(), "tabs") == "[First,Second]");
    auto const more = mounted.backend.find("Panel", "title", "More");
    REQUIRE(more.has_value());
    CHECK(mounted.backend.prop(*more, "collapsible") == "true");
    std::vector<int> const grids = mounted.backend.all("Grid");
    REQUIRE(grids.size() >= 2);
    CHECK(mounted.backend.prop(grids.front(), "columns") == "2");
    CHECK(mounted.backend.prop(grids.back(), "columns") == "1");  // the implicit group, flat

    Mounted const wide{kSchema, FormViewOptions{.gridColumns = 3}};
    CHECK(wide.backend.prop(wide.backend.all("Grid").front(), "columns") == "3");
    CHECK(wide.backend.prop(wide.backend.all("Grid").back(), "columns") == "3");  // a host grid widens the flat one
}

TEST_CASE("formView: switching tabs keeps what was typed and submits nothing", "[forms-engine][view]") {
    constexpr std::string_view kSchema = R"({"x-submitMode":"explicit","x-layout":{"groups":[
        {"title":"First","kind":"tab"},{"title":"Second","kind":"tab"}]},
      "properties":{"a":{"type":"string","x-section":0,"x-order":0},"b":{"type":"string","x-section":1,"x-order":1}},
      "required":["a","b"]})";
    Mounted mounted{kSchema};
    mounted.edit("a", "kept");
    int const tabs = mounted.backend.all("Tabs").front();
    mounted.backend.chooseIndex(tabs, 1);
    mounted.harness.settle();
    CHECK(mounted.form->tabSelection(0).peek() == 1);
    mounted.edit("b", "two");
    mounted.backend.chooseIndex(tabs, 0);
    mounted.harness.settle();
    CHECK(mounted.backend.prop(mounted.widget("TextInput", "field_a"), "text") == "kept");
    CHECK(mounted.form->field("a").text().peek() == "kept");
    CHECK(mounted.harness.server.submits().empty());
}

TEST_CASE("formView: an object is a panel of member cells; a collection adds and removes rows",
          "[forms-engine][view]") {
    constexpr std::string_view kSchema = R"({"properties":{
      "address":{"type":["object","null"],"title":"Address","x-order":0,
                 "properties":{"street":{"type":"string","x-order":0}},"required":["street"]},
      "lines":{"type":"array","title":"Lines","x-order":1,
               "items":{"type":"object","properties":{"sku":{"type":"string","x-order":0}},"required":["sku"]}}},
      "required":["lines"]})";
    Mounted mounted{kSchema};
    CHECK(mounted.backend.prop(mounted.widget("Panel", "field_address"), "title") == "Address");
    mounted.edit("address.street", "Main");
    CHECK(mounted.form->body() == R"({"address":{"street":"Main"},"lines":[]})");
    mounted.backend.click(mounted.widget("Button", "add_lines"));
    mounted.harness.settle();
    mounted.backend.click(mounted.widget("Button", "add_lines"));
    mounted.harness.settle();
    REQUIRE(mounted.form->field("lines").rows().size() == 2);
    mounted.edit("lines#1.sku", "A");
    mounted.edit("lines#2.sku", "B");
    CHECK(mounted.form->body() == R"({"address":{"street":"Main"},"lines":[{"sku":"A"},{"sku":"B"}]})");
    CHECK(mounted.backend.find("Panel", "title", "Lines 2").has_value());
    // Removing a mounted row tears its widgets down and renumbers the rest.
    mounted.backend.click(mounted.widget("Button", "remove_lines#1"));
    mounted.harness.settle();
    CHECK_FALSE(mounted.has("TextInput", "field_lines#1.sku"));
    CHECK(mounted.backend.find("Panel", "title", "Lines 1").has_value());
    CHECK_FALSE(mounted.backend.find("Panel", "title", "Lines 2").has_value());
    CHECK(mounted.form->body() == R"({"address":{"street":"Main"},"lines":[{"sku":"B"}]})");
}

TEST_CASE("Overrides: field beats widget beats unit beats kind; a miss draws the built-in control",
          "[forms-engine][view]") {
    auto const marker = [](std::string tag) {
        return [tag = std::move(tag)](FieldView const& view) {
            ui::Text text{.text = tag};
            text.common.testId = "override_" + view.state().path();
            return ui::text(std::move(text));
        };
    };
    FormViewOptions options{};
    options.overrides.byKind(FieldKind::Integer, marker("kind"))
        .byUnit("kg", marker("unit"))
        .byWidget("slider", marker("widget"))
        .byField("count", marker("field"))
        .byKind(FieldKind::Quantity, marker("kind"));
    Mounted const mounted{kKinds, std::move(options)};
    CHECK(mounted.backend.prop(mounted.widget("Text", "override_count"), "text") == "field");
    CHECK(mounted.backend.prop(mounted.widget("Text", "override_volume"), "text") == "widget");
    CHECK(mounted.backend.prop(mounted.widget("Text", "override_mass"), "text") == "unit");
    CHECK(mounted.has("TextInput", "field_name"));  // no override: built in
    CHECK_FALSE(mounted.has("TextInput", "field_count"));
}

TEST_CASE("Overrides: an override receives the field and drives the session like the built-in control",
          "[forms-engine][view]") {
    FormViewOptions options{};
    options.overrides.byField("name", [](FieldView const& view) {
        morph::forms::FieldState* const state = &view.state();
        return ui::row(
            ui::Row{.children = {view.defaultControl(), ui::button(ui::Button{.label = "Fill", .onClick = [state] {
                                                                                  state->text().set("Filled");
                                                                              }})}});
    });
    Mounted mounted{kKinds, std::move(options)};
    auto const fill = mounted.backend.find("Button", "label", "Fill");
    REQUIRE(fill.has_value());
    mounted.backend.click(*fill);
    mounted.harness.settle();
    CHECK(mounted.form->field("name").text().peek() == "Filled");
    CHECK(mounted.backend.prop(mounted.widget("TextInput", "field_name"), "text") == "Filled");
    mounted.edit("count", "2");
    REQUIRE(mounted.form->body().has_value());
    CHECK(mounted.form->body()->contains(R"("name":"Filled")"));
}

TEST_CASE("formView: Choice option keys keep ids past 2^53 exact", "[forms-engine][view]") {
    constexpr std::string_view kSchema = R"({"properties":{"slot":{"type":["integer","null"],"x-order":0,
      "x-optionsAction":"ListRows","x-optionValue":"id","x-optionLabel":"name"}},"required":["slot"]})";
    Harness harness;
    harness.server.optionsWith([](morph::test::formsengine::Call const&) {
        return std::optional<std::string>{
            R"([{"id":9007199254740993,"name":"Alpha"},{"id":9007199254740992,"name":"Beta"}])"};
    });
    RecordingBackend backend;
    auto const form = harness.session("Probe", kSchema);
    {
        ui::Mounted const view{harness.rt, backend, morph::forms::formView(*form)};
        harness.settle();
        auto const select = backend.find("Select", "testId", "field_slot");
        REQUIRE(select.has_value());
        CHECK(backend.prop(*select, "options") == R"(["9007199254740993":Alpha,"9007199254740992":Beta])");
        backend.choose(*select, ui::Key{std::string{"9007199254740993"}});
        harness.settle();
        CHECK(form->body() == R"({"slot":9007199254740993})");
    }
}

// Mutation: make retryOptions do nothing -> the failed fetch is never repeated and the select stays empty.
TEST_CASE("formView: opening a Choice whose options fetch failed fetches them again", "[forms-engine][view]") {
    constexpr std::string_view kSchema = R"({"properties":{"slot":{"type":["integer","null"],"x-order":0,
      "x-optionsAction":"ListRows","x-optionValue":"id","x-optionLabel":"name"}},"required":["slot"]})";
    Harness harness;
    harness.server.optionsWith([](morph::test::formsengine::Call const&) { return std::nullopt; });
    RecordingBackend backend;
    auto const form = harness.session("Probe", kSchema);
    {
        ui::Mounted const view{harness.rt, backend, morph::forms::formView(*form)};
        harness.settle();
        harness.server.rejectFetch(0, "the board is not open yet");
        harness.settle();
        auto const select = backend.find("Select", "testId", "field_slot");
        REQUIRE(select.has_value());
        CHECK(backend.prop(*select, "options") == "[]");
        harness.server.optionsWith([](morph::test::formsengine::Call const&) {
            return std::optional<std::string>{R"([{"id":1,"name":"One"}])"};
        });
        backend.open(*select);
        harness.settle();
        CHECK(harness.server.fetches().size() == 2);
        CHECK(backend.prop(*select, "options") == R"(["1":One])");
        // Options that were fetched are not fetched again by opening.
        backend.open(*select);
        harness.settle();
        CHECK(harness.server.fetches().size() == 2);
    }
}

TEST_CASE("formView: a date-time control's draft and instant convert in the display zone", "[forms-engine][view]") {
    namespace engine = morph::forms::detail::engine;
    CHECK_FALSE(engine::draftInstant("2024-02-30", true, 0).has_value());
    CHECK_FALSE(engine::draftInstant("2024-01-01T25:00", false, 0).has_value());
    auto const day = engine::draftInstant("2024-01-01", true, 60);  // a date ignores the zone
    REQUIRE(day.has_value());
    CHECK(engine::instantDraft(day, true, 60) == "2024-01-01");
    auto const instant = engine::draftInstant("2024-01-01T10:00", false, 60);
    REQUIRE(instant.has_value());
    CHECK(engine::instantDraft(instant, false, 0) == "2024-01-01T09:00:00");  // read at UTC+1, shown at UTC
    CHECK(engine::instantDraft(instant, false, 60) == "2024-01-01T10:00:00");
    CHECK(engine::instantDraft(std::nullopt, false, 0).empty());
    CHECK(engine::instantDraft(morph::time::Timestamp{}, false, 0).empty());
}

// Mutation: ignore `fieldLayout` in fieldCell (every cell stays a column, label over control).
TEST_CASE("formView: an inline layout puts the label beside the control; a stacked one keeps it above",
          "[forms-engine][view]") {
    constexpr std::string_view kSchema = R"({"properties":{
      "name":{"type":"string","x-order":0},
      "flag":{"type":"boolean","x-order":1},
      "note":{"type":"string","description":"Shown to the customer","x-order":2},
      "address":{"type":["object","null"],"title":"Address","x-order":3,
                 "properties":{"street":{"type":"string","x-order":0}}}}})";
    FormViewOptions options;
    options.fieldLayout = morph::forms::FieldLayout::Inline;
    Mounted const inlined{kSchema, std::move(options)};
    RecordingBackend const& backend = inlined.backend;
    int const cell = inlined.widget("Row", "cell_name");
    CHECK(backend.prop(cell, "align") == "center");
    CHECK(backend.prop(cell, "gap") == "3");
    std::vector<int> const line = backend.children(cell);
    REQUIRE(line.size() == 2);
    CHECK(line.at(0) == inlined.widget("Text", "label_name"));
    CHECK(backend.prop(line.at(0), "layout") == "fixed(16)/content");
    CHECK(backend.prop(line.at(1), "layout") == "stretch(1)/content");
    CHECK(backend.children(line.at(1)) == std::vector<int>{inlined.widget("TextInput", "field_name")});
    // A Boolean's checkbox carries its label, so a spacer of the label's width lines its box up.
    std::vector<int> const flag = backend.children(inlined.widget("Row", "cell_flag"));
    REQUIRE(flag.size() == 2);
    CHECK(backend.kindOf(flag.at(0)) == "Spacer");
    CHECK(backend.prop(flag.at(0), "layout") == "fixed(16)/content");
    // The help is a second row, indented by the label's width.
    std::vector<int> const note = backend.children(inlined.widget("Column", "cell_note"));
    REQUIRE(note.size() == 2);
    CHECK(backend.kindOf(note.at(0)) == "Row");
    std::vector<int> const help = backend.children(note.at(1));
    REQUIRE(help.size() == 2);
    CHECK(backend.kindOf(help.at(0)) == "Spacer");
    CHECK(backend.prop(help.at(1), "text") == "Shown to the customer");
    // An object's panel stays stacked; its members follow the form's layout.
    CHECK(inlined.has("Column", "cell_address"));
    CHECK(inlined.has("Row", "cell_address.street"));

    Mounted const stacked{kSchema};
    std::vector<int> const column = stacked.backend.children(stacked.widget("Column", "cell_name"));
    REQUIRE(column.size() == 2);
    CHECK(column.at(0) == stacked.widget("Text", "label_name"));
    CHECK(column.at(1) == stacked.widget("TextInput", "field_name"));
    CHECK_FALSE(stacked.has("Row", "cell_name"));
}

TEST_CASE("formView: an inline label narrower than one unit is refused", "[forms-engine][view]") {
    Harness harness;
    auto const form = harness.session("Probe", R"({"properties":{"name":{"type":"string"}}})");
    FormViewOptions options;
    options.fieldLayout = morph::forms::FieldLayout::Inline;
    options.inlineLabelWidth = 0;
    CHECK_THROWS_AS(morph::forms::formView(*form, std::move(options)), std::invalid_argument);
}
