// SPDX-License-Identifier: Apache-2.0

// Lowering of a group, as a document spells it with the properties left out or given another way: each snippet is a
// child of one column, mounted, and what the backend records is read back.

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;

namespace {

std::string documentOf(std::string_view child) {
    return R"({"vocab":"ui/1","id":"low","view":{"kind":"column","children":[)" + std::string{child} + "]}}";
}

}  // namespace

TEST_CASE("cov h lowering: a group without a description or gap is a legend and its shown errors",
          "[covh][ui][interpreter]") {
    ui::testing::Harness h;
    auto const screen = h.mount(documentOf(R"({"kind":"group","testId":"g","label":"Legend","children":[]})"));
    int const group = h.byTestId("Column", "g");
    std::vector<int> const lines = h.backend.children(group);
    CHECK(lines.size() == 2);  // the legend and the errors line, no description
    CHECK(h.backend.prop(group, "a11yRole") == "group");
    CHECK(h.backend.prop(group, "a11yName") == "Legend");
}

TEST_CASE("cov h lowering: a group shows the errors a document hands it, one line each, and keeps the role it names",
          "[covh][ui][interpreter]") {
    ui::testing::Harness h;
    auto const screen = h.mount(
        documentOf(R"({"kind":"group","testId":"g","label":"Legend","description":"About","gap":2,)"
                   R"("errors":{"lit":["first","second"]},"a11y":{"role":"region","name":"Named"},"children":[]})"));
    int const group = h.byTestId("Column", "g");
    std::vector<int> const lines = h.backend.children(group);
    REQUIRE(lines.size() == 3);
    CHECK(h.backend.prop(lines.at(1), "text") == "About");
    CHECK(h.backend.prop(lines.back(), "text") == "first\\nsecond");
    CHECK(h.backend.prop(lines.back(), "visible") != "false");
    CHECK(h.backend.prop(group, "a11yRole") == "region");
    CHECK(h.backend.prop(group, "a11yName") == "Named");
}
