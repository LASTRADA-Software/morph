// SPDX-License-Identifier: Apache-2.0
//
// Theme tokens (spec 6 §6 and §12 "Tokens"): a DTCG 2025.10 export loads as a theme; variants layer over `base`; an
// alias resolves, inside a composite value too; a cycle and a missing reference refuse; a missing token takes its
// default; an unknown `$type` and an unknown path are ignored.

#include <catch2/catch_test_macros.hpp>
#include <initializer_list>
#include <map>
#include <morph/ui/service.hpp>
#include <morph/ui/theme.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

using morph::ui::Rgba;
using morph::ui::TokenSet;

namespace {

/// A design tool's export: 2025.10 colour objects, dimension objects, nested groups, descriptions, extensions, a
/// composite with an alias inside, and a type this client does not know.
constexpr std::string_view kBase = R"({
  "$description": "Exported by a design tool",
  "color": {
    "$type": "color",
    "surface": { "$value": { "colorSpace": "srgb", "components": [1, 1, 1], "hex": "#ffffff" } },
    "text": { "$value": { "colorSpace": "srgb", "components": [0.0588, 0.0902, 0.1647] },
              "$description": "Body text", "$extensions": { "com.tool": { "id": 17 } } },
    "accent": { "$value": "#2563EB" },
    "focus": { "$value": "{color.accent}" },
    "err": { "$value": { "colorSpace": "srgb", "components": [0.8, 0.1, 0.1], "alpha": 0.5 } }
  },
  "space": { "$type": "dimension", "2": { "$value": { "value": 8, "unit": "px" } } },
  "font": { "family": { "$type": "fontFamily", "$value": ["Inter", "sans-serif"] },
            "weight": { "$type": "fontWeight", "$value": "semi-bold" } },
  "elevation": { "1": { "$type": "shadow", "$value": { "color": "{color.text}", "offsetX": { "value": 0, "unit": "px" },
                                                      "offsetY": { "value": 1, "unit": "px" },
                                                      "blur": { "value": 2, "unit": "px" },
                                                      "spread": { "value": 0, "unit": "px" } } } },
  "motion": { "spring": { "$type": "springy", "$value": 3 } },
  "opacity": { "stale": { "$type": "number", "$value": 0.4 } }
})";

constexpr std::string_view kDark = R"({
  "color": { "$type": "color", "surface": { "$value": "#0f172a" }, "text": { "$value": "#f8fafc" },
             "accent": { "$value": "#60a5fa" } } })";

TokenSet parsed(std::string_view text) {
    auto tokens = morph::ui::parseTokens(text);
    if (!tokens) {
        FAIL(tokens.error());
    }
    return std::move(*tokens);
}

std::string refusal(std::vector<std::string_view> const& files) {
    std::vector<TokenSet> sets;
    sets.reserve(files.size());
    for (std::string_view const file : files) {
        sets.push_back(parsed(file));
    }
    std::vector<TokenSet const*> layers;
    layers.reserve(sets.size());
    for (TokenSet const& set : sets) {
        layers.push_back(&set);
    }
    auto const resolved = morph::ui::resolveTokens(layers);
    return resolved ? std::string{} : resolved.error();
}

}  // namespace

TEST_CASE("theme: a design tool's DTCG 2025.10 export loads and resolves", "[ui][theme]") {
    TokenSet const base = parsed(kBase);
    CHECK_FALSE(base.contains("motion.spring"));
    auto const resolved = morph::ui::resolveTokens({&base});
    REQUIRE(resolved);
    CHECK(morph::ui::colorOf(resolved->at("color.surface").value) == Rgba{.r = 255, .g = 255, .b = 255, .a = 255});
    CHECK(morph::ui::colorOf(resolved->at("color.text").value)->hex() == "#0f172a");
    CHECK(morph::ui::colorOf(resolved->at("color.err").value)->hex() == "#cc1a1a80");
    // An alias takes its target's type and value; inside a composite, the member is replaced.
    CHECK(resolved->at("color.focus").type == "color");
    CHECK(morph::ui::colorOf(resolved->at("color.focus").value)->hex() == "#2563eb");
    auto const& shadow = std::get<morph::ui::Json::object_t>(resolved->at("elevation.1").value.data);
    CHECK(morph::ui::colorOf(shadow.at("color"))->hex() == "#0f172a");
}

TEST_CASE("theme: a variant layers over base, later files winning", "[ui][theme]") {
    TokenSet const base = parsed(kBase);
    TokenSet const dark = parsed(kDark);
    auto const resolved = morph::ui::resolveTokens({&base, &dark});
    REQUIRE(resolved);
    morph::ui::ThemeValues const values = morph::ui::themeValues(*resolved);
    CHECK(values.colors.at("color.surface").hex() == "#0f172a");
    CHECK(values.colors.at("color.text").hex() == "#f8fafc");
    // The alias follows the layered target, not base's.
    CHECK(values.colors.at("color.focus").hex() == "#60a5fa");
    // What dark does not set comes from base.
    CHECK(values.colors.at("color.err").hex() == "#cc1a1a80");
}

TEST_CASE("theme: a missing token takes its default; an unknown path is ignored", "[ui][theme]") {
    morph::ui::ThemeValues const defaults = morph::ui::themeValues();
    CHECK(defaults.colors.at("color.text").hex() == "#1d1d1f");
    CHECK(defaults.numbers.at("opacity.stale") == 0.55);

    TokenSet const base = parsed(kBase);
    auto const resolved = morph::ui::resolveTokens({&base});
    REQUIRE(resolved);
    morph::ui::ThemeValues const values = morph::ui::themeValues(*resolved);
    CHECK(values.numbers.at("opacity.stale") == 0.4);
    CHECK(values.colors.at("color.muted").hex() == "#6e6e73");
    CHECK(values.colors.at("color.warn.bg").hex() == "#fff4e5");
    CHECK_FALSE(values.colors.contains("space.2"));
    CHECK(values.colors.size() + values.numbers.size() == morph::ui::kThemeTokens.size());

    // A path of the set with another type keeps the default.
    TokenSet const wrong = parsed(R"({"color": {"text": {"$type": "number", "$value": 3}}})");
    auto const wrongResolved = morph::ui::resolveTokens({&wrong});
    REQUIRE(wrongResolved);
    CHECK(morph::ui::themeValues(*wrongResolved).colors.at("color.text").hex() == "#1d1d1f");
    // Even when its value would read as a colour.
    TokenSet const named = parsed(R"({"color": {"text": {"$type": "fontFamily", "$value": "#123456"}}})");
    auto const namedResolved = morph::ui::resolveTokens({&named});
    REQUIRE(namedResolved);
    CHECK(morph::ui::themeValues(*namedResolved).colors.at("color.text").hex() == "#1d1d1f");
}

TEST_CASE("theme: an alias cycle and an alias to a missing token refuse", "[ui][theme]") {
    CHECK(refusal({R"({"color": {"$type": "color", "a": {"$value": "{color.b}"}, "b": {"$value": "{color.a}"}}})"})
              .contains("an alias cycle"));
    CHECK(refusal({R"({"color": {"$type": "color", "a": {"$value": "{color.a}"}}})"}).contains("an alias cycle"));
    CHECK(refusal({R"({"color": {"$type": "color", "a": {"$value": "{color.gone}"}}})"})
              .contains("refers to `color.gone`"));
    // A cycle through a composite value refuses too.
    CHECK(refusal({R"({"s": {"$type": "shadow", "$value": {"color": "{c}"}}, "c": {"$value": "{s}"}})"})
              .contains("an alias cycle"));
    // A typed alias to a token of another type refuses.
    CHECK(refusal({R"({"n": {"$type": "number", "$value": 1}, "c": {"$type": "color", "$value": "{n}"}})"})
              .contains("aliases the number token"));
    // A chain too long refuses rather than recursing without bound.
    std::string chain = R"({"t0": {"$type": "number", "$value": 1})";
    for (int index = 1; index <= 100; ++index) {
        chain += R"(, "t)" + std::to_string(index) + R"(": {"$value": "{t)" + std::to_string(index - 1) + R"(}"})";
    }
    chain += '}';
    CHECK(refusal({chain}).contains("an alias chain longer than"));
    // Layering can close a cycle neither file has alone.
    CHECK(refusal({R"({"a": {"$type": "number", "$value": "{b}"}, "b": {"$type": "number", "$value": 1}})",
                   R"({"b": {"$type": "number", "$value": "{a}"}})"})
              .contains("an alias cycle"));
}

TEST_CASE("theme: a malformed token file is refused", "[ui][theme]") {
    auto const refused = [](std::string_view text) { return !morph::ui::parseTokens(text).has_value(); };
    CHECK(refused("{not json"));
    CHECK(refused("[1]"));
    CHECK(refused(R"({"color": 3})"));
    CHECK(refused(R"({"color": {"a.b": {"$type": "color", "$value": "#fff"}}})"));
    CHECK(refused(R"({"!future": {}})"));
    CHECK(refused(R"({"x": {"$value": "#fff"}})"));
    CHECK(refused(R"({"x": {"$type": "color", "$value": "blue"}})"));
    CHECK(refused(R"({"x": {"$type": "color", "$value": {"colorSpace": "display-p3", "components": [1, 0, 0]}}})"));
    CHECK(refused(R"({"x": {"$type": "dimension", "$value": "8px"}})"));
    CHECK(refused(R"({"x": {"$type": "fontWeight", "$value": "heavyish"}})"));
    CHECK(refused(R"({"g": {"$extends": "{other}", "x": {"$type": "number", "$value": 1}}})"));
    // A display-p3 colour with a hex fallback is read through the hex.
    CHECK_FALSE(refused(R"({"x": {"$type": "color", "$value": {"colorSpace": "display-p3", "components": [1, 0, 0],
                                                              "hex": "#ff0000"}}})"));
}

TEST_CASE("theme: an index lists the files of each variant, and every combination is checked", "[ui][theme]") {
    auto const index = morph::ui::parseThemeIndex(R"({"vocab": "ui-theme/1", "base": "theme/base.tokens.json",
      "dark": "theme/dark.tokens.json",
      "density": {"compact": "theme/compact.tokens.json"},
      "surfaces": {"sidebar": "theme/sidebar.tokens.json"}, "icons": {}})");
    REQUIRE(index);
    CHECK(index->variants.size() == 1);
    std::map<std::string, TokenSet, std::less<>> files;
    files.emplace("theme/base.tokens.json", parsed(kBase));
    files.emplace("theme/dark.tokens.json", parsed(kDark));
    files.emplace("theme/compact.tokens.json", parsed(R"({"space": {"2": {"$type": "dimension",
      "$value": {"value": 4, "unit": "px"}}}})"));
    // The sidebar aliases a token only `dark` has a meaning for, which still resolves over base.
    files.emplace("theme/sidebar.tokens.json", parsed(R"({"color": {"surface": {"$value": "{color.text}"}}})"));
    morph::ui::TokenFileLookup const lookup = [&files](std::string_view id) -> TokenSet const* {
        auto const found = files.find(id);
        return found == files.end() ? nullptr : &found->second;
    };
    CHECK_FALSE(morph::ui::checkTheme(*index, lookup).has_value());
    auto const sidebar = morph::ui::resolveTheme(*index, lookup, "dark", "compact", "sidebar");
    REQUIRE(sidebar);
    CHECK(morph::ui::themeValues(*sidebar).colors.at("color.surface").hex() == "#f8fafc");

    // A density whose alias only resolves with some variants is refused for the combination that breaks it.
    files.insert_or_assign("theme/compact.tokens.json",
                           parsed(R"({"color": {"selection": {"$type": "color", "$value": "{color.only.dark}"}}})"));
    files.insert_or_assign("theme/dark.tokens.json",
                           parsed(R"({"color": {"only": {"dark": {"$type": "color", "$value": "#000"}}}})"));
    auto const reason = morph::ui::checkTheme(*index, lookup);
    REQUIRE(reason);
    CHECK(reason->contains("density compact"));
    CHECK(reason->contains("refers to `color.only.dark`"));

    files.erase("theme/sidebar.tokens.json");
    auto const missing = morph::ui::checkTheme(*index, lookup);
    REQUIRE(missing);
    CHECK(missing->contains("`theme/sidebar.tokens.json` is not in the bundle"));
}

TEST_CASE("theme: a malformed index is refused", "[ui][theme]") {
    auto const refused = [](std::string_view text) { return !morph::ui::parseThemeIndex(text).has_value(); };
    CHECK(refused(R"({"vocab": "ui-theme/2", "base": "b"})"));
    CHECK(refused(R"({"vocab": "ui-theme/1"})"));
    CHECK(refused(R"({"vocab": "ui-theme/1", "base": "b", "dark": 3})"));
    CHECK(refused(R"({"vocab": "ui-theme/1", "base": "b", "density": ["compact"]})"));
    CHECK(refused(R"({"vocab": "ui-theme/1", "base": "b", "!future": 1})"));
    CHECK(morph::ui::isThemeIndex(R"({"vocab": "ui-theme/1", "base": "b"})"));
    CHECK_FALSE(morph::ui::isThemeIndex(kBase));
}

TEST_CASE("theme: the UI service refuses a malformed or unresolvable theme when the bundle is registered",
          "[ui][theme][ui-service]") {
    auto const registry = [] {
        return morph::ui::UiRegistry{morph::ui::ApplicationInfo{.id = "lims", .version = "1"}};
    };
    auto const theme = [](std::string id, std::string_view bytes) {
        return morph::ui::BundleFile{
            .kind = "theme", .id = std::move(id), .bytes = std::string{bytes}, .isPublic = true};
    };
    constexpr std::string_view kIndex =
        R"({"vocab": "ui-theme/1", "base": "theme/base.tokens.json", "dark": "theme/dark.tokens.json"})";

    morph::ui::UiRegistry good = registry();
    REQUIRE(good.addFile(theme("theme", kIndex)));
    REQUIRE(good.addFile(theme("theme/base.tokens.json", kBase)));
    REQUIRE(good.addFile(theme("theme/dark.tokens.json", kDark)));
    CHECK(good.build());

    // A malformed token file or index is refused when it is added.
    morph::ui::UiRegistry early = registry();
    auto const badTokens =
        early.addFile(theme("theme/x.tokens.json", R"({"x": {"$type": "color", "$value": "blue"}})"));
    REQUIRE_FALSE(badTokens);
    CHECK(badTokens.error().message().contains("the token file"));
    auto const badIndex = early.addFile(theme("theme", R"({"vocab": "ui-theme/1"})"));
    REQUIRE_FALSE(badIndex);
    CHECK(badIndex.error().message().contains("the theme index"));

    // A cycle across files, and a file the index names that the bundle lacks, refuse the bundle at build.
    morph::ui::UiRegistry cyclic = registry();
    REQUIRE(cyclic.addFile(theme("theme", kIndex)));
    REQUIRE(cyclic.addFile(theme("theme/base.tokens.json", kBase)));
    REQUIRE(cyclic.addFile(
        theme("theme/dark.tokens.json", R"({"color": {"$type": "color", "accent": {"$value": "{color.focus}"}}})")));
    auto const cycle = cyclic.build();
    REQUIRE_FALSE(cycle);
    CHECK(cycle.error().message().contains("base + dark: an alias cycle"));

    morph::ui::UiRegistry missing = registry();
    REQUIRE(missing.addFile(theme("theme", kIndex)));
    REQUIRE(missing.addFile(theme("theme/base.tokens.json", kBase)));
    auto const absent = missing.build();
    REQUIRE_FALSE(absent);
    CHECK(absent.error().message().contains("`theme/dark.tokens.json` is not in the bundle"));
}

TEST_CASE("theme: every colour form reads, and every malformed one is refused", "[ui][theme]") {
    auto const color = [](std::string_view json) -> std::optional<std::string> {
        auto const parsed = morph::ui::parseJson(json);
        REQUIRE(parsed);
        auto const read = morph::ui::colorOf(*parsed);
        return read ? std::optional<std::string>{read->hex()} : std::nullopt;
    };
    CHECK(color(R"("#1aF")") == "#11aaff");
    CHECK(color(R"("#0A0b0C")") == "#0a0b0c");
    CHECK(color(R"("#0a0b0c80")") == "#0a0b0c80");
    CHECK(color(R"({"hex": "#123456", "alpha": 0})") == "#12345600");
    CHECK(color(R"({"colorSpace": "srgb", "components": [0, 0.5, 1], "alpha": 1})") == "#0080ff");
    CHECK(color(R"({"colorSpace": "srgb", "components": [0, 0, 1]})") == "#0000ff");
    for (std::string_view const bad : std::initializer_list<std::string_view>{
             R"("123456")", R"("#12g456")", R"("#12!456")", R"("#12@456")", R"("#1234")", R"("#")", "3", "[]",
             R"({"hex": 3})", R"({"hex": "nope"})", R"({"hex": "#fff", "alpha": 2})",
             R"({"hex": "#fff", "alpha": "x"})", R"({"components": [0, 0, 0]})", R"({"colorSpace": "srgb"})",
             R"({"colorSpace": "srgb", "components": [0, 0]})",
             R"({"colorSpace": "srgb", "components": [0, 0, -0.1]})",
             R"({"colorSpace": "srgb", "components": [0, "a", 0]})", R"({"colorSpace": 3, "components": [0, 0, 0]})",
             R"({"colorSpace": "srgb", "components": {}})"}) {
        CAPTURE(bad);
        CHECK_FALSE(color(bad).has_value());
    }
}

TEST_CASE("theme: every simple type's shape is checked", "[ui][theme]") {
    auto const accepted = [](std::string_view type, std::string_view value) {
        std::string const text =
            R"({"x": {"$type": ")" + std::string{type} + R"(", "$value": )" + std::string{value} + "}}";
        auto const tokens = morph::ui::parseTokens(text);
        if (!tokens) {
            UNSCOPED_INFO(tokens.error());
        }
        return tokens.has_value();
    };
    CHECK(accepted("dimension", R"({"value": 1.5, "unit": "rem"})"));
    CHECK(accepted("dimension", R"({"value": -2, "unit": "px"})"));
    CHECK_FALSE(accepted("dimension", R"({"value": 1, "unit": "em"})"));
    CHECK_FALSE(accepted("dimension", R"({"value": "1", "unit": "px"})"));
    CHECK_FALSE(accepted("dimension", R"({"unit": "px"})"));
    CHECK_FALSE(accepted("dimension", R"({"value": 1, "unit": 3})"));
    CHECK(accepted("duration", R"({"value": 200, "unit": "ms"})"));
    CHECK(accepted("duration", R"({"value": 0.5, "unit": "s"})"));
    CHECK_FALSE(accepted("duration", R"({"value": 1, "unit": "px"})"));
    CHECK(accepted("number", "-3"));
    CHECK_FALSE(accepted("number", R"("3")"));
    CHECK(accepted("fontFamily", R"("Inter")"));
    CHECK_FALSE(accepted("fontFamily", "[]"));
    CHECK_FALSE(accepted("fontFamily", R"(["Inter", 3])"));
    CHECK_FALSE(accepted("fontFamily", "3"));
    CHECK(accepted("fontWeight", "700"));
    CHECK(accepted("fontWeight", R"("bold")"));
    CHECK_FALSE(accepted("fontWeight", "0"));
    CHECK_FALSE(accepted("fontWeight", "1001"));
    CHECK_FALSE(accepted("fontWeight", "true"));
    CHECK(accepted("shadow", R"({"anything": true})"));
}

TEST_CASE("theme: declarations and bounds of a token file are checked", "[ui][theme]") {
    auto const reason = [](std::string_view text) {
        auto const tokens = morph::ui::parseTokens(text);
        return tokens ? std::string{} : tokens.error();
    };
    CHECK(reason(R"({"g": {"$type": 3, "x": {"$value": 1}}})").contains("`g.$type` is a string"));
    CHECK(reason(R"({"x": {"$type": 3, "$value": 1}})").contains("`x.$type` is a string"));
    // A composite holding an alias still needs a type; only a plain alias takes its target's.
    CHECK(reason(R"({"x": {"$value": {"color": "{c}"}}})").contains("has no `$type`"));
    CHECK(reason(R"({"x": {"$value": "{c}"}})").empty());
    CHECK(reason(R"({"x": {"$type": "number", "$value": "{}"}})").contains("not a number"));
    CHECK(reason(R"({"x": {"$type": "number", "$value": "{c"}})").contains("not a number"));
    CHECK(reason(R"({"x": {"$type": "number", "$value": "{abc"}})").contains("not a number"));

    std::string many = "{";
    for (std::size_t index = 0; index <= morph::ui::kMaxThemeTokens; ++index) {
        many += (index == 0 ? "" : ",") + std::string{R"("t)"} + std::to_string(index) +
                R"(": {"$type": "number", "$value": 1})";
    }
    many += '}';
    CHECK(reason(many).contains("more than 16384 tokens"));
}

TEST_CASE("theme: an alias inside a list resolves, and a resolved value is checked against its type", "[ui][theme]") {
    CHECK(refusal({R"({"f": {"$type": "fontFamily", "$value": ["{base}", "sans-serif"]},
                       "base": {"$type": "fontFamily", "$value": ["Inter"]}})"})
              .contains("a font family is a name or a list of names"));
    CHECK(refusal({R"({"f": {"$type": "fontFamily", "$value": ["{gone}"]}})"}).contains("refers to `gone`"));
    CHECK(refusal({R"({"c": {"$type": "color", "$value": {"colorSpace": "srgb", "components": ["{n}", 0, 0]}},
                       "n": {"$type": "number", "$value": 0.5}})"})
              .empty());
    CHECK(refusal({R"({"c": {"$type": "color", "$value": {"colorSpace": "srgb", "components": ["{d}", 0, 0]}},
                       "d": {"$type": "dimension", "$value": {"value": 1, "unit": "px"}}})"})
              .contains("the token `c`: not a colour"));
}

TEST_CASE("theme: an index's maps and bounds are checked, and an unlisted name is refused", "[ui][theme]") {
    auto const reason = [](std::string_view text) {
        auto const index = morph::ui::parseThemeIndex(text);
        return index ? std::string{} : index.error();
    };
    CHECK_FALSE(reason("{nope").empty());
    CHECK(reason("[1]").contains("an object"));
    CHECK(reason(R"({"vocab": 1, "base": "b"})").contains("vocab is ui-theme/1"));
    CHECK(reason(R"({"vocab": "ui-theme/1", "base": "b", "dark": ""})").contains("`dark` names a token file"));
    CHECK(reason(R"({"vocab": "ui-theme/1", "base": "b", "density": {"compact": 3}})")
              .contains("`density.compact` names a token file"));
    CHECK(reason(R"({"vocab": "ui-theme/1", "base": "b", "surfaces": {"rail": ""}})")
              .contains("`surfaces.rail` names a token file"));
    CHECK(reason(R"({"vocab": "ui-theme/1", "base": "b", "surfaces": "rail"})").contains("`surfaces` is an object"));
    for (std::string_view const key : {"variant", "density", "surfaces"}) {
        std::string text = R"({"vocab": "ui-theme/1", "base": "b")";
        std::string list;
        for (std::size_t index = 0; index <= morph::ui::kMaxThemeVariants; ++index) {
            std::string const name = "v" + std::to_string(index);
            if (key == "variant") {
                text += R"(,")" + name + R"(": "f")";
            } else {
                list += (index == 0 ? R"(")" : R"(,")") + name + R"(": "f")";
            }
        }
        if (key != "variant") {
            text += R"(,")" + std::string{key} + R"(": {)" + list + "}";
        }
        text += '}';
        CAPTURE(key);
        CHECK(reason(text).contains("more than 16"));
    }
    CHECK_FALSE(morph::ui::isThemeIndex("{nope"));
    CHECK_FALSE(morph::ui::isThemeIndex("[1]"));
    CHECK_FALSE(morph::ui::isThemeIndex(R"({"vocab": 1})"));

    auto const index = morph::ui::parseThemeIndex(R"({"vocab": "ui-theme/1", "base": "b", "dark": "d",
      "density": {"compact": "c"}, "surfaces": {"rail": "r"}})");
    REQUIRE(index);
    TokenSet const empty;
    morph::ui::TokenFileLookup const lookup = [&empty](std::string_view) -> TokenSet const* { return &empty; };
    for (auto const& [variant, density, surface, what] :
         std::initializer_list<std::tuple<std::string_view, std::string_view, std::string_view, std::string_view>>{
             {"light", "", "", "the theme has no variant `light`"},
             {"", "cosy", "", "the theme has no density `cosy`"},
             {"", "", "sidebar", "the theme has no surface `sidebar`"}}) {
        auto const resolved = morph::ui::resolveTheme(*index, lookup, variant, density, surface);
        REQUIRE_FALSE(resolved);
        CHECK(resolved.error() == what);
    }
    CHECK(morph::ui::resolveTheme(*index, lookup, "dark", "compact", "rail"));
    morph::ui::TokenFileLookup const none = [](std::string_view) -> TokenSet const* { return nullptr; };
    auto const noBase = morph::ui::resolveTheme(*index, none, "dark", "", "");
    REQUIRE_FALSE(noBase);
    CHECK(noBase.error() == "the token file `b` is not in the bundle");
    CHECK(reason(R"({"base": "b"})").contains("vocab is ui-theme/1"));
}

TEST_CASE("theme: a number of the set takes the theme's value only when its type is number", "[ui][theme]") {
    TokenSet const tokens = parsed(R"({"opacity": {"stale": {"$type": "color", "$value": "#fff"}}})");
    auto const resolved = morph::ui::resolveTokens({&tokens});
    REQUIRE(resolved);
    CHECK(morph::ui::themeValues(*resolved).numbers.at("opacity.stale") == 0.55);
}
