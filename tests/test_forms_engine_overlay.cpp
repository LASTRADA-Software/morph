// SPDX-License-Identifier: Apache-2.0
//
// The field overlay (spec 6 §10): what a document's `fields` changes about a form before the session is built.

#include <catch2/catch_test_macros.hpp>
#include <morph/forms/blank_as.hpp>
#include <morph/forms/engine/field_model.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "forms_engine_support.hpp"

namespace {

using morph::forms::FieldKind;
using morph::forms::FieldOverlay;
using morph::forms::FieldOverlays;
using morph::forms::FormModel;
using morph::test::formsengine::Harness;

constexpr std::string_view kSchema = R"({"properties":{
  "rowIndex":{"type":"integer","x-order":0},
  "analystId":{"type":"integer","x-order":1},
  "mass":{"type":["object","null"],"title":"Mass","x-decimalPlaces":3,
          "ExtUnits":{"unitAscii":"kg","unitUnicode":"kg"},
          "x-unitAlternatives":[{"id":"g","display":"g","decimals":1,"num":1,"den":1000}],"x-order":2},
  "receivedOn":{"type":["integer","null"],"x-order":3},
  "remarks":{"type":["string","null"],"x-order":4},
  "ratio":{"type":["number","null"],"x-order":5}},
  "required":["rowIndex","analystId","mass"]})";

[[nodiscard]] FieldOverlays sampleOverlays() {
    FieldOverlays overlays;
    overlays.emplace("rowIndex", FieldOverlay{.omit = true});
    overlays.emplace("analystId", FieldOverlay{.hidden = true});
    overlays.emplace("mass", FieldOverlay{.label = "Masse", .unit = "g", .decimals = 2});
    overlays.emplace("receivedOn", FieldOverlay{.widget = "epochDays"});
    overlays.emplace("remarks", FieldOverlay{.blankAs = morph::forms::BlankAs::Empty});
    overlays.emplace("ratio", FieldOverlay{.unit = "%", .decimals = 1});
    return overlays;
}

}  // namespace

TEST_CASE("FieldOverlay: omit, hidden, label, unit, decimals, widget and blankAs change the model",
          "[forms-engine][overlay]") {
    auto const model = FormModel::fromSchema("Lims_Capture", kSchema, sampleOverlays());
    INFO((model ? std::string{} : model.error().path + ": " + model.error().message));
    REQUIRE(model.has_value());
    CHECK(model->find("rowIndex") == nullptr);  // omitted from the form
    REQUIRE(model->find("analystId") != nullptr);
    CHECK(model->find("analystId")->hidden);
    auto const* const mass = model->find("mass");
    REQUIRE(mass != nullptr);
    CHECK(mass->title == "Masse");
    CHECK(mass->entryUnit == 1);
    CHECK(mass->units.at(1).decimals == 2U);
    CHECK(mass->decimalPlaces == 3U);  // the wire keeps the schema's places
    CHECK(model->find("receivedOn")->kind == FieldKind::EpochDate);
    CHECK(model->find("remarks")->blankAs == morph::forms::BlankAs::Empty);
    CHECK(model->find("ratio")->unit == "%");
    CHECK(model->find("ratio")->displayDecimals == 1U);
}

TEST_CASE("FieldOverlay: a field or unit the schema does not have is refused", "[forms-engine][overlay]") {
    FieldOverlays unknownField;
    unknownField.emplace("nope", FieldOverlay{.hidden = true});
    CHECK(FormModel::fromSchema("A", kSchema, unknownField).error().path == "fields.nope");
    FieldOverlays unknownUnit;
    unknownUnit.emplace("mass", FieldOverlay{.unit = "lb"});
    CHECK(FormModel::fromSchema("A", kSchema, unknownUnit).error().path == "fields.mass.unit");
}

TEST_CASE("FieldOverlay: an omitted member leaves the body, a hidden one stays, the entry unit is the overlay's",
          "[forms-engine][overlay]") {
    Harness harness;
    auto model = FormModel::fromSchema("Lims_Capture", kSchema, sampleOverlays());
    REQUIRE(model.has_value());
    morph::forms::FormSession form{harness.rt, *std::move(model), harness.server.submitter(),
                                   harness.server.fetcher()};
    CHECK(form.field("mass").unit().peek() == 1);  // entered in grams
    harness.type(form, "analystId", "7");
    harness.type(form, "mass", "2650500.25");  // two places in grams, as the overlay says
    REQUIRE(form.body().has_value());
    CHECK(*form.body() == R"({"analystId":7,"mass":{"num":265050025,"den":100000,"dp":3}})");
    // A prefill arrives in the canonical unit and is shown in the entry unit.
    form.prefill(R"({"analystId":7,"mass":{"num":2650500,"den":1000,"dp":3}})");
    harness.settle();
    CHECK(form.field("mass").text().peek() == "2650500.00");
    CHECK(form.field("mass").unit().peek() == 1);
}

TEST_CASE("FormModel::withSubmitMode chooses over x-submitMode", "[forms-engine][overlay]") {
    auto const model = FormModel::fromSchema("A", R"({"properties":{"a":{"type":"string"}},"required":[]})");
    REQUIRE(model.has_value());
    CHECK(model->submitMode() == morph::forms::SubmitMode::Automatic);
    CHECK(model->withSubmitMode(morph::forms::SubmitMode::Explicit).submitMode() ==
          morph::forms::SubmitMode::Explicit);
}

TEST_CASE("FieldOverlay: an overlay on a schema without the field is refused; odd shapes are passed over",
          "[forms-engine][overlay]") {
    FieldOverlays named;
    named.emplace("a", FieldOverlay{.omit = true});
    auto const noProperties = FormModel::fromSchema("X", R"({"title":"none"})", named);
    REQUIRE_FALSE(noProperties.has_value());
    CHECK(noProperties.error().path == "fields.a");

    // `omit` with no `required` list, or one holding a non-string; an overlay on a property that is not an object.
    FieldOverlays overlays;
    overlays.emplace("a", FieldOverlay{.omit = true});
    overlays.emplace("b", FieldOverlay{.omit = true});
    overlays.emplace("c", FieldOverlay{.label = "C"});
    overlays.emplace("d", FieldOverlay{.blankAs = morph::forms::BlankAs::Omit});
    auto const lenient = FormModel::fromSchema(
        "X", R"({"properties":{"a":{"type":"string"},"b":{"type":"string"},"c":5,"d":{"type":"string"}}})", overlays);
    CHECK_FALSE(lenient.has_value());  // "c" is not a property schema: the reader refuses it, overlay or not
    auto const withRequired = FormModel::fromSchema(
        "X",
        R"({"properties":{"a":{"type":"string"},"b":{"type":"string"},"d":{"type":"string"}},"required":[3,"b"]})",
        [] {
            FieldOverlays some;
            some.emplace("b", FieldOverlay{.omit = true});
            some.emplace("d", FieldOverlay{.blankAs = morph::forms::BlankAs::Omit});
            return some;
        }());
    REQUIRE(withRequired.has_value());
    CHECK(withRequired->find("b") == nullptr);
    REQUIRE(withRequired->find("d") != nullptr);
    CHECK(withRequired->find("d")->blankAs == morph::forms::BlankAs::Omit);
}
