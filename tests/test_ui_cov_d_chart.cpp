// SPDX-License-Identifier: Apache-2.0

// A chart's bars: the room one position gives them is the least distance between two positions, and the axis holds
// half a slot beyond the outermost bar.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <format>
#include <string>
#include <string_view>

#include "ui_interpreter_support.hpp"

namespace {

using morph::ui::testing::Harness;

std::string chartOver(std::string_view series) {
    return std::format(R"({{"vocab":"ui/1","id":"bars",
      "view":{{"kind":"chart","testId":"chart","summary":"s","series":[{}]}}}})",
                       series);
}

std::string barSeries(std::string_view points, std::string_view label = "") {
    return std::format(R"({{{}"style":"bars","points":{{"lit":{}}},"x":{{"row":"x"}},"y":{{"row":"v"}}}})",
                       label.empty() ? std::string{} : std::format(R"("label":"{}",)", label), points);
}

std::string prop(Harness& h, std::string_view name) { return h.backend.prop(h.byTestId("Chart", "chart"), name); }

}  // namespace

TEST_CASE(
    "covd chart: a bar's room is the least distance between two bar positions, and the axis holds half of it beyond",
    "[ui][interpreter][chart][covd]") {
    struct Case {
        std::string_view positions;
        std::string_view slot;
        std::string_view range;
    };
    auto const [positions, slot, range] =
        GENERATE(Case{"[1,2,3]", "1", "[0.35,3.65]"}, Case{"[1,2,4]", "1", "[0.3,4.7]"},
                 Case{"[1,3,4]", "1", "[0.3,4.7]"},  // a later pair is nearer than the first
                 Case{"[1,1,2]", "1", "[0.4,2.6]"},  // two bars at one position leave no distance
                 Case{"[10,20,40]", "10", "[3,47]"}, Case{"[5]", "1", "[4.45,5.55]"});
    CAPTURE(positions);
    std::string points = "[";
    std::string_view rest = positions.substr(1, positions.size() - 2);
    while (!rest.empty()) {
        std::size_t const comma = rest.find(',');
        std::string_view const x = rest.substr(0, comma);
        points += (points.size() > 1 ? "," : "") + std::format(R"({{"x":{},"v":1}})", x);
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
    }
    points += "]";
    Harness h;
    auto const screen = h.mount(chartOver(barSeries(points)));
    CHECK(prop(h, "barSlot") == slot);
    CHECK(prop(h, "xRange") == range);
}

TEST_CASE("covd chart: a line's positions widen the axis a bar series is drawn on", "[ui][interpreter][chart][covd]") {
    Harness h;
    std::string const line =
        R"({"label":"Trend","points":{"lit":[{"x":0,"v":2},{"x":10,"v":3}]},"x":{"row":"x"},"y":{"row":"v"}})";
    auto const screen = h.mount(chartOver(barSeries(R"([{"x":4,"v":1},{"x":5,"v":2}])", "Bars") + "," + line));
    CHECK(prop(h, "barSlot") == "1");
    CHECK(prop(h, "xRange") == "[-1.05,11.05]");
}

TEST_CASE("covd chart: bars that hold no value leave the axis to the line", "[ui][interpreter][chart][covd]") {
    Harness h;
    auto const screen = h.mount(chartOver(barSeries(R"([{"x":1,"v":null},{"x":2,"v":null}])")));
    CHECK(prop(h, "barSlot") == "1");
}
