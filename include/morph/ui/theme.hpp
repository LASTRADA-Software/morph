// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
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
            out += kDigits[static_cast<std::size_t>(channel >> 4U)];
            out += kDigits[static_cast<std::size_t>(channel & 0xFU)];
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

/// @brief Reads `#rgb`, `#rrggbb` or `#rrggbbaa`.
/// @param text The text.
/// @return The colour, or nothing.
[[nodiscard]] inline std::optional<Rgba> hexColor(std::string_view text) {
    if (!text.starts_with('#')) {
        return std::nullopt;
    }
    text.remove_prefix(1);
    auto const digit = [](char const c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    if (!std::ranges::all_of(text, [&digit](char const c) { return digit(c) >= 0; })) {
        return std::nullopt;
    }
    auto const pair = [&digit, text](std::size_t const at) {
        return static_cast<std::uint8_t>((digit(text[at]) * 16) + digit(text[at + 1]));
    };
    if (text.size() == 3) {
        auto const single = [&digit, text](std::size_t const at) {
            return static_cast<std::uint8_t>(digit(text[at]) * 17);
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

/// @brief Whether a string is an alias, `{a.b}`.
/// @param text The text.
/// @return The path it names, or nothing.
[[nodiscard]] inline std::optional<std::string_view> aliasOf(std::string_view text) {
    if (text.size() < 3 || !text.starts_with('{') || !text.ends_with('}')) {
        return std::nullopt;
    }
    return text.substr(1, text.size() - 2);
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
inline constexpr std::array<std::string_view, 13> kKnownTokenTypes = {
    "color",  "dimension",  "number", "fontFamily",  "fontWeight", "duration",  "cubicBezier",
    "shadow", "typography", "border", "strokeStyle", "gradient",   "transition"};

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
    return Rgba{.r = channels[0], .g = channels[1], .b = channels[2], .a = *opacity};
}

namespace detail {

inline std::optional<std::string> checkTokenValue(std::string_view type, Json const& value) {
    auto const* const object = std::get_if<Json::object_t>(&value.data);
    if (type == "color") {
        return colorOf(value) ? std::nullopt : std::optional<std::string>{"not a colour in sRGB or hex"};
    }
    if (type == "dimension" || type == "duration") {
        Json const* const number = object != nullptr ? memberOf(*object, "value") : nullptr;
        Json const* const unit = object != nullptr ? memberOf(*object, "unit") : nullptr;
        auto const* const unitText = unit != nullptr ? std::get_if<std::string>(&unit->data) : nullptr;
        bool const known = unitText != nullptr && (type == "dimension" ? (*unitText == "px" || *unitText == "rem")
                                                                       : (*unitText == "ms" || *unitText == "s"));
        if (number == nullptr || !numberOf(*number) || !known) {
            return type == "dimension"
                       ? std::string{"a dimension is an object of a number value and the unit px or rem"}
                       : std::string{"a duration is an object of a number value and the unit ms or s"};
        }
        return std::nullopt;
    }
    if (type == "number") {
        return numberOf(value) ? std::nullopt : std::optional<std::string>{"not a number"};
    }
    if (type == "fontFamily") {
        if (std::holds_alternative<std::string>(value.data)) {
            return std::nullopt;
        }
        auto const* const names = std::get_if<Json::array_t>(&value.data);
        bool const allNames = names != nullptr && !names->empty() && std::ranges::all_of(*names, [](Json const& name) {
                                  return std::holds_alternative<std::string>(name.data);
                              });
        return allNames ? std::nullopt : std::optional<std::string>{"a font family is a name or a list of names"};
    }
    if (type == "fontWeight") {
        if (auto const number = numberOf(value)) {
            return *number >= 1.0 && *number <= 1000.0 ? std::nullopt
                                                       : std::optional<std::string>{"a weight is 1 to 1000"};
        }
        auto const* const name = std::get_if<std::string>(&value.data);
        return name != nullptr && std::ranges::find(kFontWeights, *name) != kFontWeights.end()
                   ? std::nullopt
                   : std::optional<std::string>{"not a font weight"};
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
    std::string type = inherited;
    if (Json const* const declared = memberOf(group, "$type")) {
        auto const* const text = std::get_if<std::string>(&declared->data);
        if (text == nullptr) {
            return "`" + prefix + "$type` is a string";
        }
        type = *text;
    }
    if (memberOf(group, "$extends") != nullptr) {
        return "`" + prefix + "$extends`: group inheritance is not supported";
    }
    for (auto const& [name, member] : group) {
        if (name.starts_with('$')) {
            continue;
        }
        if (name.starts_with('!')) {
            return joined({"the must-understand key `", prefix, name, "` is not understood"});
        }
        if (name.find_first_of(".{}") != std::string::npos) {
            return joined({"the token or group name `", prefix, name, "` has `.`, `{` or `}`"});
        }
        auto const* const object = std::get_if<Json::object_t>(&member.data);
        if (object == nullptr) {
            return joined({"`", prefix, name, "` is a token or a group, an object"});
        }
        std::string const path = prefix + name;
        Json const* const value = memberOf(*object, "$value");
        if (value == nullptr) {
            if (auto reason = readGroup(*object, path + ".", type, out)) {
                return reason;
            }
            continue;
        }
        std::string tokenType = type;
        if (Json const* const declared = memberOf(*object, "$type")) {
            auto const* const text = std::get_if<std::string>(&declared->data);
            if (text == nullptr) {
                return joined({"`", path, ".$type` is a string"});
            }
            tokenType = *text;
        }
        bool const alias = holdsAlias(*value);
        if (tokenType.empty() && !(alias && std::holds_alternative<std::string>(value->data))) {
            return joined({"the token `", path, "` has no `$type`"});
        }
        if (!tokenType.empty() && std::ranges::find(kKnownTokenTypes, tokenType) == kKnownTokenTypes.end()) {
            continue;
        }
        if (!alias && !tokenType.empty()) {
            if (auto reason = checkTokenValue(tokenType, *value)) {
                return joined({"the token `", path, "`: ", *reason});
            }
        }
        if (out.size() >= kMaxThemeTokens) {
            return "a token file holds more than " + std::to_string(kMaxThemeTokens) + " tokens";
        }
        out.insert_or_assign(path, Token{.type = std::move(tokenType), .value = *value});
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
            return "`" + (_chain.empty() ? path : _chain.back()) + "` refers to `" + path +
                   "`, which no token file of the variant has";
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
        auto const* const id = std::get_if<std::string>(&file.data);
        if (id == nullptr || id->empty()) {
            return "`" + std::string{key} + "." + name + "` names a token file";
        }
        out.insert_or_assign(name, *id);
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
/// have its checked type's shape, `$extends`, and more than `kMaxThemeTokens` tokens. A token of a `$type` the client
/// does not know is dropped.
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
/// `kMaxThemeVariants` variants, densities or surfaces. `icons` is accepted and not read.
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
        auto const* const id = std::get_if<std::string>(&member.data);
        if (id == nullptr || id->empty()) {
            return std::unexpected("the variant `" + key + "` names a token file");
        }
        if (key == "base") {
            index.base = *id;
        } else {
            index.variants.insert_or_assign(key, *id);
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
    if (index.variants.size() > kMaxThemeVariants || index.densities.size() > kMaxThemeVariants ||
        index.surfaces.size() > kMaxThemeVariants) {
        return std::unexpected("a theme index lists more than " + std::to_string(kMaxThemeVariants) +
                               " variants, densities or surfaces");
    }
    return index;
}

/// @brief Finds a token file of the bundle by id.
using TokenFileLookup = std::function<TokenSet const*(std::string_view id)>;

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
    auto const layer = [&](std::string const& id) -> std::optional<std::string> {
        TokenSet const* const tokens = files(id);
        if (tokens == nullptr) {
            return "the token file `" + id + "` is not in the bundle";
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

/// @brief Checks that every combination of a theme resolves: each variant (or none), density (or none) and surface
///        (or none) over `base`.
/// @param index The theme index.
/// @param files The bundle's token files.
/// @return Why the theme is refused, naming the combination, or nothing.
[[nodiscard]] inline std::optional<std::string> checkTheme(ThemeIndex const& index, TokenFileLookup const& files) {
    auto const namesOf = [](std::map<std::string, std::string, std::less<>> const& from) {
        std::vector<std::string> names{std::string{}};
        for (auto const& [name, id] : from) {
            static_cast<void>(id);
            names.push_back(name);
        }
        return names;
    };
    for (std::string const& variant : namesOf(index.variants)) {
        for (std::string const& density : namesOf(index.densities)) {
            for (std::string const& surface : namesOf(index.surfaces)) {
                auto const resolved = resolveTheme(index, files, variant, density, surface);
                if (!resolved) {
                    return "base" + (variant.empty() ? std::string{} : " + " + variant) +
                           (density.empty() ? std::string{} : " + density " + density) +
                           (surface.empty() ? std::string{} : " + surface " + surface) + ": " + resolved.error();
                }
            }
        }
    }
    return std::nullopt;
}

/// @brief What a `ui-theme/1` token is read as.
enum class ThemeTokenKind : std::uint8_t {
    Color,   ///< A `color` token.
    Number,  ///< A `number` token.
};

/// @brief One token of the closed `ui-theme/1` set: its path, kind and the `MorphUi` default.
struct ThemeTokenSpec {
    /// @brief The dotted path.
    std::string_view path;
    /// @brief How it is read.
    ThemeTokenKind kind;
    /// @brief A colour's default, `#rrggbb`; empty for a number.
    std::string_view color;
    /// @brief A number's default.
    double number = 0.0;
};

/// @brief The closed `ui-theme/1` token set, with the `MorphUi` defaults: the colour roles and the opacity the
/// built-in
///        components read, and the semantic sets (`ok`, `warn`, `err`, `info` with `bg`, `border` and `dot`).
inline constexpr std::array<ThemeTokenSpec, 25> kThemeTokens = {{
    {.path = "color.surface", .kind = ThemeTokenKind::Color, .color = "#ffffff", .number = 0.0},
    {.path = "color.text", .kind = ThemeTokenKind::Color, .color = "#1d1d1f", .number = 0.0},
    {.path = "color.muted", .kind = ThemeTokenKind::Color, .color = "#6e6e73", .number = 0.0},
    {.path = "color.accent", .kind = ThemeTokenKind::Color, .color = "#1565c0", .number = 0.0},
    {.path = "color.focus", .kind = ThemeTokenKind::Color, .color = "#1565c0", .number = 0.0},
    {.path = "color.border", .kind = ThemeTokenKind::Color, .color = "#c7c7cc", .number = 0.0},
    {.path = "color.neutral", .kind = ThemeTokenKind::Color, .color = "#e5e5ea", .number = 0.0},
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
}};

/// @brief The `ui-theme/1` set's values for one resolved theme.
struct ThemeValues {
    /// @brief Every colour of the set, by path.
    std::map<std::string, Rgba, std::less<>> colors;
    /// @brief Every number of the set, by path.
    std::map<std::string, double, std::less<>> numbers;
};

/// @brief Fills the closed `ui-theme/1` set from a resolved theme.
///
/// A path of the set takes the theme's value when the theme has it with the set's type, and its default otherwise; a
/// token outside the set is ignored.
/// @param resolved The resolved tokens; empty gives every default.
/// @return Every path of the set with its value.
[[nodiscard]] inline ThemeValues themeValues(TokenSet const& resolved = {}) {
    ThemeValues values;
    for (ThemeTokenSpec const& spec : kThemeTokens) {
        auto const found = resolved.find(spec.path);
        if (spec.kind == ThemeTokenKind::Color) {
            std::optional<Rgba> color;
            if (found != resolved.end() && found->second.type == "color") {
                color = colorOf(found->second.value);
            }
            values.colors.insert_or_assign(std::string{spec.path},
                                           color.value_or(detail::hexColor(spec.color).value_or(Rgba{})));
        } else {
            std::optional<double> number;
            if (found != resolved.end() && found->second.type == "number") {
                number = detail::numberOf(found->second.value);
            }
            values.numbers.insert_or_assign(std::string{spec.path}, number.value_or(spec.number));
        }
    }
    return values;
}

}  // namespace morph::ui
