// SPDX-License-Identifier: Apache-2.0
//
// A text input's and a form field's decimals follow a bound value, over the recording backend.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ui_interpreter_support.hpp"

namespace {

using morph::ui::testing::Harness;
using morph::ui::testing::readJson;

constexpr std::string_view kInput = R"({
  "vocab": "ui/1", "id": "price",
  "state": { "places": { "init": 2 } },
  "view": { "kind": "column", "children": [
    { "kind": "textInput", "id": "amount", "testId": "amount", "decimals": { "ref": "places" } },
    { "kind": "button", "testId": "whole", "label": "whole", "onClick": [ { "set": "places", "value": 0 } ] },
    { "kind": "button", "testId": "none", "label": "none", "onClick": [ { "set": "places", "value": null } ] },
    { "kind": "text", "testId": "verdict", "text": { "if": [ { "ref": "amount.valid" }, "valid", "invalid" ] } } ] }
})";

constexpr std::string_view kForm = R"({
  "vocab": "ui/1", "id": "capture",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "state": { "places": { "init": 2 } },
  "view": { "kind": "column", "children": [
    { "kind": "form", "id": "cap", "model": "sample", "action": "Lims_CaptureConcentration",
      "fields": { "rowIndex": { "omit": true }, "concentration": { "decimals": { "ref": "places" } } } },
    { "kind": "button", "testId": "whole", "label": "whole", "onClick": [ { "set": "places", "value": 0 } ] },
    { "kind": "button", "testId": "three", "label": "three", "onClick": [ { "set": "places", "value": 3 } ] },
    { "kind": "text", "testId": "verdict", "text": { "if": [ { "ref": "cap.concentration.valid" }, "valid", "invalid" ] } } ] }
})";

constexpr std::string_view kScope = R"({
  "vocab": "ui/1", "id": "capture",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "state": { "places": { "init": 0 } },
  "view": { "kind": "column", "children": [
    { "kind": "form", "id": "cap", "model": "sample", "action": "Lims_CaptureConcentration",
      "fields": { "rowIndex": { "omit": true }, "concentration": { "decimals": { "ref": "places" } } } },
    { "kind": "button", "testId": "none", "label": "none", "onClick": [ { "set": "places", "value": null } ] },
    { "kind": "button", "testId": "one", "label": "one", "onClick": [ { "set": "places", "value": 1 } ] },
    { "kind": "text", "testId": "verdict", "text": { "if": [ { "ref": "cap.concentration.valid" }, "valid", "invalid" ] } } ] }
})";

constexpr std::string_view kWeigh = R"({
  "vocab": "ui/1", "id": "weigh",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "state": { "places": { "init": 2 } },
  "view": { "kind": "column", "children": [
    { "kind": "form", "id": "cap", "model": "sample", "action": "Lims_WeighSample",
      "fields": { "mass": { "decimals": { "ref": "places" } } } },
    { "kind": "text", "testId": "verdict", "text": { "if": [ { "ref": "cap.mass.valid" }, "valid", "invalid" ] } } ] }
})";

// A form prefilled from a record, whose field shows the places the record's currency gives (state `places`).
constexpr std::string_view kPrefill = R"({
  "vocab": "ui/1", "id": "prefilled",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "state": { "places": { "init": 0 } },
  "queries": { "rec": { "model": "sample", "action": "Lims_ListResults", "body": { "obj": { "id": 1 } } } },
  "view": { "kind": "column", "children": [
    { "kind": "form", "id": "cap", "model": "sample", "action": "Lims_CaptureConcentration", "submit": "explicit",
      "prefill": { "query": "rec" },
      "fields": { "rowIndex": { "omit": true }, "concentration": { "decimals": { "ref": "places" } } } },
    { "kind": "button", "testId": "three", "label": "three", "onClick": [ { "set": "places", "value": 3 } ] },
    { "kind": "text", "testId": "verdict", "text": { "if": [ { "ref": "cap.concentration.valid" }, "valid", "invalid" ] } } ] }
})";

// A plain Number field whose schema shows two places and whose overlay is bound to `places`.
constexpr std::string_view kNumber = R"({
  "vocab": "ui/1", "id": "measured",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "state": { "places": { "init": 3 } },
  "view": { "kind": "column", "children": [
    { "kind": "form", "id": "cap", "model": "sample", "action": "Lims_MeasureTemperature", "submit": "explicit",
      "fields": { "reading": { "decimals": { "ref": "places" } } } },
    { "kind": "button", "testId": "whole", "label": "whole", "onClick": [ { "set": "places", "value": 0 } ] },
    { "kind": "text", "testId": "verdict", "text": { "if": [ { "ref": "cap.reading.valid" }, "valid", "invalid" ] } } ] }
})";

// A plain Number field prefilled from a record, whose reading is a Decimal.
constexpr std::string_view kNumberPrefill = R"({
  "vocab": "ui/1", "id": "measured",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "queries": { "rec": { "model": "sample", "action": "Lims_ListResults", "body": { "obj": { "id": 1 } } } },
  "view": { "kind": "column", "children": [
    { "kind": "form", "id": "cap", "model": "sample", "action": "Lims_MeasureTemperature", "submit": "explicit",
      "prefill": { "query": "rec" } },
    { "kind": "text", "testId": "verdict", "text": { "if": [ { "ref": "cap.reading.valid" }, "valid", "invalid" ] } } ] }
})";

}  // namespace

TEST_CASE("bound decimals: a text input follows the places its binding gives", "[ui][bound-decimals]") {
    Harness h;
    auto const screen = h.mount(kInput);
    INFO((h.diagnostics.empty() ? std::string{} : h.diagnostics.front().message()));
    CHECK(h.diagnostics.empty());
    int const input = h.byTestId("TextInput", "amount");
    CHECK(h.backend.prop(input, "decimals") == "2");

    h.backend.edit(input, "12.5");
    h.settle();
    CHECK(h.text("verdict") == "valid");

    h.backend.click(h.byTestId("Button", "whole"));
    h.settle();
    CHECK(h.backend.prop(input, "decimals") == "0");
    CHECK(h.backend.prop(input, "text") == "12.5");  // the text typed stays
    CHECK(h.text("verdict") == "invalid");           // and no longer fits
    CHECK(readJson(*screen, R"({"ref":"amount.errors"})") == R"(["Too many decimal places"])");

    h.backend.edit(input, "12");
    h.settle();
    CHECK(h.text("verdict") == "valid");

    h.backend.click(h.byTestId("Button", "none"));
    h.settle();
    CHECK(h.backend.prop(input, "decimals") == "-1");
}

TEST_CASE("bound decimals: a form field checks its draft again when its overlay's places change",
          "[ui][bound-decimals]") {
    Harness h;
    auto const screen = h.mount(kForm);
    INFO((h.diagnostics.empty() ? std::string{} : h.diagnostics.front().message()));
    CHECK(h.diagnostics.empty());
    int const input = h.byTestId("TextInput", "field_concentration");
    CHECK(h.backend.prop(input, "decimals") == "2");

    h.backend.edit(input, "12.5");
    h.settle();
    CHECK(h.text("verdict") == "valid");

    h.backend.click(h.byTestId("Button", "whole"));
    h.settle();
    CHECK(h.backend.prop(input, "decimals") == "0");
    CHECK(h.backend.prop(input, "text") == "12.5");
    CHECK(h.text("verdict") == "invalid");

    h.backend.click(h.byTestId("Button", "three"));
    h.settle();
    CHECK(h.backend.prop(input, "decimals") == "3");
    CHECK(h.text("verdict") == "valid");

    h.backend.edit(input, "12.5555");
    h.settle();
    CHECK(h.text("verdict") == "invalid");
}

// Mutation: an expression that gives no places leaves the places read at mount (the overlay keeps its baked value).
TEST_CASE("bound decimals: no places from the expression restores the schema's, whatever it gave before",
          "[ui][bound-decimals]") {
    Harness h;
    auto const screen = h.mount(kScope);
    CHECK(h.diagnostics.empty());
    int const input = h.byTestId("TextInput", "field_concentration");
    CHECK(h.backend.prop(input, "decimals") == "0");  // what the expression gave at mount

    h.backend.edit(input, "12.5");
    h.settle();
    CHECK(h.text("verdict") == "invalid");

    h.backend.click(h.byTestId("Button", "none"));
    h.settle();
    CHECK(h.backend.prop(input, "decimals") == "2");  // the schema's `x-decimalPlaces`
    CHECK(h.text("verdict") == "valid");

    h.backend.click(h.byTestId("Button", "one"));
    h.settle();
    CHECK(h.backend.prop(input, "decimals") == "1");
}

// Mutation: apply the places to every unit of the Quantity (drop the canonical-unit condition in `encodeExact`).
TEST_CASE("bound decimals: a form field's places are the canonical unit's, and a unit switch keeps the others'",
          "[ui][bound-decimals]") {
    Harness h;
    auto const screen = h.mount(kWeigh);
    INFO((h.diagnostics.empty() ? std::string{} : h.diagnostics.front().message()));
    CHECK(h.diagnostics.empty());
    int const input = h.byTestId("TextInput", "field_mass");
    int const unit = h.byTestId("Select", "unit_mass");
    using morph::ui::Key;

    h.backend.edit(input, "1.555");
    h.settle();
    CHECK(h.text("verdict") == "invalid");  // kg takes the overlay's 2 places, not its own 3 ...
    h.backend.edit(input, "1.55");
    h.settle();
    CHECK(h.text("verdict") == "valid");

    h.backend.choose(unit, Key{std::int64_t{1}});  // ... and grams keep their own one
    h.settle();
    CHECK(h.backend.prop(input, "text") == "1550.0");
    h.backend.edit(input, "1.55");
    h.settle();
    CHECK(h.text("verdict") == "invalid");
    h.backend.edit(input, "1500");
    h.settle();
    CHECK(h.text("verdict") == "valid");

    // Mutation: convert the draft at the unit's own places, not the overlay's (`switchUnit`).
    h.backend.choose(unit, Key{std::int64_t{0}});
    h.settle();
    CHECK(h.backend.prop(input, "text") == "1.50");  // two places, the overlay's, not the kg's own 1.500
    CHECK(h.text("verdict") == "valid");
}

// Mutation: the error is the literal English text, not resolved through the translation key.
TEST_CASE("bound decimals: a text input's too-many-places error is a form field's, through its translation key",
          "[ui][bound-decimals]") {
    Harness h;
    morph::ui::FormatContext format;
    std::vector<std::string> asked;
    format.translations = [&asked](std::string_view key, std::string_view) -> std::optional<std::string> {
        asked.emplace_back(key);
        return key == "morph.forms.error.too-precise" ? std::optional<std::string>{"Zu viele Nachkommastellen"}
                                                      : std::nullopt;
    };
    h.session.setFormatting(format);
    auto const screen = h.mount(kInput);
    int const input = h.byTestId("TextInput", "amount");
    h.backend.edit(input, "1.234");
    h.settle();
    CHECK(readJson(*screen, R"({"ref":"amount.errors"})") == R"(["Zu viele Nachkommastellen"])");
    CHECK(std::ranges::find(asked, "morph.forms.error.too-precise") != asked.end());
}

// Mutation: decode the prefill at the schema's places (drop the `_places` argument of `decodeScalar` in `load`).
TEST_CASE("bound decimals: a prefill is shown at the places its binding gives, and later changes keep the text",
          "[ui][bound-decimals]") {
    Harness h;
    auto const screen = h.mount(kPrefill);
    CHECK(h.diagnostics.empty());
    int const input = h.byTestId("TextInput", "field_concentration");

    // No places in the record's currency: 1000 loads as "1000", not the schema's "1000.00", and is valid untouched.
    h.reply(h.dispatcher.last("Lims_ListResults"), R"({"sampleId":1,"concentration":{"num":1000,"den":1,"dp":2}})");
    h.settle();
    CHECK(h.backend.prop(input, "text") == "1000");
    CHECK(h.text("verdict") == "valid");

    // Places change after the load: the text stays as typed and is checked again.
    h.backend.click(h.byTestId("Button", "three"));
    h.settle();
    CHECK(h.backend.prop(input, "text") == "1000");
    CHECK(h.text("verdict") == "valid");
}

TEST_CASE("bound decimals: a prefill with more places than the schema's is not rounded", "[ui][bound-decimals]") {
    Harness h;
    std::string document{kPrefill};
    document.replace(document.find(R"("init": 0)"), 9, R"("init": 3)");
    auto const screen = h.mount(document);
    int const input = h.byTestId("TextInput", "field_concentration");
    h.reply(h.dispatcher.last("Lims_ListResults"), R"({"sampleId":1,"concentration":{"num":1234,"den":1000,"dp":3}})");
    h.settle();
    CHECK(h.backend.prop(input, "text") == "1.234");
    CHECK(h.text("verdict") == "valid");
}

// Mutation: drop the `places` argument of `encodeNumber` in `encodeScalar`.
TEST_CASE("bound decimals: a plain Number accepts the places its binding gives, not the schema's",
          "[ui][bound-decimals]") {
    Harness h;
    auto const screen = h.mount(kNumber);
    INFO((h.diagnostics.empty() ? std::string{} : h.diagnostics.front().message()));
    CHECK(h.diagnostics.empty());
    int const input = h.byTestId("TextInput", "field_reading");

    // Three bound places against the schema's two: three typed places are accepted, four refused.
    h.backend.edit(input, "1.234");
    h.settle();
    CHECK(h.text("verdict") == "valid");
    h.backend.edit(input, "1.2345");
    h.settle();
    CHECK(h.text("verdict") == "invalid");
    h.backend.edit(input, "1.235");
    h.settle();
    CHECK(h.text("verdict") == "valid");

    // Fewer bound places than the schema's: what is typed beyond them is refused.
    h.backend.click(h.byTestId("Button", "whole"));
    h.settle();
    CHECK(h.text("verdict") == "invalid");
    h.backend.edit(input, "12");
    h.settle();
    CHECK(h.text("verdict") == "valid");
}

// Mutation: make the Number case of `decodeScalar` ignore an object value.
TEST_CASE("bound decimals: a Decimal prefill loads into a plain Number field", "[ui][bound-decimals]") {
    Harness h;
    auto const screen = h.mount(kNumberPrefill);
    CHECK(h.diagnostics.empty());
    int const input = h.byTestId("TextInput", "field_reading");

    h.reply(h.dispatcher.last("Lims_ListResults"), R"({"sampleId":1,"reading":{"num":1234,"den":1000,"dp":3}})");
    h.settle();
    CHECK(h.backend.prop(input, "text") == "1.234");
}

TEST_CASE("bound decimals: a whole Decimal prefill loads into a plain Number field at its shown places",
          "[ui][bound-decimals]") {
    Harness h;
    auto const screen = h.mount(kNumberPrefill);
    int const input = h.byTestId("TextInput", "field_reading");
    h.reply(h.dispatcher.last("Lims_ListResults"), R"({"sampleId":1,"reading":{"num":1000,"den":1,"dp":0}})");
    h.settle();
    CHECK(h.backend.prop(input, "text") == "1000.00");
}
