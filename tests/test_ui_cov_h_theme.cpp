// SPDX-License-Identifier: Apache-2.0

// Theme tokens at the edges a design tool's export rarely reaches: names an icon set may not hold, tokens a theme
// reads with the wrong type or a value out of range, shadows with a part missing, and themes whose layers are not all
// there or whose colours do not contrast.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <map>
#include <morph/ui/theme.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace ui = morph::ui;
using ui::Rgba;
using ui::TokenSet;

TokenSet tokensOf(std::string_view text) {
    auto tokens = ui::parseTokens(text);
    REQUIRE(tokens.has_value());
    return std::move(*tokens);
}

std::string tokenError(std::string_view text) {
    auto const tokens = ui::parseTokens(text);
    REQUIRE_FALSE(tokens.has_value());
    return tokens.error();
}

std::string indexError(std::string_view text) {
    auto const index = ui::parseThemeIndex(text);
    REQUIRE_FALSE(index.has_value());
    return index.error();
}

ui::Json jsonOf(std::string_view text) {
    auto json = ui::parseJson(text);
    REQUIRE(json.has_value());
    return std::move(*json);
}

}  // namespace

TEST_CASE("cov h theme: an icon name is a lowercase word of letters, digits, dots and dashes", "[covh][ui][theme]") {
    struct Case {
        std::string name;
        bool expected;
    };
    for (Case const& item : std::vector<Case>{{"", false},
                                              {"a", true},
                                              {"close", true},
                                              {"arrow-left.2", true},
                                              {"z", true},
                                              {std::string(64, 'a'), true},
                                              {std::string(65, 'a'), false},
                                              {"Close", false},
                                              {"1st", false},
                                              {"{x", false},
                                              {"-x", false},
                                              {"aB", false},
                                              {"a{", false},
                                              {"a_b", false},
                                              {"a b", false}}) {
        INFO(item.name);
        CHECK(ui::isIconName(item.name) == item.expected);
    }
}

TEST_CASE("cov h theme: an index refuses an icon set it cannot hold", "[covh][ui][theme]") {
    std::string many = R"({"vocab":"ui-theme/1","base":"b","icons":{)";
    for (std::size_t at = 0; at <= ui::kMaxThemeIcons; ++at) {
        many += (at == 0 ? "\"i" : ",\"i") + std::to_string(at) + R"(":"f")";
    }
    many += "}}";
    // The names of a long set are not icon names (digits first after the letter are, but the set is too long first).
    CHECK(indexError(many).contains("at most 256 names"));
    struct Case {
        std::string_view icons;
        std::string_view expected;
    };
    for (Case const& item : std::vector<Case>{
             {R"("icons":[])", "`icons` is an object of icon name to icon file"},
             {R"("icons":{"ok":""})", "`icons.ok` names an icon file"},
             {R"("icons":{"ok":5})", "`icons.ok` names an icon file"},
             {R"("icons":{"Bad":"f"})", "`Bad` is not an icon name"},
         }) {
        INFO(item.icons);
        CHECK(indexError(std::string{R"({"vocab":"ui-theme/1","base":"b",)"} + std::string{item.icons} + "}")
                  .contains(item.expected));
    }
    auto const fine = ui::parseThemeIndex(R"({"vocab":"ui-theme/1","base":"b","icons":{"ok":"f"}})");
    REQUIRE(fine.has_value());
    CHECK(fine->icons.at("ok") == "f");
}

TEST_CASE("cov h theme: a $root is the token of its group, and no file's top is a group", "[covh][ui][theme]") {
    CHECK(tokenError(R"({"$root":{"$value":1}})").contains("the top of a file is no group"));
    CHECK(tokenError(R"({"a":{"$root":5}})").contains("`a.$root` is a token"));
    CHECK(tokenError(R"({"a":{"$root":{"$description":"no value"}}})").contains("`a.$root` is a token"));
    auto const fine = tokensOf(R"({"a":{"$type":"number","$root":{"$value":3}}})");
    REQUIRE(fine.contains("a"));
    CHECK(fine.at("a").type == "number");
}

TEST_CASE("cov h theme: a token of the wrong type or an unknown name leaves the default", "[covh][ui][theme]") {
    ui::ThemeValues const defaults = ui::themeValues();
    struct Case {
        char const* what;
        std::string_view tokens;
        std::string_view group;  // "strokes", "keywords" or "families"
        std::string_view path;
        std::optional<std::string> expected;  // nothing: the default
    };
    for (Case const& item : std::vector<Case>{
             {"a solid stroke", R"({"border":{"style":{"$type":"strokeStyle","$value":"solid"}}})", "strokes",
              "border.style", "solid"},
             {"a dashed stroke", R"({"border":{"style":{"$type":"strokeStyle","$value":"dashed"}}})", "strokes",
              "border.style", "dashed"},
             {"a stroke that is not drawn", R"({"border":{"style":{"$type":"strokeStyle","$value":"dotted"}}})",
              "strokes", "border.style", std::nullopt},
             {"a stroke of another type", R"({"border":{"style":{"$type":"number","$value":1}}})", "strokes",
              "border.style", std::nullopt},
             {"a keyword of the set", R"({"shape":{"iconOnly":{"$type":"keyword","$value":"square"}}})", "keywords",
              "shape.iconOnly", "square"},
             {"a keyword outside the set", R"({"shape":{"iconOnly":{"$type":"keyword","$value":"round"}}})",
              "keywords", "shape.iconOnly", std::nullopt},
             {"a keyword that is no name", R"({"shape":{"iconOnly":{"$type":"keyword","$value":3}}})", "keywords",
              "shape.iconOnly", std::nullopt},
         }) {
        INFO(item.what);
        ui::ThemeValues const values = ui::themeValues(tokensOf(item.tokens));
        auto const& group = item.group == "strokes" ? values.strokes : values.keywords;
        auto const& base = item.group == "strokes" ? defaults.strokes : defaults.keywords;
        REQUIRE(group.contains(item.path));
        CHECK(group.at(std::string{item.path}) == (item.expected ? *item.expected : base.at(std::string{item.path})));
    }
    ui::ThemeValues const other = ui::themeValues(tokensOf(R"({"font":{"family":{"$type":"number","$value":3}}})"));
    CHECK(other.families.at("font.family") == defaults.families.at("font.family"));
    ui::ThemeValues const named =
        ui::themeValues(tokensOf(R"({"font":{"family":{"$type":"fontFamily","$value":["Inter","serif"]}}})"));
    CHECK(named.families.at("font.family") == std::vector<std::string>{"Inter", "serif"});
}

TEST_CASE("cov h theme: opacity, stroke and keyword tokens are checked where the theme reads them",
          "[covh][ui][theme]") {
    struct Case {
        char const* what;
        std::string_view tokens;
        std::optional<std::string> opacity;
        std::optional<std::string> stroke;
    };
    for (Case const& item : std::vector<Case>{
             {"opacity in range", R"({"opacity":{"stale":{"$type":"number","$value":0.5}}})", std::nullopt,
              std::nullopt},
             {"opacity below zero", R"({"opacity":{"stale":{"$type":"number","$value":-0.1}}})",
              "opacity.stale: must be between 0 and 1", std::nullopt},
             {"opacity above one", R"({"opacity":{"muted":{"$type":"number","$value":1.5}}})",
              "opacity.muted: must be between 0 and 1", std::nullopt},
             {"opacity of another type",
              R"({"opacity":{"stale":{"$type":"dimension","$value":{"value":-3,"unit":"px"}}}})", std::nullopt,
              std::nullopt},
             {"a solid stroke", R"({"border":{"style":{"$type":"strokeStyle","$value":"solid"}}})", std::nullopt,
              std::nullopt},
             {"a dashed stroke", R"({"border":{"style":{"$type":"strokeStyle","$value":"dashed"}}})", std::nullopt,
              std::nullopt},
             {"a dotted stroke", R"({"border":{"style":{"$type":"strokeStyle","$value":"dotted"}}})", std::nullopt,
              "border.style: only solid and dashed are drawn"},
             {"a stroke that is no name", R"({"border":{"style":{"$type":"strokeStyle","$value":4}}})", std::nullopt,
              "border.style: only solid and dashed are drawn"},
             {"a stroke of another type", R"({"border":{"style":{"$type":"number","$value":4}}})", std::nullopt,
              std::nullopt},
         }) {
        INFO(item.what);
        TokenSet const tokens = tokensOf(item.tokens);
        CHECK(ui::detail::opacityProblem(tokens) == item.opacity);
        CHECK(ui::detail::strokeProblem(tokens) == item.stroke);
    }
}

TEST_CASE("cov h theme: a shadow needs a colour and four lengths", "[covh][ui][theme]") {
    constexpr std::string_view kLengths =
        R"("offsetX":{"value":0,"unit":"px"},"offsetY":{"value":1,"unit":"px"},"blur":{"value":2,"unit":"px"},"spread":{"value":0,"unit":"px"})";
    auto const shadow = [](std::string_view color, std::string_view lengths) {
        std::string body{color};
        if (!lengths.empty()) {
            body += (body.empty() ? "" : ",") + std::string{lengths};
        }
        return jsonOf("{" + body + "}");
    };
    auto const without = [&](std::string_view name) {
        std::string lengths;
        for (std::string_view const each : {"offsetX", "offsetY", "blur", "spread"}) {
            if (each != name) {
                lengths += (lengths.empty() ? "" : ",") + std::string{"\""} + std::string{each} +
                           R"(":{"value":1,"unit":"px"})";
            }
        }
        return lengths;
    };
    auto const whole = ui::detail::shadowOf(shadow(R"("color":"#000000")", kLengths));
    REQUIRE(whole.has_value());
    CHECK(whole->offsetY == 1.0);
    CHECK(whole->blur == 2.0);
    CHECK_FALSE(ui::detail::shadowOf(shadow("", kLengths)).has_value());
    CHECK_FALSE(ui::detail::shadowOf(shadow(R"("color":"nonsense")", kLengths)).has_value());
    for (std::string_view const name : {"offsetX", "offsetY", "blur", "spread"}) {
        INFO(name);
        CHECK_FALSE(ui::detail::shadowOf(shadow(R"("color":"#000000")", without(name))).has_value());
        CHECK(ui::detail::checkShadow(shadow(R"("color":"#000000")", without(name))).has_value());
    }
    CHECK_FALSE(ui::detail::checkShadow(shadow(R"("color":"#000000")", kLengths)).has_value());
    CHECK(ui::detail::checkShadow(shadow("", kLengths)).has_value());
}

TEST_CASE("cov h theme: the tokens nothing reads are found across the files that are there", "[covh][ui][theme]") {
    ui::ThemeIndex index;
    index.base = "base";
    index.variants.emplace("dark", "base");  // a variant that is the base's own file
    index.densities.emplace("compact", "missing");
    std::map<std::string, TokenSet, std::less<>> files;
    files.emplace(
        "base",
        tokensOf(
            R"({"stray":{"$type":"number","$value":3},"color":{"$type":"color","accent":{"$value":"#112233"}}})"));
    ui::TokenFileLookup const lookup = [&files](std::string_view id) -> TokenSet const* {
        auto const found = files.find(id);
        return found == files.end() ? nullptr : &found->second;
    };
    auto const unread = ui::unreadThemeTokens(index, lookup);
    REQUIRE(unread.size() == 1);
    CHECK(unread.front().path == "stray");
    CHECK(unread.front().file == "base");
    CHECK(unread.front().unreadType.empty());
    CHECK(unread.front().message().contains("is not a ui-theme/1 token"));
}

TEST_CASE("cov h theme: contrast is measured from black to white and flagged where text sinks into its fill",
          "[covh][ui][theme]") {
    CHECK(ui::detail::luminance(Rgba{.r = 0, .g = 0, .b = 0, .a = 255}) == 0.0);
    CHECK(ui::detail::contrastRatio(Rgba{.r = 0, .g = 0, .b = 0, .a = 255},
                                    Rgba{.r = 255, .g = 255, .b = 255, .a = 255}) > 20.9);
    ui::ThemeValues values;
    values.colors["color.surface"] = Rgba{.r = 255, .g = 255, .b = 255, .a = 255};
    values.colors["color.text"] = Rgba{.r = 250, .g = 250, .b = 250, .a = 255};
    CHECK(ui::themeContrastProblems(values).empty());  // no fill to sit on
    values.colors["color.hover"] = Rgba{.r = 255, .g = 255, .b = 255, .a = 255};
    auto const problems = ui::themeContrastProblems(values);
    REQUIRE(problems.size() == 1);
    CHECK(problems.front().starts_with("color.hover: color.text is 1.0"));
    CHECK(problems.front().contains("below 4.5:1"));
    values.colors["color.muted"] = Rgba{.r = 240, .g = 240, .b = 240, .a = 255};
    CHECK(ui::themeContrastProblems(values).size() == 2);
}

TEST_CASE("cov h theme: a theme's problems are those of each layer that resolves", "[covh][ui][theme]") {
    ui::ThemeIndex index;
    index.base = "base";
    index.surfaces.emplace("faded", "faded");
    index.surfaces.emplace("broken", "broken");
    std::map<std::string, TokenSet, std::less<>> files;
    constexpr std::string_view kColors = R"({"color":{"$type":"color",
        "surface":{"$value":"#ffffff"},"text":{"$value":"#101010"},"hover":{"$value":"#ffffff"}}})";
    files.emplace("base", tokensOf(kColors));
    files.emplace("faded", tokensOf(R"({"color":{"$type":"color","text":{"$value":"#fafafa"}}})"));
    files.emplace("broken", tokensOf(R"({"color":{"$type":"color","text":{"$value":"{color.nowhere}"}}})"));
    ui::TokenFileLookup const lookup = [&files](std::string_view id) -> TokenSet const* {
        auto const found = files.find(id);
        return found == files.end() ? nullptr : &found->second;
    };
    auto const problems = ui::themeProblems(index, lookup);
    CHECK(std::ranges::none_of(problems, [](std::string const& each) { return each.starts_with("base: "); }));
    CHECK(std::ranges::count_if(problems,
                                [](std::string const& each) { return each.starts_with("surface faded: "); }) >= 1);
    CHECK(
        std::ranges::none_of(problems, [](std::string const& each) { return each.starts_with("surface broken: "); }));
    files.insert_or_assign("base", tokensOf(R"({"color":{"$type":"color","surface":{"$value":"#ffffff"},
        "text":{"$value":"#fafafa"},"hover":{"$value":"#ffffff"}}})"));
    auto const worse = ui::themeProblems(index, lookup);
    CHECK(std::ranges::count_if(worse, [](std::string const& each) { return each.starts_with("base: "); }) >= 1);
}
