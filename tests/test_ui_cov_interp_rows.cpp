// SPDX-License-Identifier: Apache-2.0

// A rows table's `rowSpans`: the spans a row names reach the table, and a value that is no list of spans is reported.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <morph/ui/mount.hpp>
#include <string>
#include <vector>

#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;

namespace {

using ui::testing::Harness;

// Three rows over columns a, b and c, each with the `spans` the case gives it.
std::string spanDocument(std::string const& firstSpans, std::string const& secondSpans) {
    return R"({"vocab":"ui/1","id":"spans",
      "state":{"items":{"init":{"lit":[{"id":1,"a":"a1","b":"b1","c":"c1","spans":)" +
           firstSpans + R"(},{"id":2,"a":"a2","b":"b2","c":"c2","spans":)" + secondSpans +
           R"(},{"id":3,"a":"a3","b":"b3","c":"c3","spans":null}]}}},
      "view":{"kind":"table","testId":"t","rows":{"ref":"items"},"key":{"row":"id"},"rowSpans":{"row":"spans"},
        "columns":[{"id":"a","label":"A"},{"id":"b","label":"B"},{"id":"c","label":"C"}]}})";
}

std::string spansOfRow(Harness const& h, std::string const& key) {
    for (int const row : h.backend.all("Row")) {
        if (h.backend.prop(row, "rowKey") == key) {
            std::string const value = h.backend.prop(row, "rowSpans");
            return value.empty() ? "-" : value;
        }
    }
    return "no row";
}

std::size_t reportsOf(Harness const& h, std::string const& text) {
    return static_cast<std::size_t>(std::ranges::count_if(
        h.diagnostics, [&text](ui::Refusal const& refusal) { return refusal.reason.contains(text); }));
}

}  // namespace

TEST_CASE("cov rows: a row's spans name a column, and rows and columns from one", "[ui][interpreter][table][cov]") {
    Harness h;
    auto const screen = h.mount(spanDocument(R"([{"column":"a","rows":2},{"column":"b","columns":2}])", "null"));
    CHECK(spansOfRow(h, "1") == "0:2,1:1x2");  // rows default to one
    CHECK(spansOfRow(h, "2") == "-");
    CHECK(reportsOf(h, "rowSpans is") == 0);
}

TEST_CASE("cov rows: spans that are no list of spans are reported once for the table", "[ui][interpreter][table][cov]") {
    Harness h;
    auto const screen = h.mount(spanDocument(R"("wide")", R"("wider")"));
    CHECK(spansOfRow(h, "1") == "-");
    CHECK(reportsOf(h, "rowSpans is \"wide\"") == 1);
    CHECK(reportsOf(h, "rowSpans is") == 1);  // the second row's value is not reported again
}

TEST_CASE("cov rows: a span with no column, an unknown one or a count that is no whole number is dropped",
          "[ui][interpreter][table][cov]") {
    Harness h;
    auto const screen = h.mount(spanDocument(
        R"([{"rows":2},{"column":"nope","rows":2},{"column":"a","rows":"two"},{"column":"b","columns":0},)"
        R"({"column":"b","columns":"x"},{"column":"c","rows":-1},{"column":"b","rows":2}])",
        "null"));
    CHECK(spansOfRow(h, "1") == "1:2");  // only the last span is well formed
    CHECK(reportsOf(h, "rowSpans is") == 1);
}
