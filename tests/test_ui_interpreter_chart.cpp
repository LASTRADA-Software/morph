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
        std::string const value = index == bound ? R"({ "ref": "fifth" })"
                                                 : R"({ "lit": )" + hundredths(10000 + (4 * kRuns.at(index))) + "}";
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

namespace {

// How many reports carry @p text.
std::ptrdiff_t reported(Harness const& h, std::string_view text) {
    return std::ranges::count_if(h.diagnostics,
                                 [text](ui::Refusal const& refusal) { return refusal.reason.contains(text); });
}

// A chart of one series positioned by `x` over `values`, each `{x, v}`.
std::string positionedChart(std::string_view values) {
    return std::format(R"({{
      "vocab": "ui/1", "id": "positioned",
      "view": {{ "kind": "chart", "testId": "chart", "summary": "s",
                 "series": [ {{ "points": {}, "x": {{ "row": "x" }}, "y": {{ "row": "v" }} }} ] }} }})",
                       values);
}

}  // namespace

// Mutation: place a date at its index rather than at its instant (`xRange` is then `[-0.05,1.05]`).
TEST_CASE("chart: dates on x place the points at their instants", "[ui][interpreter][chart]") {
    Harness h;
    auto const screen =
        h.mount(positionedChart(R"({ "lit": [ { "x": "2026-01-01", "v": 1 }, { "x": "2026-01-02", "v": 2 } ] })"));
    // 2026-01-01T00:00Z in milliseconds, padded by 5 % of one day.
    CHECK(chartProp(h, "xRange") == "[1767221280000,1767316320000]");
    CHECK(chartProp(h, "series") == "[;true;[@1767225600000,1;;|@1.767312e+12,2;;]]");
    CHECK(h.diagnostics.empty());
}

// Mutation: position by `x` even when numbers and dates mix (the points then sit at 3 and at an instant).
TEST_CASE("chart: x values that mix numbers and dates space the points by index, with a report",
          "[ui][interpreter][chart]") {
    Harness h;
    auto const screen =
        h.mount(positionedChart(R"({ "lit": [ { "x": 3, "v": 1 }, { "x": "2026-01-02", "v": 2 } ] })"));
    CHECK(chartProp(h, "xRange") == "[-0.05,1.05]");
    CHECK(chartProp(h, "series") == "[;true;[@0,1;;|@1,2;;]]");
    CHECK(reported(h, "all numbers or all dates and instants") == 1);
}

// Mutation: keep a point whose `x` is unreadable at its index (the series then holds three points).
TEST_CASE("chart: a point whose x is null is left out quietly, and one whose x is no position with a report",
          "[ui][interpreter][chart]") {
    Harness h;
    auto const screen = h.mount(positionedChart(
        R"({ "lit": [ { "x": 1, "v": 1 }, { "x": null, "v": 2 }, { "x": "soon", "v": 3 }, { "x": 4, "v": 4 } ] })"));
    CHECK(chartProp(h, "series") == "[;true;[@1,1;;|@4,4;;]]");
    CHECK(reported(h, "a number, a date or an instant") == 1);
    CHECK(h.diagnostics.size() == 1);
}

// Mutation: read any string as a tone (`purple` then marks its point).
TEST_CASE("chart: a computed tone that names none marks nothing, with a report", "[ui][interpreter][chart]") {
    Harness h;
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "tones",
      "state": { "loud": { "init": "loud" } },
      "view": { "kind": "chart", "testId": "chart", "summary": "s",
        "series": [ { "points": { "lit": [ { "v": 1, "t": "ok" }, { "v": 2, "t": "purple" }, { "v": 3, "t": 7 },
                                           { "v": 4, "t": null } ] },
                      "y": { "row": "v" }, "tone": { "row": "t" } } ],
        "lines": [ { "value": 2, "label": "two", "tone": { "ref": "loud" } } ] } })");
    CHECK(chartProp(h, "series") == "[;true;[@0,1;ok;|@1,2;;|@2,3;;|@3,4;;]]");
    CHECK(chartProp(h, "lines") == "[two@2;neutral;solid]");
    // One report per place, however many of its values name no tone.
    CHECK(reported(h, "a chart tone is neutral") == 2);
}

// Mutation: keep a value whose unit does not convert, unconverted (the series then holds `@1,2`).
TEST_CASE("chart: a value in another unit converts or is left out", "[ui][interpreter][chart]") {
    Harness h;
    ui::FormatContext format;
    format.units.insert_or_assign("mg", ui::UnitInfo{.canonical = "g",
                                                     .toCanonical = morph::math::Rational{
                                                         morph::math::Numerator{1}, morph::math::Denominator{1000},
                                                         morph::math::DecimalPlaces{0}}});
    h.session.setFormatting(format);
    auto const screen = h.mount(simpleChart(
        R"({ "lit": [ { "v": { "num": 1, "den": 1, "dp": 0, "unit": "g" } },
                      { "v": { "num": 2, "den": 1, "dp": 0, "unit": "furlong" } },
                      { "v": 3 },
                      { "v": { "num": 4000, "den": 1, "dp": 0, "unit": "mg" } } ] })",
        "[]"));
    CHECK(chartProp(h, "series") == "[;true;[@0,1;;|@3,4;;]]");
    CHECK(reported(h, "does not convert to the unit of the chart's first value") == 1);
}

// Mutation: keep a line whose value is null at zero (the lines then hold `@0`).
TEST_CASE("chart: a reference line whose value is null is not drawn", "[ui][interpreter][chart]") {
    Harness h;
    auto const screen =
        h.mount(simpleChart(R"({ "lit": [ { "v": 1 }, { "v": 3 } ] })",
                            R"([ { "value": null, "label": "none" }, { "value": 2, "style": "solid" } ])"));
    // A line without a label is labelled with its value, at the chart's places.
    CHECK(chartProp(h, "lines") == "[2@2;neutral;solid]");
    CHECK(h.diagnostics.empty());
}

// Mutation: accept a computed yMin at or above yMax (the range is then reversed).
TEST_CASE("chart: computed ends that do not leave yMin below yMax are reported, and the values' range is kept",
          "[ui][interpreter][chart]") {
    Harness h;
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "ends",
      "state": { "low": { "init": 50 }, "high": { "init": 40 } },
      "view": { "kind": "chart", "testId": "chart", "summary": "s",
        "yMin": { "ref": "low" }, "yMax": { "ref": "high" },
        "series": [ { "points": { "lit": [ { "v": 0 }, { "v": 100 } ] }, "y": { "row": "v" } } ] } })");
    CHECK(chartProp(h, "yRange") == "[-5,105]");
    REQUIRE(reported(h, "`yMin` is not below `yMax`") == 1);
    auto const refusal = std::ranges::find_if(
        h.diagnostics, [](ui::Refusal const& one) { return one.reason.contains("`yMin` is not below"); });
    CHECK(refusal->path == "/view/yMin");
}

// Mutation: mark a value below a fixed yMin as above it (the point then records `↑`).
TEST_CASE("chart: a value below a fixed yMin is clipped at the lower edge, and a null end keeps its own",
          "[ui][interpreter][chart]") {
    Harness h;
    auto const screen = h.mount(
        simpleChart(R"({ "lit": [ { "v": 1 }, { "v": 5 }, { "v": 9 } ] })", "[]", R"(, "yMin": 4, "yMax": null)"));
    CHECK(chartProp(h, "yRange") == "[4,9.4]");
    CHECK(chartProp(h, "series") == "[;true;[@0,4↓;;|@1,5;;|@2,9;;]]");
}

// Mutation: ignore `joined` (the first series then records `true`).
TEST_CASE("chart: a series that is not joined says so, and one joined in so many words is joined",
          "[ui][interpreter][chart]") {
    Harness h;
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "scatter",
      "view": { "kind": "chart", "testId": "chart", "summary": "s",
        "series": [ { "label": "a", "points": { "lit": [ { "v": 1 }, { "v": 2 } ] }, "y": { "row": "v" }, "joined": false },
                    { "label": "b", "points": { "lit": [ { "v": 3 } ] }, "y": { "row": "v" }, "joined": true } ] } })");
    CHECK(chartProp(h, "series") == "[a;false;[@0,1;;|@1,2;;],b;true;[@0,3;;]]");
}

// Mutation: draw every point however many (the first point is then index 0).
TEST_CASE("chart: a series draws only its last 1000 points, with a report", "[ui][interpreter][chart]") {
    std::string list = "[";
    for (int index = 0; index < 1005; ++index) {
        list += std::format(R"({}{{ "v": {} }})", index == 0 ? "" : ",", index);
    }
    list += ']';
    Harness h;
    auto const screen = h.mount(simpleChart(R"({ "lit": )" + list + " }", "[]"));
    std::string const series = chartProp(h, "series");
    CHECK(series.starts_with("[;true;[@5,5;;|@6,6;;|"));
    CHECK(series.ends_with("|@1004,1004;;]]"));
    CHECK(reported(h, "a series draws its last 1000 points") == 1);
}

// Mutation: draw the chart from a range that overflowed (a range is then recorded).
TEST_CASE("chart: values whose range overflows draw nothing, with a report", "[ui][interpreter][chart]") {
    Harness h;
    auto const screen =
        h.mount(simpleChart(R"({ "lit": [ { "v": 9223372036854775807 }, { "v": -9223372036854775807 } ] })", "[]"));
    CHECK(chartProp(h, "series") == "[]");
    CHECK(chartProp(h, "yTicks") == "[]");
    CHECK(reported(h, "overflow its arithmetic") == 1);
}

// Mutation: write a value at places it cannot be rounded to (the label then reads
// `100000000000000000.333333333333333333`).
TEST_CASE("chart: a value that cannot be written at the chart's places is written exactly",
          "[ui][interpreter][chart]") {
    Harness h;
    // 10^17 + 1/3 at 18 places needs a numerator of about 10^35.
    auto const screen = h.mount(simpleChart(
        R"({ "lit": [ { "v": 0 } ] })",
        R"([ { "value": { "lit": { "num": 300000000000000001, "den": 3, "dp": 1 } } } ])", R"(, "yPlaces": 18)"));
    CHECK(chartProp(h, "lines") == "[100000000000000000.3@1e+17;neutral;solid]");
}

// Mutation: convert a quantity into a unitless first value as if it had the quantity's unit (`@1,2` is then kept).
TEST_CASE("chart: a quantity after a plain first value is left out", "[ui][interpreter][chart]") {
    Harness h;
    auto const screen = h.mount(simpleChart(
        R"({ "lit": [ { "v": 1 }, { "v": { "num": 2, "den": 1, "dp": 0, "unit": "g" } }, { "v": 3 } ] })", "[]"));
    CHECK(chartProp(h, "series") == "[;true;[@0,1;;|@2,3;;]]");
    CHECK(reported(h, "does not convert to the unit of the chart's first value") == 1);
}
