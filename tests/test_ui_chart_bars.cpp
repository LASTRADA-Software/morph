// SPDX-License-Identifier: Apache-2.0

// A chart series drawn as bars: the document key, the author builder, and what reaches the backend (the bar slot, the
// axes that hold the baseline and the half slots, and each series' style), as the recording backend shows them.

#include <catch2/catch_test_macros.hpp>
#include <format>
#include <glaze/glaze.hpp>
#include <morph/ui/author.hpp>
#include <morph/ui/document.hpp>
#include <string>
#include <string_view>

#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;
namespace author = morph::ui::author;
using ui::testing::Harness;

namespace {

// A chart whose series are @p series, each `{points, y, style…}` written out, with @p extra keys on the chart.
std::string chartOf(std::string_view series, std::string_view extra = "") {
    return std::format(R"({{ "vocab": "ui/1", "id": "bars",
      "view": {{ "kind": "chart", "testId": "chart", "summary": "s" {}, "series": [ {} ] }} }})",
                       extra, series);
}

// One series of the labelled kind over @p values (a list of `{x, v}` objects), drawn per @p style (`bars` or `line`).
std::string seriesOf(std::string_view label, std::string_view values, std::string_view style,
                     std::string_view extra = "") {
    return std::format(R"({{ "label": "{}", "points": {{ "lit": {} }}, "y": {{ "row": "v" }}, "x": {{ "row": "x" }},
        "style": "{}" {} }})",
                       label, values, style, extra);
}

std::string prop(Harness const& h, std::string_view name) {
    return h.backend.prop(h.byTestId("Chart", "chart"), name);
}

// Why @p text is refused, as `path: reason`; `loads` when it is not.
std::string refusal(std::string const& text) {
    auto const loaded = ui::loadDocumentText(text);
    return loaded ? std::string{"loads"} : loaded.error().path + ": " + loaded.error().reason;
}

}  // namespace

// Mutation: leave the style out of the lowering (the series records no `;bars` and the slot stays 0).
TEST_CASE(
    "chart bars: a bars series reaches the backend with its slot, and its axes hold the baseline and the half slots",
    "[ui][interpreter][chart]") {
    Harness h;
    auto const screen = h.mount(chartOf(seriesOf("Revenue", R"([ {"x": 0, "v": 1}, {"x": 1, "v": 3} ])", "bars")));
    CHECK(prop(h, "series") == "[Revenue;false;[@0,1;;|@1,3;;];bars]");
    CHECK(prop(h, "barSlot") == "1");
    // The positions 0 and 1 widened by half a slot, then by 5 % of that span.
    CHECK(prop(h, "xRange") == "[-0.6,1.6]");
    // Every value is above 0: the baseline is the axis's lower end, and only the upper end is padded.
    CHECK(prop(h, "yRange") == "[0,3.15]");
}

// Mutation: pad both ends whatever the signs (a positive-only chart then starts below 0).
TEST_CASE("chart bars: the baseline is the axis end when nothing lies beyond it, and padded when something does",
          "[ui][interpreter][chart]") {
    auto const range = [](std::string_view values) {
        Harness h;
        auto const screen = h.mount(chartOf(seriesOf("a", values, "bars")));
        return prop(h, "yRange");
    };
    CHECK(range(R"([ {"x": 0, "v": 2}, {"x": 1, "v": 4} ])") == "[0,4.2]");
    CHECK(range(R"([ {"x": 0, "v": -2}, {"x": 1, "v": -4} ])") == "[-4.2,0]");
    CHECK(range(R"([ {"x": 0, "v": -2}, {"x": 1, "v": 4} ])") == "[-2.3,4.3]");
}

// Mutation: leave a gap's position out of the slot (the slot is then 5).
TEST_CASE("chart bars: a gap keeps its position, so the slot never spans the labels of empty positions",
          "[ui][interpreter][chart]") {
    Harness h;
    auto const screen = h.mount(chartOf(
        seriesOf("a", R"([ {"x": 0, "v": 1}, {"x": 1, "v": null}, {"x": 2, "v": null}, {"x": 5, "v": 2} ])", "bars")));
    CHECK(prop(h, "barSlot") == "1");
}

// Mutation: let a line series set the slot (it is then 1, from the line's positions).
TEST_CASE("chart bars: only bars series set the slot, and a line series beside them records as before",
          "[ui][interpreter][chart]") {
    Harness h;
    auto const screen =
        h.mount(chartOf(seriesOf("Bars", R"([ {"x": 0, "v": 1}, {"x": 2, "v": 3} ])", "bars") + "," +
                        seriesOf("Line", R"([ {"x": 0, "v": 2}, {"x": 1, "v": 2}, {"x": 2, "v": 2} ])", "line")));
    CHECK(prop(h, "barSlot") == "2");
    CHECK(prop(h, "series") == "[Bars;false;[@0,1;;|@2,3;;];bars,Line;true;[@0,2;;|@1,2;;|@2,2;;]]");
}

// Mutation: default the style to bars (the unstyled series records `;bars`).
TEST_CASE("chart bars: a series with no style, or style line, is a line and leaves the slot 0",
          "[ui][interpreter][chart]") {
    Harness h;
    auto const screen = h.mount(chartOf(R"({ "points": { "lit": [ {"v": 1}, {"v": 2} ] }, "y": { "row": "v" } })"));
    CHECK(prop(h, "series") == "[;true;[@0,1;;|@1,2;;]]");
    CHECK(prop(h, "barSlot") == "0");
    Harness again;
    auto const named =
        again.mount(chartOf(R"({ "points": { "lit": [ {"v": 1} ] }, "y": { "row": "v" }, "style": "line" })"));
    CHECK(prop(again, "series") == "[;true;[@0,1;;]]");
}

// Mutation: accept any style name, or refuse `joined: false` beside bars.
TEST_CASE("chart bars: the loader refuses an unknown style and a joined bars series, and takes joined false",
          "[ui][document][chart]") {
    auto const one = [](std::string_view extra) {
        return refusal(chartOf(std::format(R"({{ "points": {{ "lit": [] }}, "y": {{ "row": "v" }} {} }})", extra)));
    };
    CHECK(one(R"(, "style": "bars")") == "loads");
    CHECK(one(R"(, "style": "bars", "joined": false)") == "loads");
    CHECK(one(R"(, "style": "dots")") == "/view/series/0/style: a series `style` is `line` or `bars`");
    CHECK(one(R"(, "style": "bars", "joined": true)") == "/view/series/0/joined: a `bars` series is not joined");
    CHECK(one(R"(, "joined": true)") == "loads");
}

// Mutation: leave the style out of the author's series (the document draws a line).
TEST_CASE("chart bars: the author builder writes bars, and its document loads", "[ui][author][chart]") {
    author::Node const node =
        author::chart({.summary = "s",
                       .series = {{.label = author::ref("name"),
                                   .points = author::ref("runs"),
                                   .y = [](author::AnyHandle const& point) { return point.at("v"); },
                                   .joined = false,
                                   .style = author::SeriesStyle::Bars}}});
    std::string const written = glz::write_json(node.json()).value_or(std::string{});
    CHECK(written.contains(R"("style":"bars")"));
    CHECK(written.contains(R"("joined":false)"));
    CHECK(refusal(R"({"vocab": "ui/1", "id": "a", "state": {"name": {"init": {"lit": "n"}},
        "runs": {"init": {"lit": []}}}, "view": )" +
                  written + "}") == "loads");
}
