// SPDX-License-Identifier: Apache-2.0

// The theme's value readers: icon names, DTCG dimensions, shadows and font families, read from the JSON a theme
// holds, and the shapes each refuses.

#include <algorithm>
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

namespace {

ui::ThemeValues valuesOf(std::string_view text) {
    auto tokens = ui::parseTokens(text);
    REQUIRE(tokens.has_value());
    auto resolved = ui::resolveTokens({&*tokens});
    REQUIRE(resolved.has_value());
    return ui::themeValues(*resolved);
}

}  // namespace

TEST_CASE("ui theme values: the neutral tone has its parts, and the hover fill and the track are their own tokens",
          "[ui][theme]") {
    ui::ThemeValues const defaults = ui::themeValues();
    CHECK(defaults.colors.at("color.neutral").hex() == "#6e6e73");
    CHECK(defaults.colors.at("color.neutral.bg").hex() == "#6e6e7326");
    CHECK(defaults.colors.at("color.neutral.border").hex() == "#6e6e73");
    CHECK(defaults.colors.at("color.neutral.dot").hex() == "#6e6e73");
    CHECK(defaults.colors.at("color.hover").hex() == "#1d1d1f14");
    CHECK(defaults.colors.at("color.track").hex() == "#e5e5ea");
    CHECK(defaults.colors.at("color.controlSurface").hex() == "#ffffff");
    CHECK(defaults.colors.at("color.controlBorder").hex() == "#c7c7cc");
    CHECK(defaults.colors.at("color.divider").hex() == "#dcdce0");
    ui::ThemeShadow const& popup = defaults.shadows.at("elevation.popup");
    CHECK(popup.color.hex() == "#00000024");
    CHECK(popup.offsetX == 0.0);
    CHECK(popup.offsetY == 8.0);
    CHECK(popup.blur == 24.0);
    CHECK(popup.spread == 0.0);
}

// Mutation: leave `radius.badge` out of `kThemeTokens` (a bundle's value is dropped and the default 4 reads).
TEST_CASE("ui theme values: the badge radius is a token with a default", "[ui][theme]") {
    CHECK(ui::themeValues().dimensions.at("radius.badge") == 4.0);
    ui::ThemeValues const values =
        valuesOf(R"({"radius": {"$type": "dimension", "badge": {"$value": {"value": 2, "unit": "px"}}}})");
    CHECK(values.dimensions.at("radius.badge") == 2.0);
}

// Mutation: accept any string as a stroke style (`dotted` reads), or leave `border.style` out of `kThemeTokens` (the
// bundle's `dashed` is dropped).
TEST_CASE("ui theme values: a surface's border style is solid or dashed, and any other is refused", "[ui][theme]") {
    CHECK(ui::themeValues().strokes.at("border.style") == "solid");
    ui::ThemeValues const dashed = valuesOf(R"({"border": {"style": {"$type": "strokeStyle", "$value": "dashed"}}})");
    CHECK(dashed.strokes.at("border.style") == "dashed");

    auto const parse = [](std::string_view text) {
        auto tokens = ui::parseTokens(text);
        REQUIRE(tokens.has_value());
        return std::move(*tokens);
    };
    ui::TokenSet const base = parse(R"({"border": {"style": {"$type": "strokeStyle", "$value": "dashed"}}})");
    ui::TokenSet const dotted = parse(R"({"border": {"style": {"$type": "strokeStyle", "$value": "dotted"}}})");
    ui::ThemeIndex index;
    index.base = "base";
    index.surfaces.emplace("empty", "dotted");
    ui::TokenFileLookup const files = [&](std::string_view id) -> ui::TokenSet const* {
        if (id == "base") {
            return &base;
        }
        return id == "dotted" ? &dotted : nullptr;
    };
    auto const refused = ui::checkTheme(index, files);
    REQUIRE(refused.has_value());
    CHECK(refused->find("border.style: only solid and dashed are drawn") != std::string::npos);
    index.surfaces.clear();
    CHECK_FALSE(ui::checkTheme(index, files).has_value());
}

// Mutation: accept any string as a keyword (`round` reads), or leave the paths out of `kThemeTokens` (the bundle's
// values are dropped).
TEST_CASE("ui theme values: a keyword is one of the names its spec lists, and any other is refused", "[ui][theme]") {
    CHECK(ui::themeValues().keywords.at("shape.iconOnly") == "default");
    CHECK(ui::themeValues().keywords.at("font.overline.case") == "none");
    CHECK(valuesOf(R"({"shape": {"iconOnly": {"$type": "keyword", "$value": "square"}}})")
              .keywords.at("shape.iconOnly") == "square");
    CHECK(valuesOf(R"({"font": {"overline": {"case": {"$type": "keyword", "$value": "uppercase"}}}})")
              .keywords.at("font.overline.case") == "uppercase");

    auto const parse = [](std::string_view text) {
        auto tokens = ui::parseTokens(text);
        REQUIRE(tokens.has_value());
        return std::move(*tokens);
    };
    ui::TokenSet const base = parse(R"({"shape": {"iconOnly": {"$type": "keyword", "$value": "square"}}})");
    ui::TokenSet const round = parse(R"({"shape": {"iconOnly": {"$type": "keyword", "$value": "round"}}})");
    ui::ThemeIndex index;
    index.base = "base";
    index.surfaces.emplace("odd", "round");
    ui::TokenFileLookup const files = [&](std::string_view id) -> ui::TokenSet const* {
        if (id == "base") {
            return &base;
        }
        return id == "round" ? &round : nullptr;
    };
    auto const refused = ui::checkTheme(index, files);
    REQUIRE(refused.has_value());
    CHECK(refused->find("shape.iconOnly: only default and square are drawn") != std::string::npos);
    index.surfaces.clear();
    CHECK_FALSE(ui::checkTheme(index, files).has_value());
    CHECK(ui::unreadThemeTokens(index, files).empty());
}

// Mutation: drop the fill's alpha before comparing (the translucent default reads as opaque near-black).
TEST_CASE("ui theme values: text that cannot be read on a fill is reported", "[ui][theme]") {
    CHECK(ui::themeContrastProblems(ui::themeValues()).empty());

    // The gallery's neutral text colour used as the hover fill: dark text on a dark fill.
    auto const dark = ui::themeContrastProblems(valuesOf(R"({"color": {"$type": "color",
        "text": {"$value": "#1C1E21"}, "hover": {"$value": "#363C46"}}})"));
    REQUIRE(dark.size() >= 1);
    CHECK(dark.front().starts_with("color.hover: color.text is 1.5"));
    CHECK(dark.front().ends_with(":1 on it, below 4.5:1"));

    // A control fill the text cannot be read on.
    auto const pale = ui::themeContrastProblems(valuesOf(R"({"color": {"$type": "color",
        "text": {"$value": "#c9ccd6"}, "controlSurface": {"$value": "#ffffff"}}})"));
    CHECK(std::ranges::any_of(
        pale, [](std::string const& line) { return line.starts_with("color.controlSurface: color.text is 1.6"); }));

    // Muted holds 3:1, not 4.5:1: the default muted on the default track is 4.04:1 and passes.
    CHECK(ui::themeContrastProblems(valuesOf(R"({"color": {"$type": "color", "track": {"$value": "#e5e5ea"}}})"))
              .empty());
    auto const faint = ui::themeContrastProblems(valuesOf(R"({"color": {"$type": "color",
        "muted": {"$value": "#9a9aa0"}, "track": {"$value": "#e5e5ea"}}})"));
    CHECK(std::ranges::any_of(faint, [](std::string const& line) {
        return line.starts_with("color.track: color.muted is ") && line.ends_with(":1 on it, below 3:1");
    }));
}

TEST_CASE("ui theme values: the type steps and the strong weight have defaults, and a weight name reads as its number",
          "[ui][theme]") {
    ui::ThemeValues const defaults = ui::themeValues();
    for (std::string_view const path :
         {"font.size.small", "font.size.caption", "font.size.mono", "font.size.display"}) {
        CHECK(defaults.dimensions.at(std::string{path}) == 0.0);
    }
    CHECK(defaults.numbers.at("font.weight.strong") == 700.0);

    CHECK(valuesOf(R"({"font": {"weight": {"strong": {"$type": "fontWeight", "$value": "semi-bold"}}}})")
              .numbers.at("font.weight.strong") == 600.0);
    CHECK(valuesOf(R"({"font": {"weight": {"strong": {"$type": "fontWeight", "$value": 650}}}})")
              .numbers.at("font.weight.strong") == 650.0);
    // A weight of another type keeps the default.
    CHECK(valuesOf(R"({"font": {"weight": {"strong": {"$type": "number", "$value": 300}}}})")
              .numbers.at("font.weight.strong") == 700.0);
    CHECK(valuesOf(R"({"font": {"size": {"small": {"$type": "dimension", "$value": {"value": 11, "unit": "px"}}}}})")
              .dimensions.at("font.size.small") == 11.0);
}
