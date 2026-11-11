// SPDX-License-Identifier: Apache-2.0
//
// A text input's and a form field's decimals follow a bound value, over the recording backend.

#include <catch2/catch_test_macros.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <string>
#include <string_view>

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
