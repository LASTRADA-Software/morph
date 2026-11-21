// SPDX-License-Identifier: Apache-2.0

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
#include <morph/ui/bundle.hpp>
#include <morph/ui/control_shape.hpp>
#include <morph/ui/theme.hpp>
#include <morph/ui/value.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace morph::ui::detail {
namespace {

/// @brief A member of a JSON object.
/// @param object The object.
/// @param name   The member.
/// @return It, or null.
Json const* memberOf(Json::object_t const& object, std::string_view name) {
    auto const* const found = object.find(name);
    return found == object.end() ? nullptr : &found->second;
}

/// @brief Whether a value holds an alias anywhere.
/// @param json The value.
/// @return Whether it does.
// NOLINTNEXTLINE(misc-no-recursion): a composite value is walked member by member, bounded by kMaxJsonDepth.
bool holdsAlias(Json const& json) {
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

/// @brief Checks a `dimension` or a `duration`: an object of a number `value` and a known `unit`.
/// @param type  `dimension` or `duration`.
/// @param value The value.
/// @return Why it is not one, or nothing.
std::optional<std::string> checkMeasure(std::string_view type, Json const& value) {
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
std::optional<std::string> checkFontFamily(Json const& value) {
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
std::optional<std::string> checkFontWeight(Json const& value) {
    if (auto const number = numberOf(value)) {
        return *number >= 1.0 && *number <= 1000.0 ? std::nullopt
                                                   : std::optional<std::string>{"a weight is 1 to 1000"};
    }
    auto const* const name = std::get_if<std::string>(&value.data);
    return name != nullptr && std::ranges::find(kFontWeights, *name) != kFontWeights.end()
               ? std::nullopt
               : std::optional<std::string>{"not a font weight"};
}

std::optional<std::string> checkTokenValue(std::string_view type, Json const& value) {
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
std::string joined(std::initializer_list<std::string_view> parts) {
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
std::expected<std::string, std::string> declaredType(Json::object_t const& object, std::string_view where,
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
std::optional<std::string> readToken(std::string const& path, Json::object_t const& object, Json const& value,
                                     std::string const& type, TokenSet& out) {
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
std::optional<std::string> checkMemberName(std::string const& prefix, std::string const& name) {
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
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the group's path, then its inherited type; swapped, the `$root` token is read at the wrong path.
std::optional<std::string> readRoot(Json const& member, std::string const& prefix, std::string const& inherited,
                                    TokenSet& out) {
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

/// @brief Reads a map of name to file id from a theme index.
/// @param index The index's members.
/// @param key   `density` or `surfaces`.
/// @param out   Where the entries go.
/// @return Why the index is refused, or nothing.
std::optional<std::string> readThemeMap(Json::object_t const& index, std::string_view key,
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
std::optional<std::string> readIcons(Json::object_t const& index,
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

/// @brief Whether @p name is one of the names of the keyword at @p path.
/// @param path The keyword's path.
/// @param name The name.
/// @return Whether the keyword takes it.
bool isKeywordName(std::string_view path, std::string_view name) {
    return std::ranges::any_of(kKeywords, [path, name](KeywordSpec const& spec) {
        return spec.path == path && std::ranges::find(spec.names, name) != spec.names.end();
    });
}

/// @brief Names one combination of a theme's variant, density and surface.
/// @param variant The variant, or empty for none.
/// @param density The density, or empty for none.
/// @param surface The surface, or empty for none.
/// @return `base`, then each given layer.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): variant, density, surface: the order they layer in.
std::string combinationLabel(std::string const& variant, std::string const& density, std::string const& surface) {
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
}

/// @brief Why one combination of a theme is refused.
/// @param index   The theme index.
/// @param files   The bundle's token files.
/// @param variant The variant, or empty for none.
/// @param density The density, or empty for none.
/// @param surface The surface, or empty for none.
/// @return The reason, naming the combination, or nothing when it resolves and draws.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): variant, density, surface: the order they layer in.
std::optional<std::string> combinationProblem(ThemeIndex const& index, TokenFileLookup const& files,
                                              std::string const& variant, std::string const& density,
                                              std::string const& surface) {
    auto const resolved = resolveTheme(index, files, variant, density, surface);
    if (!resolved) {
        return combinationLabel(variant, density, surface) + ": " + resolved.error();
    }
    if (auto problem = strokeProblem(*resolved)) {
        return combinationLabel(variant, density, surface) + ": " + *problem;
    }
    if (auto problem = keywordProblem(*resolved)) {
        return combinationLabel(variant, density, surface) + ": " + *problem;
    }
    if (auto problem = dimensionProblem(*resolved)) {
        return combinationLabel(variant, density, surface) + ": " + *problem;
    }
    if (auto problem = opacityProblem(*resolved)) {
        return combinationLabel(variant, density, surface) + ": " + *problem;
    }
    return std::nullopt;
}

/// @brief A resolved theme's token at a path.
/// @param resolved The resolved tokens.
/// @param path     The token's path.
/// @return It, or null when the theme does not have it.
Token const* tokenAt(TokenSet const& resolved, std::string_view path) {
    auto const found = resolved.find(path);
    return found == resolved.end() ? nullptr : &found->second;
}

/// @brief A `font.family` token's value: the theme's names when it has the token as a `fontFamily`, else the default.
/// @param spec  The token of the set.
/// @param found The theme's token at that path, or null.
/// @return The names, in order; empty for none.
std::vector<std::string> familyValue(ThemeTokenSpec const& spec, Token const* found) {
    if (found != nullptr && found->type == "fontFamily") {
        return familiesOf(found->value);
    }
    std::vector<std::string> names;
    if (!spec.family.empty()) {
        names.emplace_back(spec.family);
    }
    return names;
}

/// @brief A name the token holds, when it is a string.
/// @param value The token's value.
/// @return The string, or nothing.
std::optional<std::string> tokenString(Json const& value) {
    if (auto const* const named = std::get_if<std::string>(&value.data)) {
        return *named;
    }
    return std::nullopt;
}

/// @brief Reads one token of the set into @p values: the theme's value when it has the set's type, else the default.
/// @param values Receives the value.
/// @param spec   The token of the set.
/// @param token  The theme's token at the spec's path, or `nullptr`.
void fillValue(ThemeValues& values, ThemeTokenSpec const& spec, Token const* token) {
    std::string const path{spec.path};
    switch (spec.kind) {
        case ThemeTokenKind::Color: {
            auto const color = typedValue(token, "color", [](Json const& value) { return colorOf(value); });
            // A colour with no default follows another colour and is left out until the theme sets it.
            if (!color && spec.color.empty()) {
                break;
            }
            values.colors.insert_or_assign(path, color.value_or(hexColor(spec.color).value_or(Rgba{})));
            break;
        }
        case ThemeTokenKind::Shadow: {
            auto const shadow = typedValue(token, "shadow", [](Json const& value) { return shadowOf(value); });
            // A shadow with no default follows another shadow and is left out until the theme sets it.
            if (!shadow && spec.shadow.color.empty()) {
                break;
            }
            values.shadows.insert_or_assign(
                path, shadow.value_or(ThemeShadow{.color = hexColor(spec.shadow.color).value_or(Rgba{}),
                                                  .offsetX = spec.shadow.offsetX,
                                                  .offsetY = spec.shadow.offsetY,
                                                  .blur = spec.shadow.blur,
                                                  .spread = spec.shadow.spread}));
            break;
        }
        case ThemeTokenKind::FontFamily:
            values.families.insert_or_assign(path, familyValue(spec, token));
            break;
        case ThemeTokenKind::Stroke: {
            auto const named = typedValue(token, "strokeStyle", [](Json const& value) { return tokenString(value); });
            std::string stroke{spec.stroke};
            if (named && (*named == "solid" || *named == "dashed")) {
                stroke = *named;
            }
            values.strokes.insert_or_assign(path, std::move(stroke));
            break;
        }
        case ThemeTokenKind::Keyword: {
            auto const named = typedValue(token, "keyword", [](Json const& value) { return tokenString(value); });
            std::string keyword{spec.keyword};
            if (named && isKeywordName(spec.path, *named)) {
                keyword = *named;
            }
            values.keywords.insert_or_assign(path, std::move(keyword));
            break;
        }
        case ThemeTokenKind::FontWeight: {
            auto const weight = typedValue(token, "fontWeight", [](Json const& value) { return weightOf(value); });
            values.numbers.insert_or_assign(path, weight.value_or(spec.number));
            break;
        }
        case ThemeTokenKind::Dimension: {
            auto const pixels = typedValue(token, "dimension", [](Json const& value) { return pixelsOf(value); });
            values.dimensions.insert_or_assign(path, pixels.value_or(spec.number));
            break;
        }
        case ThemeTokenKind::Number:
        default: {
            auto const number = typedValue(token, "number", [](Json const& value) { return numberOf(value); });
            values.numbers.insert_or_assign(path, number.value_or(spec.number));
            break;
        }
    }
}

/// @brief Calls @p reach with the path of every alias inside a value.
/// @param value The value.
/// @param reach The callback.
// NOLINTNEXTLINE(misc-no-recursion): a composite value is walked member by member, bounded by kMaxJsonDepth.
void forEachAlias(Json const& value, std::function<void(std::string_view)> const& reach) {
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

/// @brief The ids of the files a theme index names, the base first, each once.
/// @param index The theme index.
/// @return The ids.
std::vector<std::string> themeFileIds(ThemeIndex const& index) {
    std::vector<std::string> fileIds{index.base};
    for (auto const* const named : {&index.variants, &index.densities, &index.surfaces}) {
        for (auto const& [name, fileId] : *named) {
            static_cast<void>(name);
            if (std::ranges::find(fileIds, fileId) == fileIds.end()) {
                fileIds.push_back(fileId);
            }
        }
    }
    return fileIds;
}

/// @brief The paths a theme reads: those of the set, and everything they reach through aliases in any file.
/// @param fileIds The ids of the theme's files.
/// @param files   The bundle's token files.
/// @return The paths.
std::set<std::string, std::less<>> readTokenPaths(std::vector<std::string> const& fileIds,
                                                  TokenFileLookup const& files) {
    std::map<std::string, std::vector<Token const*>, std::less<>> defined;
    for (std::string const& fileId : fileIds) {
        if (TokenSet const* const set = files(fileId)) {
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
                forEachAlias(token->value, reach);
            }
        }
    }
    return read;
}

/// @brief @p top drawn over the opaque colour @p under.
/// @param top   The colour, which may be translucent.
/// @param under The colour beneath it.
/// @return The opaque colour that results.
Rgba over(Rgba top, Rgba under) {
    auto const mix = [&top](std::uint8_t above, std::uint8_t below) {
        return static_cast<std::uint8_t>(std::lround(((above * top.a) + (below * (255.0 - top.a))) / 255.0));
    };
    return Rgba{.r = mix(top.r, under.r), .g = mix(top.g, under.g), .b = mix(top.b, under.b), .a = 255};
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

}  // namespace
}  // namespace morph::ui::detail

namespace morph::ui {

bool isIconName(std::string_view name) {
    if (name.empty() || name.size() > 64 || name.front() < 'a' || name.front() > 'z') {
        return false;
    }
    return std::ranges::all_of(name, [](char const chr) {
        return (chr >= 'a' && chr <= 'z') || (chr >= '0' && chr <= '9') || chr == '.' || chr == '-';
    });
}

std::string_view iconGlyph(std::string_view name) {
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 14> kGlyphs = {{
        {"tone.ok", "✓"},
        {"tone.warn", "!"},
        {"tone.err", "✗"},
        {"tone.info", "i"},
        {"nav.expand", "▸"},
        {"nav.collapse", "▾"},
        {"select.open", "▾"},
        {"select.close", "▴"},
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

std::string Rgba::hex() const {
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
namespace detail {

std::optional<double> numberOf(Json const& json) {
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

std::optional<double> pixelsOf(Json const& json) {
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

std::optional<Rgba> hexColor(std::string_view text) {
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

std::optional<std::uint8_t> channelOf(double const value) {
    if (value < 0.0 || value > 1.0) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(std::lround(value * 255.0));
}

std::optional<std::string_view> aliasOf(std::string_view text) {
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
}  // namespace detail

std::optional<Rgba> colorOf(Json const& json) {
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

std::optional<std::string> checkShadow(Json const& value) {
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

std::optional<double> weightOf(Json const& value) {
    if (auto const number = numberOf(value)) {
        return number;
    }
    auto const* const name = std::get_if<std::string>(&value.data);
    if (name == nullptr) {
        return std::nullopt;
    }
    if (auto const weight = weightNamed(*name)) {
        return static_cast<double>(*weight);
    }
    return std::nullopt;
}

// NOLINTBEGIN(misc-no-recursion,bugprone-easily-swappable-parameters): groups nest, bounded by kMaxJsonDepth; the
// group's path, then the type it passes down.
std::optional<std::string> readGroup(Json::object_t const& group, std::string const& prefix,
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
}  // namespace detail

bool isThemeIndex(std::string_view text) {
    auto const json = parseJson(text);
    if (!json) {
        return false;
    }
    auto const* const object = std::get_if<Json::object_t>(&json->data);
    Json const* const vocab = object != nullptr ? detail::memberOf(*object, "vocab") : nullptr;
    return vocab != nullptr && std::holds_alternative<std::string>(vocab->data);
}

std::expected<TokenSet, std::string> parseTokens(std::string_view text) {
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

std::expected<TokenSet, std::string> resolveTokens(std::vector<TokenSet const*> const& layers) {
    TokenSet merged;
    for (TokenSet const* const layer : layers) {
        for (auto const& [path, token] : *layer) {
            merged.insert_or_assign(path, token);
        }
    }
    return detail::AliasResolver{merged}.all();
}

std::expected<ThemeIndex, std::string> parseThemeIndex(std::string_view text) {
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

// NOLINTBEGIN(bugprone-easily-swappable-parameters): variant, density, surface: the order they layer in.
std::expected<TokenSet, std::string> resolveTheme(ThemeIndex const& index, TokenFileLookup const& files,
                                                  std::string_view variant, std::string_view density,
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

std::optional<std::string> keywordProblem(TokenSet const& resolved) {
    for (KeywordSpec const& spec : kKeywords) {
        auto const found = resolved.find(spec.path);
        if (found == resolved.end() || found->second.type != "keyword") {
            continue;
        }
        auto const* const named = std::get_if<std::string>(&found->second.value.data);
        if (named == nullptr || !isKeywordName(spec.path, *named)) {
            return std::string{spec.path} + ": only " + std::string{spec.names.front()} + " and " +
                   std::string{spec.names.back()} + " are drawn";
        }
    }
    return std::nullopt;
}

std::optional<std::string> dimensionProblem(TokenSet const& resolved) {
    for (std::string_view const path : kNonNegativePaths) {
        auto const found = resolved.find(path);
        if (found == resolved.end() || found->second.type != "dimension") {
            continue;
        }
        auto const pixels = pixelsOf(found->second.value);
        if (pixels && *pixels < 0.0) {
            return std::string{path} + ": must not be negative";
        }
    }
    return std::nullopt;
}

std::optional<std::string> opacityProblem(TokenSet const& resolved) {
    for (std::string_view const path : kOpacityPaths) {
        auto const found = resolved.find(path);
        if (found == resolved.end() || found->second.type != "number") {
            continue;
        }
        auto const number = numberOf(found->second.value);
        if (number && (*number < 0.0 || *number > 1.0)) {
            return std::string{path} + ": must be between 0 and 1";
        }
    }
    return std::nullopt;
}

std::optional<std::string> strokeProblem(TokenSet const& resolved) {
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

std::optional<std::string> checkTheme(ThemeIndex const& index, TokenFileLookup const& files) {
    auto const namesOf = [](std::map<std::string, std::string, std::less<>> const& from) {
        std::vector<std::string> names{std::string{}};
        for (auto const& [name, fileId] : from) {
            static_cast<void>(fileId);
            names.push_back(name);
        }
        return names;
    };
    std::vector<std::string> const surfaces = namesOf(index.surfaces);
    for (std::string const& variant : namesOf(index.variants)) {
        for (std::string const& density : namesOf(index.densities)) {
            for (std::string const& surface : surfaces) {
                if (auto problem = detail::combinationProblem(index, files, variant, density, surface)) {
                    return problem;
                }
            }
        }
    }
    return std::nullopt;
}
namespace detail {

std::optional<ThemeShadow> shadowOf(Json const& json) {
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

std::vector<std::string> familiesOf(Json const& json) {
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

ThemeValues themeValues(TokenSet const& resolved) {
    ThemeValues values;
    for (ThemeTokenSpec const& spec : kThemeTokens) {
        detail::fillValue(values, spec, detail::tokenAt(resolved, spec.path));
    }
    return values;
}

std::string UnreadThemeToken::message() const {
    if (unreadType.empty()) {
        return file + ": the token `" + path +
               "` is not a ui-theme/1 token and no token of the set refers to it, so nothing draws it";
    }
    return file + ": the token `" + path + "` is of type " + unreadType + ", which ui-theme/1 does not read";
}

std::vector<UnreadThemeToken> unreadThemeTokens(ThemeIndex const& index, TokenFileLookup const& files) {
    std::vector<std::string> const fileIds = detail::themeFileIds(index);
    std::set<std::string, std::less<>> const read = detail::readTokenPaths(fileIds, files);
    std::vector<UnreadThemeToken> unread;
    std::set<std::string, std::less<>> reported;
    for (std::string const& fileId : fileIds) {
        TokenSet const* const set = files(fileId);
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
                UnreadThemeToken{.file = fileId, .path = path, .unreadType = readType ? std::string{} : token.type});
        }
    }
    return unread;
}
namespace detail {

double luminance(Rgba color) {
    auto const linear = [](std::uint8_t channel) {
        double const value = channel / 255.0;
        return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return (0.2126 * linear(color.r)) + (0.7152 * linear(color.g)) + (0.0722 * linear(color.b));
}

double contrastRatio(Rgba first, Rgba second) {
    double const high = std::max(luminance(first), luminance(second));
    double const low = std::min(luminance(first), luminance(second));
    return (high + 0.05) / (low + 0.05);
}
}  // namespace detail

std::vector<std::string> themeContrastProblems(ThemeValues const& values) {
    struct TextRule {
        std::string_view path;
        double minimum;
        std::string_view label;
    };
    static constexpr std::array<TextRule, 2> kTexts = {{{.path = "color.text", .minimum = 4.5, .label = "4.5"},
                                                        {.path = "color.muted", .minimum = 3.0, .label = "3"}}};
    static constexpr std::array<std::string_view, 4> kFills = {"color.hover", "color.selection", "color.track",
                                                               "color.controlSurface"};
    std::vector<std::string> problems;
    auto const colorAt = [&values](std::string_view path) -> std::optional<Rgba> {
        auto const found = values.colors.find(path);
        return found == values.colors.end() ? std::nullopt : std::optional<Rgba>{found->second};
    };
    auto const surface = colorAt("color.surface").value_or(Rgba{});
    auto const check = [&](std::string_view fillPath, TextRule const& text) {
        auto const fill = colorAt(fillPath);
        auto const color = colorAt(text.path);
        if (!fill || !color) {
            return;
        }
        Rgba const under = detail::over(*fill, detail::over(surface, Rgba{}));
        double const ratio = detail::contrastRatio(detail::over(*color, under), under);
        if (ratio < text.minimum) {
            problems.push_back(
                std::format("{}: {} is {:.2f}:1 on it, below {}:1", fillPath, text.path, ratio, text.label));
        }
    };
    for (std::string_view const fillPath : kFills) {
        for (TextRule const& text : kTexts) {
            check(fillPath, text);
        }
    }
    check("color.selection", {.path = "color.selectionText", .minimum = 4.5, .label = "4.5"});
    return problems;
}

std::vector<std::string> themeProblems(ThemeIndex const& index, TokenFileLookup const& files) {
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
