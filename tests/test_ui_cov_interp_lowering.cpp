// SPDX-License-Identifier: Apache-2.0

// Lowering of single elements: each snippet is a child of one column, mounted, and what the interpreter reports or the
// backend records is read back.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <morph/ui/mount.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;

namespace {

std::string documentOf(std::string_view child) {
    return R"({"vocab":"ui/1","id":"low",
      "state":{"bad":{"init":"wat"},"num":{"init":3},"flag":{"init":true},"none":{"init":null},
               "items":{"init":{"lit":[{"id":1}]}}},
      "view":{"kind":"column","children":[)" +
           std::string{child} + "]}}";
}

std::size_t reports(ui::testing::Harness const& h, std::string_view text) {
    return static_cast<std::size_t>(std::ranges::count_if(
        h.diagnostics, [text](ui::Refusal const& refusal) { return refusal.reason.contains(text); }));
}

}  // namespace

TEST_CASE("cov lowering: a computed sizing that is no sizing is reported and lowers to content",
          "[ui][interpreter][cov][r2]") {
    struct Case {
        std::string_view child;
        std::string_view reported;  // empty when nothing is reported
    };
    for (Case const& item : std::vector<Case>{
             {.child = R"({"kind":"text","text":"x","testId":"a","sizing":{"ref":"bad"}})",
              .reported = R"(sizing is "wat")"},
             {.child = R"({"kind":"text","text":"x","testId":"a","sizing":{"obj":{"width":{"ref":"bad"}}}})",
              .reported = R"(sizing is {"width":"wat"})"},
             {.child = R"({"kind":"text","text":"x","testId":"a","sizing":{"obj":{"height":{"obj":{"fixed":3}}}}})",
              .reported = ""},
             {.child = R"({"kind":"text","text":"x","testId":"a","sizing":{"obj":{"fixed":3}}})", .reported = ""},
         }) {
        INFO(item.child);
        ui::testing::Harness h;
        auto const screen = h.mount(documentOf(item.child));
        CHECK(h.byTestId("Text", "a") != 0);
        CHECK(reports(h, "sizing is") == (item.reported.empty() ? 0U : 1U));
        if (!item.reported.empty()) {
            CHECK(reports(h, item.reported) == 1);
        }
    }
}

TEST_CASE("cov lowering: a progress shows a number it is given and nothing else", "[ui][interpreter][cov][r2]") {
    struct Case {
        std::string_view value;
        std::string_view shown;
    };
    for (Case const& item : std::vector<Case>{{.value = R"({"ref":"num"})", .shown = "3"},
                                              {.value = R"({"fn":"div","args":[1,4]})", .shown = "0.25"},
                                              {.value = R"({"ref":"none"})", .shown = "none"},
                                              {.value = R"({"ref":"bad"})", .shown = "none"}}) {
        INFO(item.value);
        ui::testing::Harness h;
        auto const screen = h.mount(
            documentOf(R"({"kind":"progress","testId":"a","label":"p","value":)" + std::string{item.value} + "}"));
        CHECK(h.backend.prop(h.byTestId("Progress", "a"), "value") == item.shown);
    }
}

TEST_CASE("cov lowering: a checkbox bound to a value that is no flag is unchecked", "[ui][interpreter][cov][r2]") {
    ui::testing::Harness h;
    auto const screen = h.mount(documentOf(R"({"kind":"checkbox","testId":"a","label":"C","checked":{"ref":"num"}})"));
    CHECK(h.backend.prop(h.byTestId("Checkbox", "a"), "checked") == "false");
    ui::testing::Harness flagged;
    auto const other =
        flagged.mount(documentOf(R"({"kind":"checkbox","testId":"a","label":"C","checked":{"ref":"flag"}})"));
    CHECK(flagged.backend.prop(flagged.byTestId("Checkbox", "a"), "checked") == "true");
}

TEST_CASE("cov lowering: an activation runs a button's click and its activate commands",
          "[ui][interpreter][cov][r2]") {
    ui::testing::Harness h;
    auto const screen = h.mount(R"({"vocab":"ui/1","id":"low","state":{"n":{"init":0}},
      "view":{"kind":"column","children":[
        {"kind":"button","testId":"a","label":"B","onActivate":[{"set":"n","value":4}]},
        {"kind":"text","testId":"n","text":{"ref":"n"}}]}})");
    h.backend.click(h.byTestId("Button", "a"));
    h.settle();
    CHECK(h.text("n") == "4");
}
