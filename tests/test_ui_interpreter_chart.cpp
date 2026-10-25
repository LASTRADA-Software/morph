// SPDX-License-Identifier: Apache-2.0

// A chart lowered by the interpreter and mounted on RecordingBackend: its range, ticks, lines, points and reports.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;
using ui::testing::Harness;

namespace {

// The assay control's 20 runs, in tenths of an SD from the mean 100.0 % (SD 0.4): 100 + 0.04 per tenth.
constexpr std::array<std::int64_t, 20> kRuns{-3, 3, -6, 1, 7, -2, -8, 2, 4, -4, -1, 8, -3, -9, 3, 4, -3, 9, 14, 34};

// An exact decimal literal at two places.
std::string hundredths(std::int64_t value) { return std::format(R"({{ "num": {}, "den": 100, "dp": 2 }})", value); }

// The runs as `{run, value, violation}` objects; the value of the run at @p bound reads `fifth` instead.
std::string runsList(std::size_t bound = kRuns.size()) {
    std::string list = R"({ "list": [)";
    for (std::size_t index = 0; index < kRuns.size(); ++index) {
        std::string const value =
            index == bound ? R"({ "ref": "fifth" })" : R"({ "lit": )" + hundredths(10000 + (4 * kRuns.at(index))) + "}";
        std::string const violation = index + 1 == kRuns.size() ? R"("+3.4SD, rule 1-3s")" : "null";
        list += std::format(R"({}{{ "obj": {{ "run": {}, "value": {}, "violation": {} }} }})", index == 0 ? "" : ",",
                            index + 1, value, violation);
    }
    return list + "] }";
}

// Design 10's chart over @p runs, with @p extra keys on the chart.
std::string controlChart(std::string const& runs, std::string_view extra = "") {
    auto const line = [](std::string_view label, std::int64_t tenths, std::string_view rest) {
        return std::format(R"({{ "value": {{ "lit": {{ "num": {}, "den": 10, "dp": 1 }} }}, "label": "{}"{} }})",
                           1000 + tenths, label, rest);
    };
    return std::format(
        R"({{
      "vocab": "ui/1", "id": "control",
      "state": {{ "fifth": {{ "init": {{ "lit": {} }} }} }},
      "view": {{ "kind": "column", "children": [
        {{ "kind": "button", "testId": "bump", "label": "bump",
           "onClick": [ {{ "set": "fifth", "value": {{ "lit": {} }} }} ] }},
        {{ "kind": "chart", "testId": "chart", "summary": "Levey-Jennings", "yLabel": "% LC", "yPlaces": 1 {},
          "series": [ {{ "points": {}, "y": {{ "row": "value" }}, "xLabel": {{ "row": "run" }},
                         "tone": {{ "if": [ {{ "fn": "engaged", "args": [ {{ "row": "violation" }} ] }}, "err", null ] }},
                         "note": {{ "row": "violation" }} }} ],
          "lines": [ {}, {}, {}, {}, {}, {}, {} ] }} ] }} }})",
        hundredths(10028), hundredths(10012), extra, runs, line("+3SD", 12, R"(, "tone": "err", "style": "dashed")"),
        line("+2SD", 8, R"(, "tone": "warn", "style": "dashed")"), line("+1SD", 4, R"(, "style": "dotted")"),
        line("mean", 0, ""), line("−1SD", -4, R"(, "style": "dotted")"),
        line("−2SD", -8, R"(, "tone": "warn", "style": "dashed")"),
        line("−3SD", -12, R"(, "tone": "err", "style": "dashed")"));
}

std::string chartProp(Harness const& h, std::string_view name) {
    return h.backend.prop(h.byTestId("Chart", "chart"), name);
}

// A chart of one series over `values`, with `lines` and the chart's own `extra` keys.
std::string simpleChart(std::string_view values, std::string_view lines, std::string_view extra = "") {
    return std::format(R"({{
      "vocab": "ui/1", "id": "simple",
      "view": {{ "kind": "chart", "testId": "chart", "summary": "s" {},
                 "series": [ {{ "points": {}, "y": {{ "row": "v" }} }} ], "lines": {} }} }})",
                       extra, values, lines);
}

}  // namespace

// Mutation: compute the range from the points only (the ±3SD lines fall outside `yRange`).
TEST_CASE("chart: design 10's control chart records its seven lines, no ticks, and the marked 20th run",
          "[ui][interpreter][chart]") {
    Harness h;
    auto screen = h.mount(controlChart(runsList()));
    CHECK(chartProp(h, "yRange") == "[98.672,101.488]");
    CHECK(chartProp(h, "xRange") == "[-0.95,19.95]");
    CHECK(chartProp(h, "yTicks") == "[]");
    CHECK(chartProp(h, "yLabel") == "% LC");
    CHECK(chartProp(h, "lines") ==
          "[+3SD@101.2;err;dashed,+2SD@100.8;warn;dashed,+1SD@100.4;neutral;dotted,mean@100;neutral;solid,"
          "−1SD@99.6;neutral;dotted,−2SD@99.2;warn;dashed,−3SD@98.8;err;dashed]");
    std::string const series = chartProp(h, "series");
    CHECK(series.starts_with("[;true;[1@0,99.88;;|2@1,100.12;;|"));
    CHECK(series.ends_with("|20@19,101.36;err;+3.4SD\\, rule 1-3s]]"));
    CHECK(chartProp(h, "summary") == "Levey-Jennings");
    CHECK(h.diagnostics.empty());
}

// Mutation: drop the equality check on `ChartData` in FakeChart's `update` (every property logs again).
TEST_CASE("chart: an inner run's new value sets the series alone", "[ui][interpreter][chart]") {
    Harness h;
    auto screen = h.mount(controlChart(runsList(4)));
    CHECK(chartProp(h, "series").contains("|5@4,100.28;;|"));
    h.backend.clearLog();
    h.backend.click(h.byTestId("Button", "bump"));
    h.settle();
    std::vector<std::string> chartLines;
    std::ranges::copy_if(h.backend.log(), std::back_inserter(chartLines),
                         [](std::string const& line) { return line.contains("Chart#"); });
    REQUIRE(chartLines.size() == 1);
    CHECK(chartLines.front().contains(" series=["));
    CHECK(chartLines.front().contains("|5@4,100.12;;|"));
}

// Mutation: join across a gap by reading a `null` y as 0 (no `gap` is recorded).
TEST_CASE("chart: a null value is a gap, and a quantity line converts to the series' unit",
          "[ui][interpreter][chart]") {
    Harness h;
    ui::FormatContext format;
    format.units.insert_or_assign("mg", ui::UnitInfo{.canonical = "g",
                                                     .toCanonical = morph::math::Rational{
                                                         morph::math::Numerator{1}, morph::math::Denominator{1000},
                                                         morph::math::DecimalPlaces{0}}});
    h.session.setFormatting(format);
    auto screen = h.mount(simpleChart(
        R"({ "lit": [ { "v": { "num": 400, "den": 1, "dp": 0, "unit": "mg" } }, { "v": null },
                      { "v": { "num": 500, "den": 1, "dp": 0, "unit": "mg" } } ] })",
        R"([ { "value": { "lit": { "num": 1, "den": 2, "dp": 1, "unit": "g" } }, "label": "half" } ])"));
    CHECK(chartProp(h, "series") == "[;true;[@0,400;;|@1,gap;;|@2,500;;]]");
    CHECK(chartProp(h, "lines") == "[half@500;neutral;solid]");
    CHECK(h.diagnostics.empty());
}

// Mutation: let any line replace the ticks (`yTicks` is then empty).
TEST_CASE("chart: without three lines the axis has ticks at a step of 1, 2 or 5, in the locale",
          "[ui][interpreter][chart]") {
    Harness h;
    ui::FormatContext format;
    format.numeric.decimalSeparator = ",";
    h.session.setFormatting(format);
    auto screen = h.mount(simpleChart(R"({ "lit": [ { "v": { "num": 990, "den": 10, "dp": 1 } },
                                                     { "v": { "num": 1010, "den": 10, "dp": 1 } } ] })",
                                      "[]", R"(, "yPlaces": 1)"));
    // 99.0 to 101.0, padded by 0.1: a step of 1 is the largest that puts three ticks inside, shown at `yPlaces`.
    CHECK(chartProp(h, "yRange") == "[98.9,101.1]");
    CHECK(chartProp(h, "yTicks") == "[99\\,0@99,100\\,0@100,101\\,0@101]");
}

// Mutation: let any line replace the ticks (`yTicks` is then empty).
TEST_CASE("chart: a stability trend with two specification lines keeps its ticks and labels the lines",
          "[ui][interpreter][chart]") {
    Harness h;
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "trend",
      "view": { "kind": "chart", "testId": "chart", "summary": "trend", "yLabel": "Assay, % LC",
        "series": [
          { "label": "25 °C", "points": { "lit": [ { "m": 0, "v": 100 }, { "m": 3, "v": 99 }, { "m": 12, "v": 98 } ] },
            "x": { "row": "m" }, "y": { "row": "v" }, "xLabel": { "fn": "format", "args": ["{0} m", { "row": "m" }] } },
          { "label": "40 °C", "points": { "lit": [ { "m": 0, "v": 100 }, { "m": 3, "v": 97 } ] },
            "x": { "row": "m" }, "y": { "row": "v" } } ],
        "lines": [ { "value": 95, "label": "LSL 95.0", "tone": "err", "style": "dashed" },
                   { "value": 105, "label": "USL 105.0", "tone": "err", "style": "dashed" } ] } })");
    std::string const ticks = chartProp(h, "yTicks");
    auto const count = std::ranges::count(ticks, '@');
    CHECK(count >= 3);
    CHECK(count <= 6);
    CHECK(ticks == "[95@95,100@100,105@105]");
    CHECK(chartProp(h, "lines") == "[LSL 95.0@95;err;dashed,USL 105.0@105;err;dashed]");
    // The months are spaced as they are: 0, 3 and 12, the axis padded by 5 % of 12.
    CHECK(chartProp(h, "xRange") == "[-0.6,12.6]");
    CHECK(chartProp(h, "series") ==
          "[25 °C;true;[0 m@0,100;;|3 m@3,99;;|12 m@12,98;;],40 °C;true;[@0,100;;|@3,97;;]]");
}

// Mutation: keep a value that is not a number as a gap (the series then records `@1,gap`).
TEST_CASE("chart: a string value is left out and reported once, however often the chart recomputes",
          "[ui][interpreter][chart]") {
    Harness h;
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "strings",
      "state": { "n": { "init": 1 } },
      "view": { "kind": "column", "children": [
        { "kind": "button", "testId": "bump", "label": "bump",
          "onClick": [ { "set": "n", "value": { "fn": "add", "args": [ { "ref": "n" }, 1 ] } } ] },
        { "kind": "chart", "testId": "chart", "summary": "s",
          "series": [ { "points": { "list": [ { "obj": { "v": { "ref": "n" } } }, { "obj": { "v": "x" } } ] },
                        "y": { "row": "v" } } ] } ] } })");
    h.backend.click(h.byTestId("Button", "bump"));
    h.settle();
    h.backend.click(h.byTestId("Button", "bump"));
    h.settle();
    CHECK(chartProp(h, "series") == "[;true;[@0,3;;]]");
    auto const reports = std::ranges::count_if(
        h.diagnostics, [](ui::Refusal const& refusal) { return refusal.reason.contains("number or a quantity"); });
    CHECK(reports == 1);
}

// Mutation: clamp a value outside a fixed end without marking it (the 20th run records `101.36` or `101`).
TEST_CASE("chart: a value above a fixed yMax is a clipped point at the edge", "[ui][interpreter][chart]") {
    Harness h;
    auto screen = h.mount(controlChart(runsList(), R"(, "yMax": 101)"));
    CHECK(chartProp(h, "yRange") == "[98.672,101]");
    CHECK(chartProp(h, "series").ends_with("|20@19,101↑;err;+3.4SD\\, rule 1-3s]]"));
}

TEST_CASE("chart: points that are no list draw as empty, reported unless null", "[ui][interpreter][chart]") {
    Harness h;
    auto screen = h.mount(simpleChart("null", "[]"));
    CHECK(chartProp(h, "series") == "[;true;[]]");
    CHECK(h.diagnostics.empty());
    Harness other;
    auto second = other.mount(simpleChart(R"("runs")", "[]"));
    CHECK(other.backend.prop(other.byTestId("Chart", "chart"), "series") == "[;true;[]]");
    CHECK(std::ranges::count_if(other.diagnostics, [](ui::Refusal const& refusal) {
              return refusal.reason.contains("`points` is a list");
          }) == 1);
}
