// SPDX-License-Identifier: Apache-2.0

// A removable badge's accessible name, and a step list's `labelled`, each as a literal and as a value that changes.

#include <catch2/catch_test_macros.hpp>
#include <string>

#include "ui_interpreter_support.hpp"

namespace {

using morph::ui::testing::Harness;

constexpr char const* kLabels = R"({"vocab":"ui/1","id":"labels",
  "state":{"name":{"init":"QC"},"drop":{"init":"Drop"},"shown":{"init":true}},
  "view":{"kind":"column","children":[
    {"kind":"badge","testId":"fixed","text":"Fixed","onRemove":[{"set":"name","value":{"lit":"x"}}]},
    {"kind":"badge","testId":"named","text":"Named","removeLabel":"Detach","onRemove":[{"set":"name","value":{"lit":"x"}}]},
    {"kind":"badge","testId":"bound","text":{"ref":"name"},"onRemove":[{"set":"name","value":{"lit":"x"}}]},
    {"kind":"badge","testId":"boundLabel","text":"Tag","removeLabel":{"ref":"drop"},"onRemove":[{"set":"name","value":{"lit":"x"}}]},
    {"kind":"badge","testId":"plain","text":"Plain"},
    {"kind":"steps","testId":"literalSteps","items":{"lit":[{"label":"a","state":"done"}]},"labelled":false},
    {"kind":"steps","testId":"boundSteps","items":{"lit":[{"label":"a","state":"done"}]},"labelled":{"ref":"shown"}},
    {"kind":"steps","testId":"defaultSteps","items":{"lit":[{"label":"a","state":"done"}]}},
    {"kind":"button","testId":"flip","label":"Flip","onClick":[{"set":"name","value":{"lit":"Other"}},{"set":"drop","value":{"lit":"Detach now"}},{"set":"shown","value":{"lit":false}}]}]}})";

}  // namespace

TEST_CASE("covj mount labels: a removable badge is named by its label or its text, and follows a bound one",
          "[covj][ui][mount]") {
    Harness h;
    auto const screen = h.mount(kLabels);
    auto const removable = [&h](char const* testId) {
        return h.backend.prop(h.byTestId("Badge", testId), "removable");
    };
    CHECK(removable("fixed") == "Remove Fixed");
    CHECK(removable("named") == "Detach");
    CHECK(removable("bound") == "Remove QC");
    CHECK(removable("boundLabel") == "Drop");
    CHECK_FALSE(h.backend.hasProp(h.byTestId("Badge", "plain"), "removable"));
    h.backend.click(h.byTestId("Button", "flip"));
    h.settle();
    CHECK(removable("fixed") == "Remove Fixed");
    CHECK(removable("bound") == "Remove Other");
    CHECK(removable("boundLabel") == "Detach now");
}

TEST_CASE("covj mount labels: steps are labelled unless a literal or a value says otherwise", "[covj][ui][mount]") {
    Harness h;
    auto const screen = h.mount(kLabels);
    CHECK(h.backend.prop(h.byTestId("Steps", "literalSteps"), "labelled") == "false");
    CHECK(h.backend.prop(h.byTestId("Steps", "boundSteps"), "labelled") == "true");
    CHECK_FALSE(h.backend.hasProp(h.byTestId("Steps", "defaultSteps"), "labelled"));
    h.backend.click(h.byTestId("Button", "flip"));
    h.settle();
    CHECK(h.backend.prop(h.byTestId("Steps", "boundSteps"), "labelled") == "false");
}
