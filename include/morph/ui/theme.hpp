// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "bundle.hpp"
#include "value.hpp"

/// @file
/// @brief Theme tokens (`ui-theme/1`, spec 6 §6): DTCG 2025.10 token files read and checked, layered into a variant,
///        their aliases resolved, and the closed `ui-theme/1` set filled from them with its defaults. No toolkit: the
///        Qt Quick renderer applies the result (`qt_quick::applyTheme`).
///
/// Specified in `docs/spec/ui/theme.md`.

namespace morph::ui {

/// @brief The vocabulary of a theme index.
inline constexpr std::string_view kThemeVocabulary = "ui-theme/1";

/// @brief The most tokens one token file may hold.
inline constexpr std::size_t kMaxThemeTokens = 16384;

/// @brief The most variants, densities or surfaces a theme index may list, each.
inline constexpr std::size_t kMaxThemeVariants = 16;

/// @brief The most names a theme index's icon set may hold.
inline constexpr std::size_t kMaxThemeIcons = 256;

/// @brief The icon names the built-in components draw: an empty state without an icon of its own, a banner's tone,
///        a chevron that opens and closes, a close button, a date input's calendar, an overflow menu's trigger, and
///        the compact shell's navigation menu. A bundle's icon set replaces any of them by name.
inline constexpr std::array<std::string_view, 13> kFrameworkIcons = {
    "empty",      "tone.info",    "tone.ok", "tone.warn", "tone.err", "nav.back", "nav.forward",
    "nav.expand", "nav.collapse", "close",   "date.pick", "more",     "nav.menu"};

/// @brief Whether @p name is an icon name: `[a-z][a-z0-9.-]{0,63}`.
/// @param name The name.
/// @return It.
[[nodiscard]] inline bool isIconName(std::string_view name) {
    if (name.empty() || name.size() > 64 || name.front() < 'a' || name.front() > 'z') {
        return false;
    }
    return std::ranges::all_of(name, [](char const chr) {
        return (chr >= 'a' && chr <= 'z') || (chr >= '0' && chr <= '9') || chr == '.' || chr == '-';
    });
}

/// @brief The glyph a text renderer draws for a framework icon name.
/// @param name The icon name.
/// @return The glyph, or empty for a name that has none (`empty`, which a text renderer draws as nothing).
[[nodiscard]] inline std::string_view iconGlyph(std::string_view name) {
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 12> kGlyphs = {{
        {"tone.ok", "✓"},
        {"tone.warn", "!"},
        {"tone.err", "✗"},
        {"tone.info", "i"},
        {"nav.expand", "▸"},
        {"nav.collapse", "▾"},
        {"nav.back", "‹"},
        {"nav.forward", "›"},
        {"close", "×"},
        {"date.pick", "▦"},
        {"more", "⋯"},
        {"nav.menu", "☰"},
    }};
    for (auto const& [icon, glyph] : kGlyphs) {
        if (icon == name) {
            return glyph;
        }
    }
    return {};
}

/// @brief The longest chain of aliases a token may resolve through.
inline constexpr std::size_t kMaxAliasChain = 64;

/// @brief One token: its DTCG type and its value, which may hold aliases until the set is resolved.
struct Token {
    /// @brief The `$type`, own or inherited; empty for an alias whose type is its target's.
    std::string type;
    /// @brief The `$value`.
    Json value;
};

/// @brief Tokens by their dotted path, such as `color.accent`.
using TokenSet = std::map<std::string, Token, std::less<>>;

/// @brief A colour, 8 bits per channel.
struct Rgba {
    /// @brief Red.
    std::uint8_t r = 0;
    /// @brief Green.
    std::uint8_t g = 0;
    /// @brief Blue.
    std::uint8_t b = 0;
    /// @brief Alpha; 255 is opaque.
    std::uint8_t a = 255;

    /// @brief Field-wise equality.
    /// @return Whether every channel is equal.
    bool operator==(Rgba const&) const = default;

    /// @brief The colour as `#rrggbb`, or `#rrggbbaa` when it is not opaque.
    /// @return The text.
    [[nodiscard]] std::string hex() const {
        static constexpr std::string_view kDigits = "0123456789abcdef";
        std::string out = "#";
        auto const put = [&out](std::uint8_t const channel) {
            out += kDigits.at(static_cast<std::size_t>(channel >> 4U));
            out += kDigits.at(static_cast<std::size_t>(channel & 0xFU));
        };
        put(r);
        put(g);
        put(b);
        if (a != 255) {
            put(a);
        }
        return out;
    }
};

/// @brief The index of a theme: which token files make up each variant.
struct ThemeIndex {
    /// @brief The file every variant layers over.
    std::string base;
    /// @brief Named variants, such as `light` and `dark`, each one file.
    std::map<std::string, std::string, std::less<>> variants;
    /// @brief Densities, such as `comfortable` and `compact`, each one file.
    std::map<std::string, std::string, std::less<>> densities;
    /// @brief Named surfaces, such as `sidebar`, each one file that overrides the variant for a subtree.
    std::map<std::string, std::string, std::less<>> surfaces;
    /// @brief The icon set: icon name to the bundle's `icon` file that draws it; empty for none.
    std::map<std::string, std::string, std::less<>> icons;
};

namespace detail {

/// @brief A number of a JSON value.
/// @param json The value.
/// @return It, or nothing when it is not a number.
[[nodiscard]] inline std::optional<double> numberOf(Json const& json) {
    if (auto const* const value = std::get_if<double>(&json.data)) {
        return *value;
    }
    if (auto const* const value = std::get_if<std::int64_t>(&json.data)) {
        return static_cast<double>(*value);
    }
    if (auto const* const value = std::get_if<std::uint64_t>(&json.data)) {
        return static_cast<double>(*value);
    }
    return std::nullopt;
}

/// @brief A member of a JSON object.
/// @param object The object.
/// @param name   The member.
/// @return It, or null.
[[nodiscard]] inline Json const* memberOf(Json::object_t const& object, std::string_view name) {
    auto const* const found = object.find(name);
    return found == object.end() ? nullptr : &found->second;
}

/// @brief How many px a `rem` is: the default root font size, which the renderers do not change.
inline constexpr double kPixelsPerRem = 16.0;

/// @brief A DTCG `dimension` value in px.
/// @param json The value: an object of a number `value` and the unit `px` or `rem`.
/// @return Its length in px, or nothing when it is not a dimension.
[[nodiscard]] inline std::optional<double> pixelsOf(Json const& json) {
    auto const* const object = std::get_if<Json::object_t>(&json.data);
    if (object == nullptr) {
        return std::nullopt;
    }
    Json const* const value = memberOf(*object, "value");
    Json const* const unit = memberOf(*object, "unit");
    auto const length = value != nullptr ? numberOf(*value) : std::nullopt;
    auto const* const unitText = unit != nullptr ? std::get_if<std::string>(&unit->data) : nullptr;
    if (!length || unitText == nullptr) {
        return std::nullopt;
    }
    if (*unitText == "px") {
        return length;
    }
    if (*unitText == "rem") {
        return *length * kPixelsPerRem;
    }
    return std::nullopt;
}

/// @brief Reads `#rgb`, `#rrggbb` or `#rrggbbaa`.
/// @param text The text.
/// @return The colour, or nothing.
[[nodiscard]] inline std::optional<Rgba> hexColor(std::string_view text) {
    if (!text.starts_with('#')) {
        return std::nullopt;
    }
    text.remove_prefix(1);
    auto const digit = [](char const symbol) -> int {
        if (symbol >= '0' && symbol <= '9') {
            return symbol - '0';
        }
        if (symbol >= 'a' && symbol <= 'f') {
            return symbol - 'a' + 10;
        }
        if (symbol >= 'A' && symbol <= 'F') {
            return symbol - 'A' + 10;
        }
        return -1;
    };
    if (!std::ranges::all_of(text, [&digit](char const symbol) { return digit(symbol) >= 0; })) {
        return std::nullopt;
    }
    auto const pair = [&digit, text](std::size_t const offset) {
        return static_cast<std::uint8_t>((digit(text.at(offset)) * 16) + digit(text.at(offset + 1)));
    };
    if (text.size() == 3) {
        auto const single = [&digit, text](std::size_t const offset) {
            return static_cast<std::uint8_t>(digit(text.at(offset)) * 17);
        };
        return Rgba{.r = single(0), .g = single(1), .b = single(2), .a = 255};
    }
    if (text.size() == 6 || text.size() == 8) {
        return Rgba{.r = pair(0), .g = pair(2), .b = pair(4), .a = text.size() == 8 ? pair(6) : std::uint8_t{255}};
    }
    return std::nullopt;
}

/// @brief A channel from 0 to 1, as 8 bits.
/// @param value The channel.
/// @return It, or nothing outside 0 to 1.
[[nodiscard]] inline std::optional<std::uint8_t> channelOf(double const value) {
    if (value < 0.0 || value > 1.0) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(std::lround(value * 255.0));
}

/// @brief The member name that holds a group's own token.
inline constexpr std::string_view kRootMember = "$root";

/// @brief Whether a string is an alias, `{a.b}`.
/// @param text The text.
/// @return The path it names, or nothing. A trailing `.$root` is dropped: `{color.ok.$root}` names `color.ok`, the
///         group's own token.
[[nodiscard]] inline std::optional<std::string_view> aliasOf(std::string_view text) {
    if (text.size() < 3 || !text.starts_with('{') || !text.ends_with('}')) {
        return std::nullopt;
    }
    std::string_view path = text.substr(1, text.size() - 2);
    constexpr std::string_view suffix = ".$root";
    if (path.size() > suffix.size() && path.ends_with(suffix)) {
        path.remove_suffix(suffix.size());
    }
    return path;
}

/// @brief Whether a value holds an alias anywhere.
/// @param json The value.
/// @return Whether it does.
// NOLINTNEXTLINE(misc-no-recursion): a composite value is walked member by member, bounded by kMaxJsonDepth.
[[nodiscard]] inline bool holdsAlias(Json const& json) {
    if (auto const* const text = std::get_if<std::string>(&json.data)) {
        return aliasOf(*text).has_value();
    }
    if (auto const* const array = std::get_if<Json::array_t>(&json.data)) {
        for (Json const& item : *array) {
            if (holdsAlias(item)) {
                return true;
            }
        }
    }
    if (auto const* const object = std::get_if<Json::object_t>(&json.data)) {
        for (auto const& [name, member] : *object) {
            static_cast<void>(name);
            if (holdsAlias(member)) {
                return true;
            }
        }
    }
    return false;
}

/// @brief The types the client checks; a token of another `$type` the client still knows is accepted unchecked.
inline constexpr std::array<std::string_view, 14> kKnownTokenTypes = {
    "color",  "dimension",  "number", "fontFamily",  "fontWeight", "duration",   "cubicBezier",
    "shadow", "typography", "border", "strokeStyle", "gradient",   "transition", "keyword"};

/// @brief The named font weights of DTCG.
inline constexpr std::array<std::string_view, 18> kFontWeights = {
    "thin",       "hairline",   "extra-light", "ultra-light", "light",       "normal",
    "regular",    "book",       "medium",      "semi-bold",   "demi-bold",   "bold",
    "extra-bold", "ultra-bold", "black",       "heavy",       "extra-black", "ultra-black"};

/// @brief Whether a resolved value has its type's shape.
/// @param type  The `$type`.
/// @param value The value, with no alias left.
/// @return Why it does not, or nothing.
[[nodiscard]] inline std::optional<std::string> checkTokenValue(std::string_view type, Json const& value);

}  // namespace detail

/// @brief Reads a colour value: `#rgb`, `#rrggbb`, `#rrggbbaa`, or the DTCG 2025.10 object, by its `hex` or by its
///        `srgb` `components` (0 to 1) and `alpha`.
/// @param json The value.
/// @return The colour, or nothing when it is none of those.
[[nodiscard]] inline std::optional<Rgba> colorOf(Json const& json) {
    if (auto const* const text = std::get_if<std::string>(&json.data)) {
        return detail::hexColor(*text);
    }
    auto const* const object = std::get_if<Json::object_t>(&json.data);
    if (object == nullptr) {
        return std::nullopt;
    }
    std::optional<double> alpha = 1.0;
    if (Json const* const given = detail::memberOf(*object, "alpha")) {
        alpha = detail::numberOf(*given);
    }
    auto const opacity = alpha ? detail::channelOf(*alpha) : std::nullopt;
    if (!opacity) {
        return std::nullopt;
    }
    if (Json const* const hex = detail::memberOf(*object, "hex")) {
        auto const* const text = std::get_if<std::string>(&hex->data);
        auto color = text != nullptr ? detail::hexColor(*text) : std::nullopt;
        if (color) {
            color->a = *opacity;
        }
        return color;
    }
    Json const* const space = detail::memberOf(*object, "colorSpace");
    Json const* const components = detail::memberOf(*object, "components");
    auto const* const spaceName = space != nullptr ? std::get_if<std::string>(&space->data) : nullptr;
    auto const* const list = components != nullptr ? std::get_if<Json::array_t>(&components->data) : nullptr;
    if (spaceName == nullptr || *spaceName != "srgb" || list == nullptr || list->size() != 3) {
        return std::nullopt;
    }
    std::array<std::uint8_t, 3> channels{};
    for (std::size_t index = 0; index < 3; ++index) {
        auto const number = detail::numberOf(list->at(index));
        auto const channel = number ? detail::channelOf(*number) : std::nullopt;
        if (!channel) {
            return std::nullopt;
        }
        channels.at(index) = *channel;
    }
    return Rgba{.r = channels.at(0), .g = channels.at(1), .b = channels.at(2), .a = *opacity};
}

namespace detail {

/// @brief Checks a `dimension` or a `duration`: an object of a number `value` and a known `unit`.
/// @param type  `dimension` or `duration`.
/// @param value The value.
/// @return Why it is not one, or nothing.
[[nodiscard]] inline std::optional<std::string> checkMeasure(std::string_view type, Json const& value) {
    bool const dimension = type == "dimension";
    auto const* const object = std::get_if<Json::object_t>(&value.data);
    Json const* const number = object != nullptr ? memberOf(*object, "value") : nullptr;
    Json const* const unit = object != nullptr ? memberOf(*object, "unit") : nullptr;
    auto const* const unitText = unit != nullptr ? std::get_if<std::string>(&unit->data) : nullptr;
    std::array<std::string_view, 2> const units =
        dimension ? std::array<std::string_view, 2>{"px", "rem"} : std::array<std::string_view, 2>{"ms", "s"};
    bool const known = unitText != nullptr && std::ranges::find(units, *unitText) != units.end();
    if (number != nullptr && numberOf(*number) && known) {
        return std::nullopt;
    }
    return dimension ? std::string{"a dimension is an object of a number value and the unit px or rem"}
                     : std::string{"a duration is an object of a number value and the unit ms or s"};
}

/// @brief Checks a `fontFamily`: a name, or a non-empty list of names.
/// @param value The value.
/// @return Why it is not one, or nothing.
[[nodiscard]] inline std::optional<std::string> checkFontFamily(Json const& value) {
    if (std::holds_alternative<std::string>(value.data)) {
        return std::nullopt;
    }
    auto const* const names = std::get_if<Json::array_t>(&value.data);
    bool const allNames = names != nullptr && !names->empty() && std::ranges::all_of(*names, [](Json const& name) {
                              return std::holds_alternative<std::string>(name.data);
                          });
    return allNames ? std::nullopt : std::optional<std::string>{"a font family is a name or a list of names"};
}

/// @brief Checks a `fontWeight`: 1 to 1000, or a DTCG weight name.
/// @param value The value.
/// @return Why it is not one, or nothing.
[[nodiscard]] inline std::optional<std::string> checkFontWeight(Json const& value) {
    if (auto const number = numberOf(value)) {
        return *number >= 1.0 && *number <= 1000.0 ? std::nullopt
                                                   : std::optional<std::string>{"a weight is 1 to 1000"};
    }
    auto const* const name = std::get_if<std::string>(&value.data);
    return name != nullptr && std::ranges::find(kFontWeights, *name) != kFontWeights.end()
               ? std::nullopt
               : std::optional<std::string>{"not a font weight"};
}

/// @brief The numeric weight of a checked `fontWeight` value.
/// @param value The value, checked by `checkFontWeight`: a number from 1 to 1000, or a DTCG weight name.
/// @return The weight, or nothing when it is neither.
[[nodiscard]] inline std::optional<double> weightOf(Json const& value);

/// @brief Checks a `shadow`: an object of a `color` and the dimensions `offsetX`, `offsetY`, `blur` and `spread`, or a
///        non-empty list of them.
/// @param value The value.
/// @return Why it is not one, or nothing.
[[nodiscard]] inline std::optional<std::string> checkShadow(Json const& value) {
    auto const one = [](Json const& layer) {
        auto const* const object = std::get_if<Json::object_t>(&layer.data);
        if (object == nullptr) {
            return false;
        }
        Json const* const color = memberOf(*object, "color");
        if (color == nullptr || !colorOf(*color)) {
            return false;
        }
        return std::ranges::all_of(std::array<std::string_view, 4>{"offsetX", "offsetY", "blur", "spread"},
                                   [object](std::string_view name) {
                                       Json const* const length = memberOf(*object, name);
                                       return length != nullptr && !checkMeasure("dimension", *length);
                                   });
    };
    if (auto const* const layers = std::get_if<Json::array_t>(&value.data)) {
        if (!layers->empty() && std::ranges::all_of(*layers, one)) {
            return std::nullopt;
        }
    } else if (one(value)) {
        return std::nullopt;
    }
    return std::string{"a shadow is an object of a colour and the dimensions offsetX, offsetY, blur and spread"};
}

inline std::optional<double> weightOf(Json const& value) {
    if (auto const number = numberOf(value)) {
        return number;
    }
    auto const* const name = std::get_if<std::string>(&value.data);
    if (name == nullptr) {
        return std::nullopt;
    }
    // The weights DTCG gives its names, in the order of `kFontWeights`.
    static constexpr std::array<double, 18> kValues = {100, 100, 200, 200, 300, 400, 400, 400, 500,
                                                       600, 600, 700, 800, 800, 900, 900, 950, 950};
    auto const found = std::ranges::find(kFontWeights, *name);
    if (found == kFontWeights.end()) {
        return std::nullopt;
    }
    return kValues.at(static_cast<std::size_t>(found - kFontWeights.begin()));
}

inline std::optional<std::string> checkTokenValue(std::string_view type, Json const& value) {
    if (type == "shadow") {
        return checkShadow(value);
    }
    if (type == "color") {
        return colorOf(value) ? std::nullopt : std::optional<std::string>{"not a colour in sRGB or hex"};
    }
    if (type == "dimension" || type == "duration") {
        return checkMeasure(type, value);
    }
    if (type == "number") {
        return numberOf(value) ? std::nullopt : std::optional<std::string>{"not a number"};
    }
    if (type == "fontFamily") {
        return checkFontFamily(value);
    }
    if (type == "fontWeight") {
        return checkFontWeight(value);
    }
    return std::nullopt;
}

/// @brief Joins text, for a refusal built inside a loop.
/// @param parts The pieces.
/// @return Them, in order.
[[nodiscard]] inline std::string joined(std::initializer_list<std::string_view> parts) {
    std::string out;
    for (std::string_view const part : parts) {
        out += part;
    }
    return out;
}

/// @brief The `$type` an object declares, or the one it inherits.
/// @param object    The group's or token's members.
/// @param where     Its path with a trailing `.`, for the refusal.
/// @param inherited The nearest enclosing `$type`.
/// @return The type, or why the declaration is refused.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the path, then the type passed down, as readGroup takes them.
[[nodiscard]] inline std::expected<std::string, std::string> declaredType(Json::object_t const& object,
                                                                          std::string_view where,
                                                                          std::string const& inherited) {
    Json const* const declared = memberOf(object, "$type");
    if (declared == nullptr) {
        return inherited;
    }
    auto const* const text = std::get_if<std::string>(&declared->data);
    if (text == nullptr) {
        return std::unexpected(joined({"`", where, "$type` is a string"}));
    }
    return *text;
}

/// @brief Reads one token into @p out.
/// @param path   The token's path.
/// @param object The token's members.
/// @param value  Its `$value`.
/// @param type   The type it inherits.
/// @param out    Where it goes.
/// @return Why the file is refused, or nothing.
[[nodiscard]] inline std::optional<std::string> readToken(std::string const& path, Json::object_t const& object,
                                                          Json const& value, std::string const& type, TokenSet& out) {
    for (auto const& [name, member] : object) {
        static_cast<void>(member);
        if (!name.starts_with('$')) {
            return joined({"the token `", path, "` holds `", name, "`: a token holds no tokens; write `", path,
                           "` as a group whose `$root` is the token"});
        }
    }
    auto tokenType = declaredType(object, path + ".", type);
    if (!tokenType) {
        return std::move(tokenType.error());
    }
    bool const alias = holdsAlias(value);
    if (tokenType->empty()) {
        if (alias && std::holds_alternative<std::string>(value.data)) {
            out.insert_or_assign(path, Token{.type = {}, .value = value});
            return std::nullopt;
        }
        return joined({"the token `", path, "` has no `$type`"});
    }
    if (std::ranges::find(kKnownTokenTypes, *tokenType) == kKnownTokenTypes.end()) {
        // Kept unchecked with its type, so the report of tokens nothing reads can name it.
        out.insert_or_assign(path, Token{.type = std::move(*tokenType), .value = value});
        return std::nullopt;
    }
    if (!alias) {
        if (auto reason = checkTokenValue(*tokenType, value)) {
            return joined({"the token `", path, "`: ", *reason});
        }
    }
    out.insert_or_assign(path, Token{.type = std::move(*tokenType), .value = value});
    return std::nullopt;
}

/// @brief Refuses a member name a token file may not have.
/// @param prefix The group's path, with a trailing `.`.
/// @param name   The member's name.
/// @return Why it is refused, or nothing.
[[nodiscard]] inline std::optional<std::string> checkMemberName(std::string const& prefix, std::string const& name) {
    if (name.starts_with('!')) {
        return joined({"the must-understand key `", prefix, name, "` is not understood"});
    }
    if (name.find_first_of(".{}") != std::string::npos) {
        return joined({"the token or group name `", prefix, name, "` has `.`, `{` or `}`"});
    }
    return std::nullopt;
}

/// @brief Reads a group's `$root` token: the token at the group's own path.
/// @param member    The `$root` member.
/// @param prefix    The group's path, with a trailing `.`; empty at the top, where there is no path to name.
/// @param inherited The group's `$type`.
/// @param out       Where the token goes.
/// @return Why the file is refused, or nothing.
[[nodiscard]] inline std::optional<std::string> readRoot(Json const& member, std::string const& prefix,
                                                         std::string const& inherited, TokenSet& out) {
    if (prefix.empty()) {
        return std::string{"`$root` names a group's own token, and the top of a file is no group"};
    }
    std::string const path = prefix.substr(0, prefix.size() - 1);
    auto const* const object = std::get_if<Json::object_t>(&member.data);
    Json const* const value = object != nullptr ? memberOf(*object, "$value") : nullptr;
    if (value == nullptr) {
        return joined({"`", prefix, "$root` is a token, an object with a `$value`"});
    }
    return readToken(path, *object, *value, inherited, out);
}

/// @brief Reads the tokens of one group into @p out.
/// @param group     The group's members.
/// @param prefix    The group's path, with a trailing `.`; empty at the top.
/// @param inherited The nearest enclosing `$type`.
/// @param out       Where the tokens go.
/// @return Why the file is refused, or nothing.
// NOLINTBEGIN(misc-no-recursion,bugprone-easily-swappable-parameters): groups nest, bounded by kMaxJsonDepth; the
// group's path, then the type it passes down.
[[nodiscard]] inline std::optional<std::string> readGroup(Json::object_t const& group, std::string const& prefix,
                                                          std::string const& inherited, TokenSet& out) {
    // NOLINTEND(misc-no-recursion,bugprone-easily-swappable-parameters)
    auto const type = declaredType(group, prefix, inherited);
    if (!type) {
        return type.error();
    }
    if (memberOf(group, "$extends") != nullptr) {
        return joined({"`", prefix, "$extends`: group inheritance is not supported"});
    }
    for (auto const& [name, member] : group) {
        if (name == kRootMember) {
            if (auto reason = readRoot(member, prefix, *type, out)) {
                return reason;
            }
            continue;
        }
        if (name.starts_with('$')) {
            continue;
        }
        if (auto reason = checkMemberName(prefix, name)) {
            return reason;
        }
        auto const* const object = std::get_if<Json::object_t>(&member.data);
        if (object == nullptr) {
            return joined({"`", prefix, name, "` is a token or a group, an object"});
        }
        std::string const path = prefix + name;
        Json const* const value = memberOf(*object, "$value");
        auto reason = value == nullptr ? readGroup(*object, path + ".", *type, out)
                                       : readToken(path, *object, *value, *type, out);
        if (reason) {
            return reason;
        }
        if (out.size() > kMaxThemeTokens) {
            return "a token file holds more than " + std::to_string(kMaxThemeTokens) + " tokens";
        }
    }
    return std::nullopt;
}

/// @brief Resolves tokens' aliases, depth first, each token once.
class AliasResolver {
public:
    /// @brief A resolver over @p merged.
    /// @param merged The layered tokens.
    explicit AliasResolver(TokenSet const& merged) : _merged{&merged} {}

    /// @brief Resolves every token.
    /// @return The resolved set, or why it is refused.
    [[nodiscard]] std::expected<TokenSet, std::string> all() {
        for (auto const& [path, token] : *_merged) {
            static_cast<void>(token);
            if (auto reason = resolve(path)) {
                return std::unexpected(std::move(*reason));
            }
        }
        return std::move(_done);
    }

private:
    /// Resolves one token into `_done`.
    // NOLINTNEXTLINE(misc-no-recursion): an alias resolves its target first, bounded by kMaxAliasChain.
    [[nodiscard]] std::optional<std::string> resolve(std::string const& path) {
        if (_done.contains(path)) {
            return std::nullopt;
        }
        if (std::ranges::find(_chain, path) != _chain.end()) {
            std::string cycle;
            for (std::string const& step : _chain) {
                cycle += step + " -> ";
            }
            return "an alias cycle: " + cycle + path;
        }
        if (_chain.size() >= kMaxAliasChain) {
            return "an alias chain longer than " + std::to_string(kMaxAliasChain) + " at `" + path + "`";
        }
        auto const found = _merged->find(path);
        if (found == _merged->end()) {
            // Only an alias names a path the set may lack, and the aliasing token is on the chain.
            return "`" + _chain.back() + "` refers to `" + path + "`, which no token file of the variant has";
        }
        _chain.push_back(path);
        Token token = found->second;
        if (auto const* const text = std::get_if<std::string>(&token.value.data)) {
            if (auto const target = aliasOf(*text)) {
                std::string const targetPath{*target};
                if (auto reason = resolve(targetPath)) {
                    return reason;
                }
                Token const& resolved = _done.at(targetPath);
                if (!token.type.empty() && token.type != resolved.type) {
                    return "the " + token.type + " token `" + path + "` aliases the " + resolved.type + " token `" +
                           targetPath + "`";
                }
                token = Token{.type = resolved.type, .value = resolved.value};
            }
        } else if (auto reason = replaceAliases(token.value)) {
            return reason;
        }
        if (auto reason = checkTokenValue(token.type, token.value)) {
            return "the token `" + path + "`: " + *reason;
        }
        _chain.pop_back();
        _done.insert_or_assign(path, std::move(token));
        return std::nullopt;
    }

    /// Replaces each alias inside a composite value with its target's resolved value.
    // NOLINTNEXTLINE(misc-no-recursion): a composite value is walked member by member, bounded by kMaxJsonDepth.
    [[nodiscard]] std::optional<std::string> replaceAliases(Json& value) {
        if (auto const* const text = std::get_if<std::string>(&value.data)) {
            if (auto const target = aliasOf(*text)) {
                std::string const targetPath{*target};
                if (auto reason = resolve(targetPath)) {
                    return reason;
                }
                value = _done.at(targetPath).value;
            }
            return std::nullopt;
        }
        if (auto* const array = std::get_if<Json::array_t>(&value.data)) {
            for (Json& item : *array) {
                if (auto reason = replaceAliases(item)) {
                    return reason;
                }
            }
        } else if (auto* const object = std::get_if<Json::object_t>(&value.data)) {
            for (auto& [name, member] : *object) {
                static_cast<void>(name);
                if (auto reason = replaceAliases(member)) {
                    return reason;
                }
            }
        }
        return std::nullopt;
    }

    TokenSet const* _merged;
    TokenSet _done;
    std::vector<std::string> _chain;
};

/// @brief Reads a map of name to file id from a theme index.
/// @param index The index's members.
/// @param key   `density` or `surfaces`.
/// @param out   Where the entries go.
/// @return Why the index is refused, or nothing.
[[nodiscard]] inline std::optional<std::string> readThemeMap(Json::object_t const& index, std::string_view key,
                                                             std::map<std::string, std::string, std::less<>>& out) {
    Json const* const member = memberOf(index, key);
    if (member == nullptr) {
        return std::nullopt;
    }
    auto const* const object = std::get_if<Json::object_t>(&member->data);
    if (object == nullptr) {
        return "`" + std::string{key} + "` is an object of name to token file";
    }
    for (auto const& [name, file] : *object) {
        auto const* const fileId = std::get_if<std::string>(&file.data);
        if (fileId == nullptr || fileId->empty()) {
            return "`" + std::string{key} + "." + name + "` names a token file";
        }
        out.insert_or_assign(name, *fileId);
    }
    return std::nullopt;
}

/// @brief Reads a theme index's `icons`: an object of icon name (`isIconName`) to icon file id, at most
///        `kMaxThemeIcons` names.
/// @param index The index.
/// @param out   Where the set is read into.
/// @return Why it is refused, or nothing; nothing for an index without `icons`.
[[nodiscard]] inline std::optional<std::string> readIcons(Json::object_t const& index,
                                                          std::map<std::string, std::string, std::less<>>& out) {
    Json const* const icons = memberOf(index, "icons");
    if (icons == nullptr) {
        return std::nullopt;
    }
    auto const* const set = std::get_if<Json::object_t>(&icons->data);
    if (set == nullptr) {
        return std::string{"`icons` is an object of icon name to icon file"};
    }
    if (set->size() > kMaxThemeIcons) {
        return "an icon set holds at most " + std::to_string(kMaxThemeIcons) + " names";
    }
    for (auto const& [name, file] : *set) {
        if (!isIconName(name)) {
            return "`" + name + "` is not an icon name: icon names match [a-z][a-z0-9.-]{0,63}";
        }
        auto const* const fileId = std::get_if<std::string>(&file.data);
        if (fileId == nullptr || fileId->empty()) {
            return "`icons." + name + "` names an icon file";
        }
        out.insert_or_assign(name, *fileId);
    }
    return std::nullopt;
}

}  // namespace detail

/// @brief Whether a theme file is a theme index rather than a token file: its top level has a string `vocab`.
/// @param text The file.
/// @return Whether it is an index; text that is not a JSON object is not.
[[nodiscard]] inline bool isThemeIndex(std::string_view text) {
    auto const json = parseJson(text);
    if (!json) {
        return false;
    }
    auto const* const object = std::get_if<Json::object_t>(&json->data);
    Json const* const vocab = object != nullptr ? detail::memberOf(*object, "vocab") : nullptr;
    return vocab != nullptr && std::holds_alternative<std::string>(vocab->data);
}

/// @brief Parses a DTCG 2025.10 token file.
///
/// Groups nest; a token is an object with `$value`, and its `$type` is its own or the nearest group's. Refused: text
/// that is not a JSON object or nests deeper than `kMaxJsonDepth`, a must-understand (`!`) name, a name with `.`, `{`
/// or `}`, a member that is not an object, a token with no type that is not a plain alias, a value that does not
/// have its checked type's shape, `$extends`, and more than `kMaxThemeTokens` tokens, and a token that holds members
/// of its own: write it as a group whose `$root` member is the token. A group's `$root` is the token at the group's
/// own path, and an alias may name it either as `{a.b}` or as `{a.b.$root}`. A token of a `$type` the client does not
/// know is kept unchecked with its type, which `unreadThemeTokens` names.
/// @param text The file.
/// @return The tokens by path, aliases unresolved, or why the file is refused.
[[nodiscard]] inline std::expected<TokenSet, std::string> parseTokens(std::string_view text) {
    auto const json = parseJson(text);
    if (!json) {
        return std::unexpected(json.error());
    }
    auto const* const object = std::get_if<Json::object_t>(&json->data);
    if (object == nullptr) {
        return std::unexpected(std::string{"a token file is an object of groups and tokens"});
    }
    TokenSet tokens;
    if (auto reason = detail::readGroup(*object, {}, {}, tokens)) {
        return std::unexpected(std::move(*reason));
    }
    return tokens;
}

/// @brief Layers token files, later ones winning per path, and resolves every alias.
///
/// Refused: an alias cycle, an alias to a token no layer has, a chain of more than `kMaxAliasChain` aliases, an
/// alias whose declared type differs from its target's, and a resolved value that does not have its type's shape.
/// @param layers The files, `base` first.
/// @return The resolved tokens, or why the combination is refused.
[[nodiscard]] inline std::expected<TokenSet, std::string> resolveTokens(std::vector<TokenSet const*> const& layers) {
    TokenSet merged;
    for (TokenSet const* const layer : layers) {
        for (auto const& [path, token] : *layer) {
            merged.insert_or_assign(path, token);
        }
    }
    return detail::AliasResolver{merged}.all();
}

/// @brief Parses a theme index: `{"vocab": "ui-theme/1", "base": id, "dark": id, …, "density": {...},
///        "surfaces": {...}, "icons": {...}}`.
///
/// Refused: not a JSON object, a `vocab` other than `ui-theme/1`, no `base`, a must-understand (`!`) key, a variant
/// that is not a file id, a `density` or `surfaces` that is not a map of name to file id, and more than
/// `kMaxThemeVariants` variants, densities or surfaces, and an `icons` that is not an object of icon name
/// (`isIconName`) to file id or holds more than `kMaxThemeIcons` names.
/// @param text The index.
/// @return The index, or why it is refused.
[[nodiscard]] inline std::expected<ThemeIndex, std::string> parseThemeIndex(std::string_view text) {
    auto const json = parseJson(text);
    if (!json) {
        return std::unexpected(json.error());
    }
    auto const* const object = std::get_if<Json::object_t>(&json->data);
    if (object == nullptr) {
        return std::unexpected(std::string{"a theme index is an object"});
    }
    if (auto reason = detail::mustUnderstand(*json, "the theme index")) {
        return std::unexpected(std::move(*reason));
    }
    Json const* const vocab = detail::memberOf(*object, "vocab");
    auto const* const vocabText = vocab != nullptr ? std::get_if<std::string>(&vocab->data) : nullptr;
    if (vocabText == nullptr || *vocabText != kThemeVocabulary) {
        return std::unexpected(std::string{"a theme index's vocab is ui-theme/1"});
    }
    ThemeIndex index;
    for (auto const& [key, member] : *object) {
        if (key == "vocab" || key == "density" || key == "surfaces" || key == "icons") {
            continue;
        }
        auto const* const fileId = std::get_if<std::string>(&member.data);
        if (fileId == nullptr || fileId->empty()) {
            return std::unexpected("the variant `" + key + "` names a token file");
        }
        if (key == "base") {
            index.base = *fileId;
        } else {
            index.variants.insert_or_assign(key, *fileId);
        }
    }
    if (index.base.empty()) {
        return std::unexpected(std::string{"a theme index names its `base` token file"});
    }
    if (auto reason = detail::readThemeMap(*object, "density", index.densities)) {
        return std::unexpected(std::move(*reason));
    }
    if (auto reason = detail::readThemeMap(*object, "surfaces", index.surfaces)) {
        return std::unexpected(std::move(*reason));
    }
    if (auto reason = detail::readIcons(*object, index.icons)) {
        return std::unexpected(std::move(*reason));
    }
    if (index.variants.size() > kMaxThemeVariants || index.densities.size() > kMaxThemeVariants ||
        index.surfaces.size() > kMaxThemeVariants) {
        return std::unexpected("a theme index lists more than " + std::to_string(kMaxThemeVariants) +
                               " variants, densities or surfaces");
    }
    return index;
}

/// @brief Finds a token file of the bundle by id.
using TokenFileLookup = std::function<TokenSet const*(std::string_view fileId)>;

/// @brief Resolves one combination of a theme: `base`, then the variant, the density and the surface, each optional.
/// @param index   The theme index.
/// @param files   The bundle's token files.
/// @param variant A variant name, or empty for `base` alone.
/// @param density A density name, or empty for none.
/// @param surface A surface name, or empty for none.
/// @return The resolved tokens, or why they are refused: a name the index does not list, a file the bundle lacks, or
///         `resolveTokens`' refusals.
// NOLINTBEGIN(bugprone-easily-swappable-parameters): variant, density, surface: the order they layer in.
[[nodiscard]] inline std::expected<TokenSet, std::string> resolveTheme(ThemeIndex const& index,
                                                                       TokenFileLookup const& files,
                                                                       std::string_view variant,
                                                                       std::string_view density,
                                                                       std::string_view surface) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    std::vector<TokenSet const*> layers;
    auto const layer = [&](std::string const& fileId) -> std::optional<std::string> {
        TokenSet const* const tokens = files(fileId);
        if (tokens == nullptr) {
            return "the token file `" + fileId + "` is not in the bundle";
        }
        layers.push_back(tokens);
        return std::nullopt;
    };
    auto const named = [&](std::map<std::string, std::string, std::less<>> const& from, std::string_view name,
                           std::string_view what) -> std::optional<std::string> {
        if (name.empty()) {
            return std::nullopt;
        }
        auto const found = from.find(name);
        if (found == from.end()) {
            return "the theme has no " + std::string{what} + " `" + std::string{name} + "`";
        }
        return layer(found->second);
    };
    std::optional<std::string> reason = layer(index.base);
    if (!reason) {
        reason = named(index.variants, variant, "variant");
    }
    if (!reason) {
        reason = named(index.densities, density, "density");
    }
    if (!reason) {
        reason = named(index.surfaces, surface, "surface");
    }
    if (reason) {
        return std::unexpected(std::move(*reason));
    }
    return resolveTokens(layers);
}

namespace detail {

/// @brief The paths of the set read as a stroke style.
inline constexpr std::array<std::string_view, 1> kStrokePaths = {"border.style"};

/// @brief A keyword token of the set: its path and the names it may take, default first.
struct KeywordSpec {
    /// @brief The dotted path.
    std::string_view path;
    /// @brief The names, default first.
    std::array<std::string_view, 2> names;
};

/// @brief The keyword tokens of the set.
inline constexpr std::array<KeywordSpec, 2> kKeywords = {{
    {"shape.iconOnly", {"default", "square"}},
    {"font.overline.case", {"none", "uppercase"}},
}};

/// @brief Whether @p name is one of the names of the keyword at @p path.
/// @param path The keyword's path.
/// @param name The name.
/// @return Whether the keyword takes it.
[[nodiscard]] inline bool isKeywordName(std::string_view path, std::string_view name) {
    return std::ranges::any_of(kKeywords, [path, name](KeywordSpec const& spec) {
        return spec.path == path && std::ranges::find(spec.names, name) != spec.names.end();
    });
}

/// @brief Why a resolved theme names a keyword that is not drawn.
/// @param resolved The resolved tokens.
/// @return The reason, such as `shape.iconOnly: only default and square are drawn`, or nothing when every keyword
///         the theme gives is one its spec lists.
[[nodiscard]] inline std::optional<std::string> keywordProblem(TokenSet const& resolved) {
    for (KeywordSpec const& spec : kKeywords) {
        auto const found = resolved.find(spec.path);
        if (found == resolved.end() || found->second.type != "keyword") {
            continue;
        }
        auto const* const named = std::get_if<std::string>(&found->second.value.data);
        if (named == nullptr || !isKeywordName(spec.path, *named)) {
            return std::string{spec.path} + ": only " + std::string{spec.names[0]} + " and " +
                   std::string{spec.names[1]} + " are drawn";
        }
    }
    return std::nullopt;
}

/// @brief Why a resolved theme strokes an outline in a style that is not drawn.
/// @param resolved The resolved tokens.
/// @return The reason, or nothing when every stroke style of the set is `solid` or `dashed`.
[[nodiscard]] inline std::optional<std::string> strokeProblem(TokenSet const& resolved) {
    for (std::string_view const path : kStrokePaths) {
        auto const found = resolved.find(path);
        if (found == resolved.end() || found->second.type != "strokeStyle") {
            continue;
        }
        auto const* const named = std::get_if<std::string>(&found->second.value.data);
        if (named == nullptr || (*named != "solid" && *named != "dashed")) {
            return std::string{path} + ": only solid and dashed are drawn";
        }
    }
    return std::nullopt;
}

}  // namespace detail

/// @brief Checks that every combination of a theme resolves: each variant (or none), density (or none) and surface
///        (or none) over `base`, and that none strokes an outline in a style that is not drawn.
/// @param index The theme index.
/// @param files The bundle's token files.
/// @return Why the theme is refused, naming the combination, or nothing.
[[nodiscard]] inline std::optional<std::string> checkTheme(ThemeIndex const& index, TokenFileLookup const& files) {
    auto const namesOf = [](std::map<std::string, std::string, std::less<>> const& from) {
        std::vector<std::string> names{std::string{}};
        for (auto const& [name, fileId] : from) {
            static_cast<void>(fileId);
            names.push_back(name);
        }
        return names;
    };
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): variant, density, surface: the order they layer in.
    auto const label = [](std::string const& variant, std::string const& density, std::string const& surface) {
        std::string out = "base";
        if (!variant.empty()) {
            out += " + " + variant;
        }
        if (!density.empty()) {
            out += " + density " + density;
        }
        if (!surface.empty()) {
            out += " + surface " + surface;
        }
        return out;
    };
    std::vector<std::string> const surfaces = namesOf(index.surfaces);
    for (std::string const& variant : namesOf(index.variants)) {
        for (std::string const& density : namesOf(index.densities)) {
            for (std::string const& surface : surfaces) {
                auto const resolved = resolveTheme(index, files, variant, density, surface);
                if (!resolved) {
                    return label(variant, density, surface) + ": " + resolved.error();
                }
                if (auto problem = detail::strokeProblem(*resolved)) {
                    return label(variant, density, surface) + ": " + *problem;
                }
                if (auto problem = detail::keywordProblem(*resolved)) {
                    return label(variant, density, surface) + ": " + *problem;
                }
            }
        }
    }
    return std::nullopt;
}

/// @brief What a `ui-theme/1` token is read as.
enum class ThemeTokenKind : std::uint8_t {
    Color,       ///< A `color` token.
    Number,      ///< A `number` token.
    Dimension,   ///< A `dimension` token, read in px (a `rem` is 16 px).
    Shadow,      ///< A `shadow` token: a colour and four px lengths; of a list of layers, the first.
    FontFamily,  ///< A `fontFamily` token: a family name, or a list of them in order of preference.
    FontWeight,  ///< A `fontWeight` token, read as its numeric weight, 1 to 1000, in `ThemeValues::numbers`.
    Stroke,      ///< A `strokeStyle` token: `solid` or `dashed`, in `ThemeValues::strokes`.
    Keyword,     ///< A `keyword` token: one of the names the spec lists, in `ThemeValues::keywords`.
};

/// @brief A `shadow` token's default, in the form a `constexpr` table can hold.
struct ThemeShadowSpec {
    /// @brief The colour, `#rrggbbaa`.
    std::string_view color;
    /// @brief The horizontal offset, in px.
    double offsetX = 0.0;
    /// @brief The vertical offset, in px.
    double offsetY = 0.0;
    /// @brief The blur radius, in px.
    double blur = 0.0;
    /// @brief The spread, in px.
    double spread = 0.0;
};

/// @brief One token of the closed `ui-theme/1` set: its path, kind and the `MorphUi` default.
struct ThemeTokenSpec {
    /// @brief The dotted path.
    std::string_view path;
    /// @brief How it is read.
    ThemeTokenKind kind;
    /// @brief A colour's default, `#rrggbb`; empty for a number.
    std::string_view color;
    /// @brief A number's default, or a dimension's in px.
    double number = 0.0;
    /// @brief A shadow's default.
    ThemeShadowSpec shadow{};
    /// @brief A font family's default; empty for none, which draws in the platform font.
    std::string_view family;
    /// @brief A stroke style's default: `solid` or `dashed`.
    std::string_view stroke;
    /// @brief A keyword's default, the first of its names in `detail::kKeywords`.
    std::string_view keyword;
};

/// @brief The closed `ui-theme/1` token set, with the `MorphUi` defaults: the colour roles, the opacity and the
///        spacing scale (`space.0` to `space.5`, one per spacing step) the built-in components read, and the semantic
///        sets (`ok`, `warn`, `err`, `info` with `bg`, `border` and `dot`).
inline constexpr std::array<ThemeTokenSpec, 67> kThemeTokens = {{
    {.path = "color.surface", .kind = ThemeTokenKind::Color, .color = "#ffffff", .number = 0.0},
    {.path = "color.text", .kind = ThemeTokenKind::Color, .color = "#1d1d1f", .number = 0.0},
    {.path = "color.muted", .kind = ThemeTokenKind::Color, .color = "#6e6e73", .number = 0.0},
    {.path = "color.accent", .kind = ThemeTokenKind::Color, .color = "#1565c0", .number = 0.0},
    {.path = "color.focus", .kind = ThemeTokenKind::Color, .color = "#1565c0", .number = 0.0},
    {.path = "color.border", .kind = ThemeTokenKind::Color, .color = "#c7c7cc", .number = 0.0},
    // The neutral tone: its text colour and its fill, outline and marker, as the other four tones have.
    {.path = "color.neutral", .kind = ThemeTokenKind::Color, .color = "#6e6e73", .number = 0.0},
    {.path = "color.neutral.bg", .kind = ThemeTokenKind::Color, .color = "#6e6e7326", .number = 0.0},
    {.path = "color.neutral.border", .kind = ThemeTokenKind::Color, .color = "#6e6e73", .number = 0.0},
    {.path = "color.neutral.dot", .kind = ThemeTokenKind::Color, .color = "#6e6e73", .number = 0.0},
    // The fill under a hovered or pressed item: translucent, so it darkens any light surface by one step and keeps
    // the text on it readable.
    {.path = "color.hover", .kind = ThemeTokenKind::Color, .color = "#1d1d1f14", .number = 0.0},
    // The unfilled part of a progress bar, and the segmented select's track.
    {.path = "color.track", .kind = ThemeTokenKind::Color, .color = "#e5e5ea", .number = 0.0},
    // A text input's, a select's, a date input's, a check box's and a normal button's fill and outline, apart from
    // the card's `color.surface` and `color.border`; and the rule between the rows of a table or a divided list.
    {.path = "color.controlSurface", .kind = ThemeTokenKind::Color, .color = "#ffffff", .number = 0.0},
    {.path = "color.controlBorder", .kind = ThemeTokenKind::Color, .color = "#c7c7cc", .number = 0.0},
    {.path = "color.divider", .kind = ThemeTokenKind::Color, .color = "#dcdce0", .number = 0.0},
    // The dim over the window behind a modal dialog or drawer.
    {.path = "color.scrim", .kind = ThemeTokenKind::Color, .color = "#0000004d", .number = 0.0},
    {.path = "color.selection", .kind = ThemeTokenKind::Color, .color = "#cfe3ff", .number = 0.0},
    {.path = "color.ok", .kind = ThemeTokenKind::Color, .color = "#2e7d32", .number = 0.0},
    {.path = "color.ok.bg", .kind = ThemeTokenKind::Color, .color = "#e8f5e9", .number = 0.0},
    {.path = "color.ok.border", .kind = ThemeTokenKind::Color, .color = "#a5d6a7", .number = 0.0},
    {.path = "color.ok.dot", .kind = ThemeTokenKind::Color, .color = "#2e7d32", .number = 0.0},
    {.path = "color.warn", .kind = ThemeTokenKind::Color, .color = "#b26a00", .number = 0.0},
    {.path = "color.warn.bg", .kind = ThemeTokenKind::Color, .color = "#fff4e5", .number = 0.0},
    {.path = "color.warn.border", .kind = ThemeTokenKind::Color, .color = "#ffcc80", .number = 0.0},
    {.path = "color.warn.dot", .kind = ThemeTokenKind::Color, .color = "#b26a00", .number = 0.0},
    {.path = "color.err", .kind = ThemeTokenKind::Color, .color = "#c62828", .number = 0.0},
    {.path = "color.err.bg", .kind = ThemeTokenKind::Color, .color = "#fdecea", .number = 0.0},
    {.path = "color.err.border", .kind = ThemeTokenKind::Color, .color = "#ef9a9a", .number = 0.0},
    {.path = "color.err.dot", .kind = ThemeTokenKind::Color, .color = "#c62828", .number = 0.0},
    {.path = "color.info", .kind = ThemeTokenKind::Color, .color = "#1565c0", .number = 0.0},
    {.path = "color.info.bg", .kind = ThemeTokenKind::Color, .color = "#e3f2fd", .number = 0.0},
    {.path = "color.info.border", .kind = ThemeTokenKind::Color, .color = "#90caf9", .number = 0.0},
    {.path = "color.info.dot", .kind = ThemeTokenKind::Color, .color = "#1565c0", .number = 0.0},
    {.path = "opacity.stale", .kind = ThemeTokenKind::Number, .color = "", .number = 0.55},
    {.path = "space.0", .kind = ThemeTokenKind::Dimension, .color = "", .number = 0.0},
    {.path = "space.1", .kind = ThemeTokenKind::Dimension, .color = "", .number = 4.0},
    {.path = "space.2", .kind = ThemeTokenKind::Dimension, .color = "", .number = 8.0},
    {.path = "space.3", .kind = ThemeTokenKind::Dimension, .color = "", .number = 12.0},
    {.path = "space.4", .kind = ThemeTokenKind::Dimension, .color = "", .number = 16.0},
    {.path = "space.5", .kind = ThemeTokenKind::Dimension, .color = "", .number = 24.0},
    {.path = "color.background", .kind = ThemeTokenKind::Color, .color = "#f2f3f7", .number = 0.0},
    {.path = "radius.control", .kind = ThemeTokenKind::Dimension, .color = "", .number = 6.0},
    {.path = "radius.card", .kind = ThemeTokenKind::Dimension, .color = "", .number = 10.0},
    // A badge's corners: a tag, a chip, an option of a select's list.
    {.path = "radius.badge", .kind = ThemeTokenKind::Dimension, .color = "", .number = 4.0},
    {.path = "control.height", .kind = ThemeTokenKind::Dimension, .color = "", .number = 32.0},
    // The least height of a control while the window is compact: the common minimum touch target. A sibling of
    // `control.height`: the set keeps one spelling per path.
    {.path = "control.touchHeight", .kind = ThemeTokenKind::Dimension, .color = "", .number = 44.0},
    {.path = "font.size.body", .kind = ThemeTokenKind::Dimension, .color = "", .number = 0.0},
    {.path = "font.size.heading", .kind = ThemeTokenKind::Dimension, .color = "", .number = 0.0},
    // The sizes below are derived from the body size while 0, as the two above are; none derives from another.
    // A tag's and a table header's size, a field's caption, the `mono` role's and the `display` role's.
    {.path = "font.size.small", .kind = ThemeTokenKind::Dimension, .color = "", .number = 0.0},
    {.path = "font.size.caption", .kind = ThemeTokenKind::Dimension, .color = "", .number = 0.0},
    {.path = "font.size.mono", .kind = ThemeTokenKind::Dimension, .color = "", .number = 0.0},
    {.path = "font.size.display", .kind = ThemeTokenKind::Dimension, .color = "", .number = 0.0},
    // The weight of every strong font: a section, a header, a heading, a strong text.
    {.path = "font.weight.strong", .kind = ThemeTokenKind::FontWeight, .color = "", .number = 700.0},
    // A table cell's and a header label's inset on each axis.
    {.path = "table.cellPaddingX", .kind = ThemeTokenKind::Dimension, .color = "", .number = 8.0},
    {.path = "table.cellPaddingY", .kind = ThemeTokenKind::Dimension, .color = "", .number = 4.0},
    {.path = "font.family", .kind = ThemeTokenKind::FontFamily, .color = "", .number = 0.0},
    // The fixed-pitch family of the `mono` text role; none draws in the platform's fixed-pitch font. A sibling of
    // `font.family`: the set keeps one spelling per path.
    {.path = "font.mono", .kind = ThemeTokenKind::FontFamily, .color = "", .number = 0.0},
    {.path = "elevation.card",
     .kind = ThemeTokenKind::Shadow,
     .color = "",
     .number = 0.0,
     .shadow = {.color = "#00000014", .offsetX = 0.0, .offsetY = 2.0, .blur = 10.0, .spread = 0.0}},
    // A popup's shadow: a menu, a dropdown, a date picker.
    {.path = "elevation.popup",
     .kind = ThemeTokenKind::Shadow,
     .color = "",
     .number = 0.0,
     .shadow = {.color = "#00000024", .offsetX = 0.0, .offsetY = 8.0, .blur = 24.0, .spread = 0.0}},
    // Transparent: an unset theme fills a table's header with nothing, matching today's appearance exactly.
    {.path = "color.table.header", .kind = ThemeTokenKind::Color, .color = "#00000000", .number = 0.0},
    // How the outline of a button, a badge and an input is stroked on a surface: solid, or dashed. Read per surface.
    {.path = "border.style",
     .kind = ThemeTokenKind::Stroke,
     .color = "",
     .number = 0.0,
     .shadow = {},
     .family = "",
     .stroke = "solid"},
    // A chart's series colours, in drawing order: Okabe and Ito's hues, darkened until each holds 3:1 against both
    // `color.surface` and `color.background`, the contrast a graphical object needs.
    {.path = "color.chart.1", .kind = ThemeTokenKind::Color, .color = "#0072b2", .number = 0.0},
    {.path = "color.chart.2", .kind = ThemeTokenKind::Color, .color = "#c25400", .number = 0.0},
    {.path = "color.chart.3", .kind = ThemeTokenKind::Color, .color = "#00805e", .number = 0.0},
    {.path = "color.chart.4", .kind = ThemeTokenKind::Color, .color = "#a8508a", .number = 0.0},
    // Two looks that change every application's shell or sidebar, off by default: an icon-only push button or menu
    // trigger drawn square, and the `overline` text role drawn in small capitals.
    {.path = "shape.iconOnly",
     .kind = ThemeTokenKind::Keyword,
     .color = "",
     .number = 0.0,
     .shadow = {},
     .family = "",
     .stroke = "",
     .keyword = "default"},
    {.path = "font.overline.case",
     .kind = ThemeTokenKind::Keyword,
     .color = "",
     .number = 0.0,
     .shadow = {},
     .family = "",
     .stroke = "",
     .keyword = "none"},
}};

/// @brief A `shadow` of the set, resolved.
struct ThemeShadow {
    /// @brief The colour.
    Rgba color{};
    /// @brief The horizontal offset, in px.
    double offsetX = 0.0;
    /// @brief The vertical offset, in px.
    double offsetY = 0.0;
    /// @brief The blur radius, in px.
    double blur = 0.0;
    /// @brief The spread, in px.
    double spread = 0.0;
};

/// @brief The `ui-theme/1` set's values for one resolved theme.
struct ThemeValues {
    /// @brief Every colour of the set, by path.
    std::map<std::string, Rgba, std::less<>> colors;
    /// @brief Every number of the set, by path.
    std::map<std::string, double, std::less<>> numbers;
    /// @brief Every dimension of the set, by path, in px.
    std::map<std::string, double, std::less<>> dimensions;
    /// @brief Every shadow of the set, by path.
    std::map<std::string, ThemeShadow, std::less<>> shadows;
    /// @brief Every font family list of the set, by path, in order of preference; empty for none.
    std::map<std::string, std::vector<std::string>, std::less<>> families;
    /// @brief Every stroke style of the set, by path: `solid` or `dashed`.
    std::map<std::string, std::string, std::less<>> strokes;
    /// @brief Every keyword of the set, by path: one of the names its spec lists.
    std::map<std::string, std::string, std::less<>> keywords;
};

namespace detail {

/// @brief A checked `shadow` value as a `ThemeShadow`; of a list of layers, the first.
/// @param json The value, checked by `checkShadow`.
/// @return It, or nothing when it is not a shadow.
[[nodiscard]] inline std::optional<ThemeShadow> shadowOf(Json const& json) {
    Json const* layer = &json;
    if (auto const* const layers = std::get_if<Json::array_t>(&json.data)) {
        if (layers->empty()) {
            return std::nullopt;
        }
        layer = &layers->front();
    }
    auto const* const object = std::get_if<Json::object_t>(&layer->data);
    if (object == nullptr) {
        return std::nullopt;
    }
    Json const* const color = memberOf(*object, "color");
    auto const rgba = color != nullptr ? colorOf(*color) : std::nullopt;
    auto const length = [object](std::string_view name) -> std::optional<double> {
        Json const* const member = memberOf(*object, name);
        return member != nullptr ? pixelsOf(*member) : std::nullopt;
    };
    auto const x = length("offsetX");
    auto const y = length("offsetY");
    auto const blur = length("blur");
    auto const spread = length("spread");
    if (!rgba || !x || !y || !blur || !spread) {
        return std::nullopt;
    }
    return ThemeShadow{.color = *rgba, .offsetX = *x, .offsetY = *y, .blur = *blur, .spread = *spread};
}

/// @brief A checked `fontFamily` value as its names, in order: one name, or a list of them.
/// @param json The value, checked by `checkFontFamily`.
/// @return The names; empty when it is neither.
[[nodiscard]] inline std::vector<std::string> familiesOf(Json const& json) {
    std::vector<std::string> names;
    if (auto const* const name = std::get_if<std::string>(&json.data)) {
        names.push_back(*name);
    } else if (auto const* const list = std::get_if<Json::array_t>(&json.data)) {
        for (Json const& entry : *list) {
            if (auto const* const listed = std::get_if<std::string>(&entry.data)) {
                names.push_back(*listed);
            }
        }
    }
    return names;
}

}  // namespace detail

namespace detail {

/// @brief A resolved theme's token at a path.
/// @param resolved The resolved tokens.
/// @param path     The token's path.
/// @return It, or null when the theme does not have it.
[[nodiscard]] inline Token const* tokenAt(TokenSet const& resolved, std::string_view path) {
    auto const found = resolved.find(path);
    return found == resolved.end() ? nullptr : &found->second;
}

/// @brief A `font.family` token's value: the theme's names when it has the token as a `fontFamily`, else the default.
/// @param spec  The token of the set.
/// @param found The theme's token at that path, or null.
/// @return The names, in order; empty for none.
[[nodiscard]] inline std::vector<std::string> familyValue(ThemeTokenSpec const& spec, Token const* found) {
    if (found != nullptr && found->type == "fontFamily") {
        return familiesOf(found->value);
    }
    std::vector<std::string> names;
    if (!spec.family.empty()) {
        names.emplace_back(spec.family);
    }
    return names;
}

}  // namespace detail

/// @brief Fills the closed `ui-theme/1` set from a resolved theme.
///
/// A path of the set takes the theme's value when the theme has it with the set's type, and its default otherwise; a
/// token outside the set is ignored.
/// @param resolved The resolved tokens; empty gives every default.
/// @return Every path of the set with its value.
[[nodiscard]] inline ThemeValues themeValues(TokenSet const& resolved = {}) {
    ThemeValues values;
    for (ThemeTokenSpec const& spec : kThemeTokens) {
        Token const* const token = detail::tokenAt(resolved, spec.path);
        if (spec.kind == ThemeTokenKind::Color) {
            std::optional<Rgba> color;
            if (token != nullptr && token->type == "color") {
                color = colorOf(token->value);
            }
            values.colors.insert_or_assign(std::string{spec.path},
                                           color.value_or(detail::hexColor(spec.color).value_or(Rgba{})));
        } else if (spec.kind == ThemeTokenKind::Shadow) {
            std::optional<ThemeShadow> shadow;
            if (token != nullptr && token->type == "shadow") {
                shadow = detail::shadowOf(token->value);
            }
            values.shadows.insert_or_assign(
                std::string{spec.path},
                shadow.value_or(ThemeShadow{.color = detail::hexColor(spec.shadow.color).value_or(Rgba{}),
                                            .offsetX = spec.shadow.offsetX,
                                            .offsetY = spec.shadow.offsetY,
                                            .blur = spec.shadow.blur,
                                            .spread = spec.shadow.spread}));
        } else if (spec.kind == ThemeTokenKind::FontFamily) {
            values.families.insert_or_assign(std::string{spec.path}, detail::familyValue(spec, token));
        } else if (spec.kind == ThemeTokenKind::Stroke) {
            std::string stroke{spec.stroke};
            if (token != nullptr && token->type == "strokeStyle") {
                if (auto const* const named = std::get_if<std::string>(&token->value.data);
                    named != nullptr && (*named == "solid" || *named == "dashed")) {
                    stroke = *named;
                }
            }
            values.strokes.insert_or_assign(std::string{spec.path}, std::move(stroke));
        } else if (spec.kind == ThemeTokenKind::Keyword) {
            std::string keyword{spec.keyword};
            if (token != nullptr && token->type == "keyword") {
                if (auto const* const named = std::get_if<std::string>(&token->value.data);
                    named != nullptr && detail::isKeywordName(spec.path, *named)) {
                    keyword = *named;
                }
            }
            values.keywords.insert_or_assign(std::string{spec.path}, std::move(keyword));
        } else if (spec.kind == ThemeTokenKind::FontWeight) {
            std::optional<double> weight;
            if (token != nullptr && token->type == "fontWeight") {
                weight = detail::weightOf(token->value);
            }
            values.numbers.insert_or_assign(std::string{spec.path}, weight.value_or(spec.number));
        } else if (spec.kind == ThemeTokenKind::Dimension) {
            std::optional<double> pixels;
            if (token != nullptr && token->type == "dimension") {
                pixels = detail::pixelsOf(token->value);
            }
            values.dimensions.insert_or_assign(std::string{spec.path}, pixels.value_or(spec.number));
        } else {
            std::optional<double> number;
            if (token != nullptr && token->type == "number") {
                number = detail::numberOf(token->value);
            }
            values.numbers.insert_or_assign(std::string{spec.path}, number.value_or(spec.number));
        }
    }
    return values;
}

namespace detail {

/// @brief The `$type`s the set's kinds are read as.
inline constexpr std::array<std::string_view, 8> kReadTokenTypes = {
    "color", "number", "dimension", "shadow", "fontFamily", "fontWeight", "strokeStyle", "keyword"};

/// @brief Calls @p reach with the path of every alias inside a value.
/// @param value The value.
/// @param reach The callback.
// NOLINTNEXTLINE(misc-no-recursion): a composite value is walked member by member, bounded by kMaxJsonDepth.
inline void forEachAlias(Json const& value, std::function<void(std::string_view)> const& reach) {
    if (auto const* const text = std::get_if<std::string>(&value.data)) {
        if (auto const target = aliasOf(*text)) {
            reach(*target);
        }
    } else if (auto const* const array = std::get_if<Json::array_t>(&value.data)) {
        for (Json const& item : *array) {
            forEachAlias(item, reach);
        }
    } else if (auto const* const object = std::get_if<Json::object_t>(&value.data)) {
        for (auto const& [name, member] : *object) {
            static_cast<void>(name);
            forEachAlias(member, reach);
        }
    }
}

}  // namespace detail

/// @brief A token of a theme's files that nothing reads.
struct UnreadThemeToken {
    /// @brief The id of the first file that has it.
    std::string file;
    /// @brief The token's path.
    std::string path;
    /// @brief Its `$type` when ui-theme/1 reads no token of that type; empty when it reads the type and the path is
    ///        merely not in the set.
    std::string unreadType;

    /// @brief What the report says about it.
    /// @return The sentence, starting with the file.
    [[nodiscard]] std::string message() const {
        if (unreadType.empty()) {
            return file + ": the token `" + path +
                   "` is not a ui-theme/1 token and no token of the set refers to it, so nothing draws it";
        }
        return file + ": the token `" + path + "` is of type " + unreadType + ", which ui-theme/1 does not read";
    }
};

/// @brief The tokens of a theme's files that nothing reads: neither a path of `kThemeTokens` nor reachable from one
///        through aliases, a plain alias or one inside a composite value, followed transitively.
///
/// Every file the index names is walked, and a path is followed through every file that has it, so a palette token
/// that any combination's set token refers to is read. A path is reported once, with the first file that has it.
/// @param index The theme index.
/// @param files The bundle's token files.
/// @return The unread tokens, in file order, then by path.
[[nodiscard]] inline std::vector<UnreadThemeToken> unreadThemeTokens(ThemeIndex const& index,
                                                                     TokenFileLookup const& files) {
    std::vector<std::string> ids{index.base};
    for (auto const* const named : {&index.variants, &index.densities, &index.surfaces}) {
        for (auto const& [name, fileId] : *named) {
            static_cast<void>(name);
            if (std::ranges::find(ids, fileId) == ids.end()) {
                ids.push_back(fileId);
            }
        }
    }
    std::map<std::string, std::vector<Token const*>, std::less<>> defined;
    for (std::string const& id : ids) {
        if (TokenSet const* const set = files(id)) {
            for (auto const& [path, token] : *set) {
                defined[path].push_back(&token);
            }
        }
    }
    std::set<std::string, std::less<>> read;
    std::vector<std::string> pending;
    auto const reach = [&read, &pending](std::string_view path) {
        if (read.emplace(path).second) {
            pending.emplace_back(path);
        }
    };
    for (ThemeTokenSpec const& spec : kThemeTokens) {
        reach(spec.path);
    }
    while (!pending.empty()) {
        std::string const path = std::move(pending.back());
        pending.pop_back();
        if (auto const found = defined.find(path); found != defined.end()) {
            for (Token const* const token : found->second) {
                detail::forEachAlias(token->value, reach);
            }
        }
    }
    std::vector<UnreadThemeToken> unread;
    std::set<std::string, std::less<>> reported;
    for (std::string const& id : ids) {
        TokenSet const* const set = files(id);
        if (set == nullptr) {
            continue;
        }
        for (auto const& [path, token] : *set) {
            if (read.contains(path) || !reported.insert(path).second) {
                continue;
            }
            bool const readType = token.type.empty() || std::ranges::find(detail::kReadTokenTypes, token.type) !=
                                                            detail::kReadTokenTypes.end();
            unread.push_back(
                UnreadThemeToken{.file = id, .path = path, .unreadType = readType ? std::string{} : token.type});
        }
    }
    return unread;
}

namespace detail {

/// @brief @p top drawn over the opaque colour @p under.
/// @param top   The colour, which may be translucent.
/// @param under The colour beneath it.
/// @return The opaque colour that results.
[[nodiscard]] inline Rgba over(Rgba top, Rgba under) {
    auto const mix = [&top](std::uint8_t above, std::uint8_t below) {
        return static_cast<std::uint8_t>(std::lround((above * top.a + below * (255.0 - top.a)) / 255.0));
    };
    return Rgba{.r = mix(top.r, under.r), .g = mix(top.g, under.g), .b = mix(top.b, under.b), .a = 255};
}

/// @brief The WCAG 2 relative luminance of an opaque colour.
/// @param color The colour.
/// @return It, from 0 to 1.
[[nodiscard]] inline double luminance(Rgba color) {
    auto const linear = [](std::uint8_t channel) {
        double const value = channel / 255.0;
        return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return (0.2126 * linear(color.r)) + (0.7152 * linear(color.g)) + (0.0722 * linear(color.b));
}

/// @brief The WCAG 2 contrast ratio of two opaque colours.
/// @param first  One colour.
/// @param second The other.
/// @return The ratio, 1 to 21.
[[nodiscard]] inline double contrastRatio(Rgba first, Rgba second) {
    double const high = std::max(luminance(first), luminance(second));
    double const low = std::min(luminance(first), luminance(second));
    return (high + 0.05) / (low + 0.05);
}

}  // namespace detail

/// @brief The fills text lies on, and the text colours that must stay readable on them.
///
/// `color.text` must hold 4.5:1 and `color.muted` 3:1 on `color.hover`, `color.selection`, `color.track` and
/// `color.controlSurface`. Muted gets 3:1 because the framework's own defaults hold that and not 4.5:1; the check
/// catches a fill that makes text disappear, not one that merely restyles it. A translucent fill is composited over
/// `color.surface` first.
/// @param values The theme's values.
/// @return One sentence per text colour and fill that fall short, such as `color.hover: color.text is 1.50:1 on it,
///         below 4.5:1`; empty when every pair holds.
[[nodiscard]] inline std::vector<std::string> themeContrastProblems(ThemeValues const& values) {
    struct TextRule {
        std::string_view path;
        double minimum;
        std::string_view label;
    };
    static constexpr std::array<TextRule, 2> kTexts = {{{"color.text", 4.5, "4.5"}, {"color.muted", 3.0, "3"}}};
    static constexpr std::array<std::string_view, 4> kFills = {"color.hover", "color.selection", "color.track",
                                                               "color.controlSurface"};
    std::vector<std::string> problems;
    auto const colorAt = [&values](std::string_view path) -> std::optional<Rgba> {
        auto const found = values.colors.find(path);
        return found == values.colors.end() ? std::nullopt : std::optional<Rgba>{found->second};
    };
    auto const surface = colorAt("color.surface").value_or(Rgba{});
    for (std::string_view const fillPath : kFills) {
        auto const fill = colorAt(fillPath);
        if (!fill) {
            continue;
        }
        Rgba const under = detail::over(*fill, detail::over(surface, Rgba{}));
        for (TextRule const& text : kTexts) {
            auto const color = colorAt(text.path);
            if (!color) {
                continue;
            }
            double const ratio = detail::contrastRatio(detail::over(*color, under), under);
            if (ratio < text.minimum) {
                problems.push_back(
                    std::format("{}: {} is {:.2f}:1 on it, below {}:1", fillPath, text.path, ratio, text.label));
            }
        }
    }
    return problems;
}

/// @brief What is wrong with a theme that does not refuse it: its unread tokens, and the text colours that cannot be
///        read on a fill (`themeContrastProblems`), for the base and for each surface.
/// @param index The theme index.
/// @param files The bundle's token files.
/// @return The sentences, each starting with the token file or the `base` or `surface <name>` it concerns; empty for
///         a theme that is all read and legible.
[[nodiscard]] inline std::vector<std::string> themeProblems(ThemeIndex const& index, TokenFileLookup const& files) {
    std::vector<std::string> problems;
    for (UnreadThemeToken const& token : unreadThemeTokens(index, files)) {
        problems.push_back(token.message());
    }
    auto const contrast = [&problems](std::string_view layer, std::expected<TokenSet, std::string> const& resolved) {
        if (!resolved) {
            return;
        }
        for (std::string const& problem : themeContrastProblems(themeValues(*resolved))) {
            problems.push_back(std::string{layer} + ": " + problem);
        }
    };
    contrast("base", resolveTheme(index, files, {}, {}, {}));
    for (auto const& [name, file] : index.surfaces) {
        static_cast<void>(file);
        contrast("surface " + name, resolveTheme(index, files, {}, {}, name));
    }
    return problems;
}

}  // namespace morph::ui
