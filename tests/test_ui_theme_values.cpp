// SPDX-License-Identifier: Apache-2.0

// The theme's value readers: icon names, DTCG dimensions, shadows and font families, read from the JSON a theme
// holds, and the shapes each refuses.

#include <catch2/catch_test_macros.hpp>
#include <morph/ui/theme.hpp>
#include <morph/ui/value.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ui = morph::ui;

namespace {

ui::Json json(std::string_view text) {
    auto parsed = ui::parseJson(text);
    REQUIRE(parsed.has_value());
    return *parsed;
}

constexpr std::string_view kLayer = R"({"color": "#00000080", "offsetX": {"value": 0, "unit": "px"},
    "offsetY": {"value": 2, "unit": "px"}, "blur": {"value": 4, "unit": "px"}, "spread": {"value": 0, "unit": "px"}})";

}  // namespace

// Mutation: accept an upper-case letter (`Icon` then passes).
TEST_CASE("ui theme values: an icon name is a lower-case letter, then up to 63 of a-z, 0-9, . and -", "[ui][theme]") {
    CHECK(ui::isIconName("a"));
    CHECK(ui::isIconName("nav.back-2"));
    CHECK(ui::isIconName("z" + std::string(63, '9')));
    CHECK_FALSE(ui::isIconName(""));
    CHECK_FALSE(ui::isIconName("z" + std::string(64, '9')));
    CHECK_FALSE(ui::isIconName("Icon"));
    CHECK_FALSE(ui::isIconName("1con"));
    CHECK_FALSE(ui::isIconName(".nav"));
    CHECK_FALSE(ui::isIconName("naV"));
    CHECK_FALSE(ui::isIconName("nav_back"));
    CHECK_FALSE(ui::isIconName("nav back"));
}

// Mutation: read `rem` as px (2rem then reads 2).
TEST_CASE("ui theme values: a dimension is a number of px or rem", "[ui][theme]") {
    CHECK(ui::detail::pixelsOf(json(R"({"value": 2, "unit": "px"})")) == 2.0);
    CHECK(ui::detail::pixelsOf(json(R"({"value": 2, "unit": "rem"})")) == 2.0 * ui::detail::kPixelsPerRem);
    CHECK(ui::detail::pixelsOf(json(R"({"value": 2, "unit": "em"})")) == std::nullopt);
    CHECK(ui::detail::pixelsOf(json(R"({"value": 2, "unit": 1})")) == std::nullopt);
    CHECK(ui::detail::pixelsOf(json(R"({"value": "2", "unit": "px"})")) == std::nullopt);
    CHECK(ui::detail::pixelsOf(json(R"({"value": 2})")) == std::nullopt);
    CHECK(ui::detail::pixelsOf(json(R"({"unit": "px"})")) == std::nullopt);
    CHECK(ui::detail::pixelsOf(json(R"("2px")")) == std::nullopt);
}

// Mutation: take the last layer of a list (the blur then reads 8).
TEST_CASE("ui theme values: a shadow is one layer, or the first of a list, with a colour and four dimensions",
          "[ui][theme]") {
    auto const one = ui::detail::shadowOf(json(kLayer));
    REQUIRE(one.has_value());
    CHECK(one->offsetY == 2.0);
    CHECK(one->blur == 4.0);
    CHECK(one->color.a == 0x80);
    std::string const second = R"({"color": "#fff", "offsetX": {"value": 1, "unit": "px"},
        "offsetY": {"value": 1, "unit": "px"}, "blur": {"value": 8, "unit": "px"}, "spread": {"value": 0, "unit": "px"}})";
    auto const first = ui::detail::shadowOf(json("[" + std::string{kLayer} + "," + second + "]"));
    REQUIRE(first.has_value());
    CHECK(first->blur == 4.0);
    CHECK_FALSE(ui::detail::shadowOf(json("[]")).has_value());
    CHECK_FALSE(ui::detail::shadowOf(json(R"("#000")")).has_value());
    CHECK_FALSE(ui::detail::shadowOf(json(R"({"color": "#000", "offsetX": {"value": 0, "unit": "px"},
        "offsetY": {"value": 2, "unit": "px"}, "blur": {"value": 4, "unit": "px"}})"))
                    .has_value());
    CHECK_FALSE(ui::detail::shadowOf(json(R"({"color": "black", "offsetX": {"value": 0, "unit": "px"},
        "offsetY": {"value": 2, "unit": "px"}, "blur": {"value": 4, "unit": "px"}, "spread": {"value": 0, "unit": "px"}})"))
                    .has_value());

    CHECK(ui::detail::checkShadow(json(kLayer)) == std::nullopt);
    CHECK(ui::detail::checkShadow(json("[" + std::string{kLayer} + "]")) == std::nullopt);
    CHECK(ui::detail::checkShadow(json("[]")).has_value());
    CHECK(ui::detail::checkShadow(json("[" + std::string{kLayer} + R"(, "#000"])")).has_value());
    CHECK(ui::detail::checkShadow(json(R"({"offsetX": {"value": 0, "unit": "px"}})")).has_value());
}

// Mutation: keep a list's non-string entries as empty names (the list then reads three names).
TEST_CASE("ui theme values: a font family is one name or a list of them", "[ui][theme]") {
    CHECK(ui::detail::familiesOf(json(R"("Inter")")) == std::vector<std::string>{"Inter"});
    CHECK(ui::detail::familiesOf(json(R"(["Inter", 3, "Arial"])")) == std::vector<std::string>{"Inter", "Arial"});
    CHECK(ui::detail::familiesOf(json("3")).empty());
}
