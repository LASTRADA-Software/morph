// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>
#include <morph/forms/engine/field_model.hpp>
#include <morph/forms/engine/typed_form.hpp>
#include <morph/forms/forms.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// File-scope, not anonymous-namespaced: glaze's reflection needs linkage.
// NOLINTBEGIN(misc-use-internal-linkage)
struct FmBooking {
    std::int64_t guests = 0;
    std::optional<std::string> note;
};
struct FmBooked {
    bool confirmed = false;
};
struct FmModel {
    FmBooked execute(FmBooking const&) { return FmBooked{.confirmed = true}; }
};
// NOLINTEND(misc-use-internal-linkage)

BRIDGE_REGISTER_MODEL(FmModel, "Test_FormsEngineModel_Model")
BRIDGE_REGISTER_ACTION(FmModel, FmBooking, "Test_FormsEngineModel_Booking")

namespace {

using morph::forms::FieldKind;
using morph::forms::FieldSpec;
using morph::forms::FormModel;
using morph::forms::GroupKind;

[[nodiscard]] FormModel model(std::string_view action, std::string_view schema) {
    auto parsed = FormModel::fromSchema(action, schema);
    INFO((parsed ? std::string{} : parsed.error().path + ": " + parsed.error().message));
    REQUIRE(parsed.has_value());
    return *std::move(parsed);
}

[[nodiscard]] std::string names(std::span<FieldSpec const> fields) {
    std::string out{};
    for (auto const& field : fields) {
        out += (out.empty() ? "" : ",") + field.name;
    }
    return out;
}

constexpr std::string_view kProbe = R"({"type":"object","properties":{
  "slot":{"type":["integer","null"],"x-order":0,"title":"Slot","x-optionsAction":"ListSlots","x-optionValue":"id","x-optionLabel":"name"},
  "mass":{"$ref":"#/$defs/q","x-order":1,"x-decimalPlaces":3,"title":"Mass","x-placeholder":"e.g. 1050",
          "x-unitAlternatives":[{"id":"g","display":"g","decimals":1,"num":1,"den":1000},{"id":"t","display":"t","decimals":4,"num":1000,"den":1}]},
  "when":{"type":["string","null"],"format":"date-time","x-order":2,"title":"When","x-readonly":true},
  "note":{"type":["string","null"],"x-order":3,"title":"Notes","x-hidden":true}},
  "$defs":{"q":{"type":["object","null"],"ExtUnits":{"unitAscii":"kg","unitUnicode":"kg"}}},
  "required":["slot","mass","when"]})";

}  // namespace

TEST_CASE("FormModel: kinds, units, the Choice descriptor and flags come from the schema", "[forms-engine][model]") {
    FormModel const probe = model("Probe", kProbe);
    CHECK(names(probe.fields()) == "slot,mass,when,note");
    FieldSpec const& slot = probe.fields()[0];
    CHECK(slot.kind == FieldKind::Choice);
    REQUIRE(slot.choice.has_value());
    CHECK(slot.choice->optionsAction == "ListSlots");
    CHECK(slot.choice->valueField == "id");
    CHECK(slot.choice->labelField == "name");
    CHECK(slot.required);
    CHECK(slot.title == "Slot");
    FieldSpec const& mass = probe.fields()[1];
    CHECK(mass.kind == FieldKind::Quantity);
    CHECK(mass.decimalPlaces == 3U);
    CHECK(mass.unit == "kg");
    REQUIRE(mass.units.size() == 3);
    CHECK(mass.units[1].display == "g");
    CHECK(mass.units[2].num == 1000);
    CHECK(mass.placeholder == "e.g. 1050");
    FieldSpec const& when = probe.fields()[2];
    CHECK(when.kind == FieldKind::DateTime);
    CHECK(when.readOnly);
    FieldSpec const& note = probe.fields()[3];
    CHECK(note.kind == FieldKind::Text);
    CHECK_FALSE(note.required);
    CHECK(note.hidden);
    CHECK(probe.submitMode() == morph::forms::SubmitMode::Automatic);
    CHECK(probe.actionType() == "Probe");
}

TEST_CASE("FormModel: widget hints become Multiline, Slider and a radio Choice", "[forms-engine][model]") {
    FormModel const hints = model("WidgetHintsProbe", R"({"properties":{
      "summary":{"type":["string","null"],"x-order":0,"x-widget":"textarea"},
      "level":{"type":["integer","null"],"x-order":1,"x-widget":"slider","x-min":0,"x-max":100,"x-step":5},
      "mode":{"type":["integer","null"],"x-order":2,"x-widget":"radio","x-optionsAction":"ListModes"},
      "plain":{"type":["string","null"],"x-order":3}},"required":["summary","level","mode"]})");
    CHECK(hints.fields()[0].kind == FieldKind::Multiline);
    FieldSpec const& level = hints.fields()[1];
    CHECK(level.kind == FieldKind::Slider);
    CHECK(level.sliderMin == 0);
    CHECK(level.sliderMax == 100);
    CHECK(level.sliderStep == 5);
    CHECK(hints.fields()[2].kind == FieldKind::Choice);
    CHECK(hints.fields()[2].widget == "radio");
    CHECK(hints.fields()[3].kind == FieldKind::Text);
}

TEST_CASE("FormModel: every renderer kind is named", "[forms-engine][model]") {
    FormModel const kinds = model("T_Kind", R"({"$defs":{"double":{"type":"number"},
      "Row":{"type":"object","properties":{"a":{"type":"integer","x-order":0}},"required":["a"]},
      "Sub":{"type":"object","properties":{"b":{"type":"string","x-order":0}}}},
      "properties":{
      "mass":{"type":["object","null"],"properties":{"num":{"type":"integer"},"den":{"type":"integer"},"dp":{"type":"integer"}},"ExtUnits":{"unitAscii":"kg","unitUnicode":"kg"},"x-decimalPlaces":2,"x-order":0},
      "sample":{"type":"integer","x-optionsAction":"ListSamples","x-order":1},
      "role":{"type":"string","oneOf":[{"title":"A","const":"A"},{"title":"B","const":"B"}],"x-order":2},
      "takenAt":{"type":"string","format":"date-time","x-order":3},
      "day":{"type":"string","format":"date","x-order":4},
      "done":{"type":"boolean","x-order":5},
      "count":{"type":"integer","x-order":6},
      "ratio":{"$ref":"#/$defs/double","x-order":7},
      "note":{"type":"string","x-order":8},
      "tags":{"type":"array","items":{"type":"string"},"x-order":9},
      "rows":{"type":"array","items":{"$ref":"#/$defs/Row"},"x-order":10},
      "sub":{"$ref":"#/$defs/Sub","x-order":11},
      "limit":{"type":"object","properties":{"num":{"type":"integer"},"den":{"type":"integer"},"dp":{"type":"integer"}},"x-decimalPlaces":2,"x-order":12}},
      "required":[]})");
    std::vector<std::string> kindsByField{};
    for (auto const& field : kinds.fields()) {
        kindsByField.push_back(field.name + ":" + std::string{morph::forms::fieldKindName(field.kind)});
    }
    CHECK(kindsByField == std::vector<std::string>{"mass:quantity", "sample:choice", "role:enum", "takenAt:datetime",
                                                   "day:date", "done:boolean", "count:integer", "ratio:number",
                                                   "note:text", "tags:array", "rows:objectArray", "sub:object",
                                                   "limit:decimal"});
}

TEST_CASE("FormModel: a closed set is an Enum in every spelling the emitter or a hand writes",
          "[forms-engine][model]") {
    FormModel const sets = model("Sets", R"({"$defs":{"int64_t":{"type":"integer"},
      "Grade":{"type":"string","oneOf":[{"title":"Low","const":"Low"},{"title":"High","const":"High"}]}},
      "properties":{
      "role":{"type":"string","oneOf":[{"title":"Viewer","const":"Viewer"},{"title":"Member","const":"Member"},{"title":"Manager","const":"Manager"}],"x-order":0,"title":"Role"},
      "size":{"enum":[1,2,3],"x-order":1},
      "nullable":{"oneOf":[{"title":"On","const":"On"},{"title":"Off","const":"Off"},{"type":"null"}],"x-order":2},
      "grade":{"anyOf":[{"$ref":"#/$defs/Grade"},{"type":"null"}],"x-order":3},
      "inlined":{"anyOf":[{"oneOf":[{"title":"Low","const":"Low"},{"title":"High","const":"High"}]},{"type":"null"}],"x-order":4},
      "deeper":{"anyOf":[{"anyOf":[{"oneOf":[{"title":"Low","const":"Low"},{"title":"High","const":"High"}]}]},{"type":"null"}],"x-order":5},
      "partial":{"type":"string","oneOf":[{"title":"A","const":"A"},{"type":"string"}],"x-order":6},
      "optI64":{"anyOf":[{"$ref":"#/$defs/int64_t"},{"type":"null"}],"x-order":7}},"required":[]})");
    auto const options = [&](std::size_t index) {
        std::vector<std::string> out;
        for (auto const& option : sets.fields()[index].enumOptions) {
            out.push_back(option.label + "=" + option.valueJson);
        }
        return out;
    };
    CHECK(sets.fields()[0].kind == FieldKind::Enum);
    CHECK(sets.fields()[0].title == "Role");
    CHECK(options(0) == std::vector<std::string>{R"(Viewer="Viewer")", R"(Member="Member")", R"(Manager="Manager")"});
    CHECK(options(1) == std::vector<std::string>{"1=1", "2=2", "3=3"});
    CHECK(options(2) == std::vector<std::string>{R"(On="On")", R"(Off="Off")"});
    CHECK(sets.fields()[3].kind == FieldKind::Enum);
    CHECK(options(3) == std::vector<std::string>{R"(Low="Low")", R"(High="High")"});
    CHECK(sets.fields()[4].kind == FieldKind::Enum);
    CHECK(sets.fields()[5].kind != FieldKind::Enum);
    CHECK(sets.fields()[5].enumOptions.empty());
    CHECK(sets.fields()[6].kind == FieldKind::Text);
    CHECK(sets.fields()[7].kind == FieldKind::Integer);
}

TEST_CASE("FormModel: key order is irrelevant, x-order is the layout, required and layout read either way",
          "[forms-engine][model]") {
    FormModel const role = model("SetMemberRole", R"({"type":"object","properties":{
      "role":{"type":"string","oneOf":[{"title":"Viewer","const":"Viewer"}],"x-order":2,"title":"Role"},
      "principal":{"type":"string","x-order":1,"title":"Principal"},
      "projectId":{"$ref":"#/$defs/ProjectId","x-order":0,"title":"Project Id"}},
      "$defs":{"ProjectId":{"type":["integer","null"],"minimum":-9223372036854775808,"maximum":9223372036854775807,
               "x-exactMinimum":"-9223372036854775808","x-exactMaximum":"9223372036854775807"}},
      "required":["projectId","principal","role"]})");
    CHECK(names(role.fields()) == "projectId,principal,role");
    CHECK(role.fields()[0].kind == FieldKind::Integer);
    CHECK(role.fields()[0].jsonType == "integer");
    CHECK(role.fields()[0].exactMaximum == "9223372036854775807");
    CHECK(role.fields()[0].minimum == "-9223372036854775808");

    FormModel const reordered = model("X", R"({"properties":{"label":{"type":"string","x-order":1},
      "count":{"type":"integer","x-order":0}},"required":["count","label"]})");
    CHECK(names(reordered.fields()) == "count,label");
    CHECK(reordered.fields()[0].required);
    CHECK(reordered.fields()[1].required);

    FormModel const unbounded =
        model("Unbounded",
              R"({"properties":{"size":{"type":"integer","minimum":null,"maximum":null}},"required":["size"]})");
    CHECK_FALSE(unbounded.fields()[0].minimum.has_value());
    CHECK_FALSE(unbounded.fields()[0].maximum.has_value());
}

TEST_CASE("FormModel: x-layout buckets fields by x-section in x-order, with a trailing implicit group",
          "[forms-engine][model]") {
    FormModel const layout = model("LayoutProbe", R"({"properties":{
      "sampleId":{"type":["integer","null"],"x-order":0,"x-section":0},
      "density":{"type":["number","null"],"x-order":1,"x-section":1},
      "moisture":{"type":["number","null"],"x-order":2,"x-section":1},
      "notes":{"type":["string","null"],"x-order":3,"x-section":2,"x-colspan":2},
      "remarks":{"type":["string","null"],"x-order":4}},"required":["sampleId"],
      "x-layout":{"groups":[{"title":"Identity","kind":"section","fields":["sampleId"]},
        {"title":"Measurement","kind":"section","fields":["moisture","density"]},
        {"title":"Notes","kind":"accordion","fields":["notes"]}]}})");
    auto const groups = layout.groups();
    REQUIRE(groups.size() == 4);
    CHECK(groups[0].title == "Identity");
    CHECK(groups[0].kind == GroupKind::Section);
    CHECK(groups[0].fields == std::vector<std::string>{"sampleId"});
    CHECK(groups[1].fields == std::vector<std::string>{"density", "moisture"});
    CHECK(groups[2].kind == GroupKind::Accordion);
    CHECK(groups[3].implicit);
    CHECK(groups[3].fields == std::vector<std::string>{"remarks"});
    CHECK(layout.find("notes")->colspan == 2);
    CHECK(layout.find("sampleId")->colspan == 1);

    FormModel const flat = model("Probe", kProbe);
    REQUIRE(flat.groups().size() == 1);
    CHECK(flat.groups()[0].implicit);
    CHECK(flat.groups()[0].fields.size() == 4);
}

TEST_CASE("FormModel: a dependent Choice names its parents", "[forms-engine][model]") {
    FormModel const shipTo = model("ShipTo", R"({"properties":{
      "country":{"type":["integer","null"],"x-order":0,"x-optionsAction":"ListCountries","x-optionValue":"id","x-optionLabel":"name"},
      "city":{"type":["integer","null"],"x-order":1,"x-optionsAction":"ListCities","x-optionValue":"id","x-optionLabel":"name","x-optionsDependsOn":["country"]}},
      "required":["country","city"]})");
    CHECK(shipTo.fields()[0].choice->dependsOn.empty());
    CHECK(shipTo.fields()[1].choice->dependsOn == std::vector<std::string>{"country"});
}

TEST_CASE("FormModel: a plain number reads its display unit and decimals; a declared precision wins",
          "[forms-engine][model]") {
    FormModel const reading = model(
        "T_Reading",
        R"({"$defs":{"double":{"type":"number","minimum":-1.7976931348623157e+308,"maximum":1.7976931348623157e+308}},
      "properties":{"density":{"$ref":"#/$defs/double","x-order":0,"title":"Density","ExtUnits":{"unitAscii":"kg/m³","unitUnicode":"kg/m³"},"x-displayDecimals":3},
      "temperature":{"$ref":"#/$defs/double","x-order":1,"title":"Temperature","ExtUnits":{"unitAscii":"°C","unitUnicode":"°C"}}},"required":["density"]})");
    FieldSpec const& density = reading.fields()[0];
    CHECK(density.kind == FieldKind::Number);
    CHECK(density.unit == "kg/m³");
    CHECK(density.unitAscii == "kg/m³");
    CHECK(density.displayDecimals == 3U);
    CHECK_FALSE(reading.fields()[1].displayDecimals.has_value());
    FormModel const precision = model(
        "T_Precision",
        R"({"properties":{"reading":{"type":"number","x-decimalPlaces":1,"x-displayDecimals":3,"x-order":0}},"required":["reading"]})");
    CHECK(precision.fields()[0].kind == FieldKind::Decimal);
    CHECK(precision.fields()[0].decimalPlaces == 1U);
    CHECK_FALSE(precision.fields()[0].displayDecimals.has_value());
}

TEST_CASE("FormModel: nested objects and collections are described recursively, and a cycle stops",
          "[forms-engine][model]") {
    FormModel const ignition =
        model("T_Ignition", R"({"$defs":{"double":{"type":"number"},"int32_t":{"type":"integer"}},
      "properties":{"specimen":{"type":"object","x-order":0,"title":"Specimen","required":["massOfContainer","testTemperature"],"properties":{
        "massOfContainer":{"$ref":"#/$defs/double","x-order":0,"title":"Container","ExtUnits":{"unitAscii":"g","unitUnicode":"g"},"x-displayDecimals":1},
        "readoutBinderContent":{"anyOf":[{"$ref":"#/$defs/double"},{"type":"null"}],"x-order":2,"title":"Readout binder content","ExtUnits":{"unitAscii":"pct","unitUnicode":"%"},"x-displayDecimals":2},
        "testTemperature":{"$ref":"#/$defs/int32_t","x-order":3,"title":"Test Temperature"}}}},"required":["specimen"]})");
    FieldSpec const& specimen = ignition.fields()[0];
    CHECK(specimen.kind == FieldKind::Object);
    CHECK_FALSE(specimen.truncated);
    CHECK(names(specimen.children) == "massOfContainer,readoutBinderContent,testTemperature");
    CHECK(specimen.children[0].title == "Container");
    CHECK(specimen.children[0].unit == "g");
    CHECK(specimen.children[0].displayDecimals == 1U);
    CHECK(specimen.children[0].required);
    CHECK(specimen.children[0].path == "specimen.massOfContainer");
    CHECK_FALSE(specimen.children[1].required);
    CHECK(specimen.children[2].kind == FieldKind::Integer);

    FormModel const grading = model("T_Grading", R"({"$defs":{"double":{"type":"number"},
      "Row":{"type":"object","properties":{"note":{"type":["string","null"],"x-order":2},
        "sieve":{"$ref":"#/$defs/double","x-order":0,"title":"Sieve","ExtUnits":{"unitAscii":"mm","unitUnicode":"mm"}},
        "passing":{"type":["object","null"],"properties":{"num":{"type":"integer"},"den":{"type":"integer"},"dp":{"type":"integer"}},"ExtUnits":{"unitAscii":"pct","unitUnicode":"%"},"x-decimalPlaces":1,"x-order":1},
        "order":{"type":"integer","x-order":3,"x-readonly":true}},"required":["sieve","passing"]}},
      "properties":{"rows":{"type":"array","items":{"$ref":"#/$defs/Row"},"x-order":0},
        "tags":{"type":"array","items":{"type":"string"},"x-order":1}},"required":["rows"]})");
    FieldSpec const& rows = grading.fields()[0];
    CHECK(rows.kind == FieldKind::ObjectArray);
    CHECK(names(rows.children) == "sieve,passing,note,order");
    CHECK(rows.children[0].path == "rows[].sieve");
    CHECK(rows.children[1].kind == FieldKind::Quantity);
    CHECK(rows.children[3].readOnly);
    CHECK(grading.fields()[1].kind == FieldKind::Array);
    CHECK(grading.fields()[1].children.empty());

    FormModel const tree = model("TreeNodeAction", R"({"$defs":{"TreeNode":{"type":"object","properties":{
        "name":{"type":"string","x-order":0},"children":{"type":"array","items":{"$ref":"#/$defs/TreeNode"},"x-order":1}},
        "required":["name","children"]}},
      "properties":{"name":{"type":"string","x-order":0},"children":{"type":"array","items":{"$ref":"#/$defs/TreeNode"},"x-order":1}},
      "required":["name","children"]})");
    FieldSpec const& children = tree.fields()[1];
    CHECK(children.kind == FieldKind::ObjectArray);
    CHECK_FALSE(children.truncated);
    CHECK(names(children.children) == "name,children");
    CHECK(children.children[1].truncated);  // TreeNode is already on the path
    CHECK(children.children[1].children.empty());
}

TEST_CASE("FormModel: an instance-decorated schema reads its precision and exact range", "[forms-engine][model]") {
    FormModel const decorated = model("ICCapture", R"({"properties":{"value":{"type":["object","null"],
      "ExtUnits":{"unitAscii":"mg_per_L","unitUnicode":"mg/L"},"x-order":1,"x-decimalPlaces":1,
      "x-minimum":{"num":0,"den":1,"dp":1},"x-maximum":{"num":20,"den":1,"dp":1}}},"required":["value"]})");
    FieldSpec const& value = decorated.fields()[0];
    CHECK(value.decimalPlaces == 1U);
    REQUIRE(value.instanceMaximum.has_value());
    CHECK(*value.instanceMaximum == morph::math::Rational{morph::math::Numerator{20}, morph::math::Denominator{1},
                                                          morph::math::DecimalPlaces{1}});
}

TEST_CASE("FormModel: a malformed document is a SchemaError", "[forms-engine][model]") {
    CHECK(FormModel::fromSchema("A", "not json").error().path.empty());
    CHECK_FALSE(FormModel::fromSchema("A", "[1,2]").has_value());
    CHECK(FormModel::fromSchema("A", R"({"properties":[]})").error().path == "properties");
    CHECK(FormModel::fromSchema("A", R"({"properties":{"a":3}})").error().path == "properties.a");
    CHECK(FormModel::fromSchema("A", R"({"x-rules":[{"fields":["a"]}]})").error().path == "x-rules[0]");
    CHECK(FormModel::fromSchema("A", R"({"x-layout":{"groups":{}}})").error().path == "x-layout.groups");
    CHECK(FormModel::fromSchema("A", "{}").has_value());
    CHECK(FormModel::fromSchema("A", R"({"x-submitMode":"explicit"})")->submitMode() ==
          morph::forms::SubmitMode::Explicit);
}

// Only the success path: every document `schemaJson<A>()` emits for a reflectable action is one the
// reader accepts (the conformance and rule corpora pin that), and the reader's failures — not JSON, not
// an object, a non-object property, a malformed `x-rules` or `x-layout` — have no spelling in a C++
// action type, so no registered action can reach the throw. Its message is reviewed with the code.
TEST_CASE("FormModel: forAction reads a registered action's own schema, as fromSchema does", "[forms-engine][model]") {
    FormModel const typed = FormModel::forAction<FmBooking>();
    auto const runtime = FormModel::fromSchema("Test_FormsEngineModel_Booking", morph::forms::schemaJson<FmBooking>());
    REQUIRE(runtime.has_value());
    CHECK(typed.actionType() == "Test_FormsEngineModel_Booking");
    CHECK(names(typed.fields()) == "guests,note");
    REQUIRE(typed.fields().size() == runtime->fields().size());
    for (std::size_t i = 0; i < typed.fields().size(); ++i) {
        CHECK(typed.fields()[i].name == runtime->fields()[i].name);
        CHECK(typed.fields()[i].kind == runtime->fields()[i].kind);
        CHECK(typed.fields()[i].required == runtime->fields()[i].required);
    }
    CHECK(typed.find("guests")->kind == FieldKind::Integer);
}

namespace {

/// An action whose only property holds an object (or collection row) nested @p levels deep.
[[nodiscard]] std::string nestedSchema(int levels, bool collections) {
    std::string inner = R"({"type":"string"})";
    for (int level = 0; level < levels; ++level) {
        std::string const object = R"({"type":"object","properties":{"c":)" + inner + "}}";
        inner = collections ? R"({"type":"array","items":)" + object + "}" : object;
    }
    return R"({"properties":{"c":)" + inner + "}}";
}

/// How many nested levels were described before the model stopped and kept the rest as text.
[[nodiscard]] int describedLevels(FormModel const& described) {
    int levels = 0;
    FieldSpec const* current = &described.fields().front();
    while ((current->kind == FieldKind::Object || current->kind == FieldKind::ObjectArray) && !current->truncated) {
        ++levels;
        current = &current->children.front();
    }
    return levels;
}

}  // namespace

TEST_CASE("FormModel: nesting stops at the depth limit, in objects and in collections", "[forms-engine][model]") {
    int const objects = describedLevels(model("Deep", nestedSchema(8, false)));
    int const collections = describedLevels(model("DeepRows", nestedSchema(8, true)));
    CHECK(std::cmp_equal(objects, morph::forms::kMaxNestingDepth));
    CHECK(collections == objects);
    CHECK(describedLevels(model("Shallow", nestedSchema(2, false))) == 2);
    CHECK(describedLevels(model("ShallowRows", nestedSchema(2, true))) == 2);
}

TEST_CASE("FormModel: an element past a span throws, and the last one does not", "[forms-engine][model]") {
    std::vector<int> const items{1, 2};
    CHECK(morph::forms::detail::engine::elementAt(std::span<int const>{items}, 1) == 2);
    CHECK_THROWS_AS(morph::forms::detail::engine::elementAt(std::span<int const>{items}, 2), std::out_of_range);
}

TEST_CASE("FormModel: x-order defaults to the front, and a unit keeps its display and unicode",
          "[forms-engine][model]") {
    FormModel const ordered = model("Ordered", R"({"properties":{
      "b":{"type":"integer","x-order":10},"a":{"type":"integer"}}})");
    CHECK(names(ordered.fields()) == "a,b");
    FormModel const unit = model("Unit", R"({"properties":{
      "m":{"type":["object","null"],"x-decimalPlaces":2,"ExtUnits":{"unitAscii":"kg","unitUnicode":""},
        "x-unitAlternatives":[{"id":"g","display":"gram","decimals":1,"num":1,"den":1000},
                              {"id":"mg","decimals":0,"num":1,"den":1000000}]}}})");
    FieldSpec const& mass = unit.fields().front();
    CHECK(mass.unit == "kg");
    REQUIRE(mass.units.size() == 3);
    CHECK(mass.units[1].display == "gram");
    CHECK(mass.units[2].display == "mg");
    FormModel const unicode = model("Unicode", R"({"properties":{
      "m":{"type":["object","null"],"x-decimalPlaces":2,"ExtUnits":{"unitAscii":"ug","unitUnicode":"\u00b5g"}}}})");
    CHECK(unicode.fields().front().unit == "\xC2\xB5g");
}

TEST_CASE("FormModel: a slider needs the widget and both bounds; bytes need a string; exact bounds need digits",
          "[forms-engine][model]") {
    FormModel const probe = model("Probe", R"({"properties":{
      "plain":{"type":"integer","x-min":0,"x-max":10},
      "half":{"type":"integer","x-widget":"slider","x-min":0},
      "other":{"type":"integer","x-widget":"slider","x-max":10},
      "blob":{"type":"integer","contentEncoding":"base64"},
      "exact":{"type":"integer","x-exactMinimum":"abc","x-exactMaximum":"1x"}}})");
    CHECK(probe.find("plain")->kind == FieldKind::Integer);
    CHECK(probe.find("half")->kind == FieldKind::Integer);
    CHECK(probe.find("other")->kind == FieldKind::Integer);
    CHECK_FALSE(probe.find("blob")->bytes);
    CHECK_FALSE(probe.find("exact")->exactMinimum.has_value());
    CHECK_FALSE(probe.find("exact")->exactMaximum.has_value());
}

TEST_CASE("FormModel: a malformed second rule is reported at its own index", "[forms-engine][model]") {
    auto const parsed = FormModel::fromSchema("A", R"({"properties":{"a":{"type":"string"}},
          "x-rules":[{"kind":"engaged","fields":["a"]},{"kind":"not"}]})");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().path == "x-rules[1]");
}

TEST_CASE("FormModel: a section past the declared groups falls into the implicit one", "[forms-engine][model]") {
    FormModel const layout = model("Layout", R"({"properties":{
      "a":{"type":"integer","x-order":0,"x-section":0},"b":{"type":"integer","x-order":1,"x-section":1}},
      "x-layout":{"groups":[{"title":"One","kind":"section","fields":["a"]}]}})");
    REQUIRE(layout.groups().size() == 2);
    CHECK(layout.groups()[1].implicit);
    CHECK(layout.groups()[1].fields == std::vector<std::string>{"b"});
    FormModel const full = model("Full", R"({"properties":{"a":{"type":"integer","x-order":0,"x-section":0}},
      "x-layout":{"groups":[{"title":"One","kind":"section","fields":["a"]}]}})");
    CHECK(full.groups().size() == 1);
}

TEST_CASE("FormModel: a branch that holds no constants is not an enum", "[forms-engine][model]") {
    FormModel const probe = model("Probe", R"({"properties":{
      "mixed":{"type":"string","oneOf":[{"const":"A"},{"oneOf":[]}]},
      "odd":{"type":"string","anyOf":5}}})");
    CHECK(probe.find("mixed")->kind == FieldKind::Text);
    CHECK(probe.find("odd")->kind == FieldKind::Text);
}

TEST_CASE("SchemaReader: counts read as unsigned or signed only inside the range each can hold",
          "[forms-engine][model]") {
    using morph::forms::detail::engine::SchemaReader;
    auto const read = [](std::string_view literal) {
        static std::deque<morph::forms::detail::engine::Json> keep;
        auto parsed = morph::forms::detail::engine::parseJson(literal);
        REQUIRE(parsed.has_value());
        keep.push_back(*std::move(parsed));
        return &keep.back();
    };
    CHECK(SchemaReader::unsignedOf(read("-0")) == 0U);
    CHECK(SchemaReader::unsignedOf(read("0.0")) == 0U);
    CHECK_FALSE(SchemaReader::unsignedOf(read("1.8e19")).has_value());
    CHECK_FALSE(SchemaReader::unsignedOf(read("-1")).has_value());
    CHECK(SchemaReader::signedOf(read("9.1e18")).has_value());
    CHECK_FALSE(SchemaReader::signedOf(read("9.2e18")).has_value());
}
