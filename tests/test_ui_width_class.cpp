// SPDX-License-Identifier: Apache-2.0
//
// The width-class helpers: a screen reads the client's width class, `expanded` where none is set, and switches its
// layout between the compact one and the expanded one.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <morph/ui/author.hpp>
#include <string>

#include "ui_interpreter_support.hpp"

namespace ui = morph::ui::author;

// Mutations: drop the `expanded` fallback from widthClass (the document's let reads only the environment); drop
// byWidth's `compact` case (the switch has no cases and neither the `compact` key nor the narrow layout is written).
TEST_CASE("ui width class: a screen's width class falls back to expanded and switches its layout", "[ui][author]") {
    auto const screen = ui::screen("probe", [](ui::Scope& scope) {
        auto const width = ui::widthClass(scope);
        return ui::byWidth(width, {ui::text({.text = "narrow"}).testId("narrowCard")},
                           {ui::text({.text = "wide"}).testId("wideCard")});
    });
    ui::ScreenRegistry registry;
    auto const added = registry.add(screen);
    REQUIRE(added);
    std::string const& document = (*added)->text;
    CHECK_THAT(document, Catch::Matchers::ContainsSubstring(R"("widthClass")"));
    CHECK_THAT(document, Catch::Matchers::ContainsSubstring(R"("expanded")"));
    CHECK_THAT(document, Catch::Matchers::ContainsSubstring(R"("switch")"));
    CHECK_THAT(document, Catch::Matchers::ContainsSubstring(R"("compact")"));
    CHECK_THAT(document, Catch::Matchers::ContainsSubstring("narrowCard"));
    CHECK_THAT(document, Catch::Matchers::ContainsSubstring("wideCard"));
}

// Mutations: swap the two layouts in byWidth (compact shows the wide card); drop the `expanded` fallback from
// widthClass (an unset width class shows neither card).
TEST_CASE("ui width class: the compact layout shows at compact and the expanded one elsewhere", "[ui][author]") {
    auto const screen = ui::screen("probe", [](ui::Scope& scope) {
        auto const width = ui::widthClass(scope);
        return ui::byWidth(width, {ui::text({.text = "narrow"}).testId("narrowCard")},
                           {ui::text({.text = "wide"}).testId("wideCard")});
    });
    ui::ScreenRegistry registry;
    auto const added = registry.add(screen);
    REQUIRE(added);
    morph::ui::testing::Harness h;
    auto const mounted = h.mount((*added)->text);
    h.settle();
    CHECK(h.text("wideCard") == "wide");
    CHECK_FALSE(h.backend.find("Text", "testId", "narrowCard"));
    h.session.setEnvironmentValue("widthClass", morph::ui::Value{"compact"});
    h.settle();
    CHECK(h.text("narrowCard") == "narrow");
    CHECK_FALSE(h.backend.find("Text", "testId", "wideCard"));
    h.session.setEnvironmentValue("widthClass", morph::ui::Value{"medium"});
    h.settle();
    CHECK(h.text("wideCard") == "wide");
    CHECK_FALSE(h.backend.find("Text", "testId", "narrowCard"));
}
