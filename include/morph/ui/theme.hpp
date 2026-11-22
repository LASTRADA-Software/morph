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
#include <morph/export.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

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
///        a chevron that opens and closes, a select's drop-down indicator, a close button, a date input's calendar,
///        an overflow menu's trigger, the compact shell's navigation menu, and the chat panel's Dictate button. A
///        bundle's icon set replaces any of them by name.
inline constexpr std::array<std::string_view, 16> kFrameworkIcons = {
    "empty",       "tone.info",  "tone.ok",      "tone.warn",   "tone.err",     "nav.back",
    "nav.forward", "nav.expand", "nav.collapse", "select.open", "select.close", "close",
    "date.pick",   "more",       "nav.menu",     "chat.dictate"};

/// @brief Whether @p name is an icon name: `[a-z][a-z0-9.-]{0,63}`.
/// @param name The name.
/// @return It.
[[nodiscard]] MORPH_API bool isIconName(std::string_view name);

/// @brief The glyph a text renderer draws for a framework icon name.
/// @param name The icon name.
/// @return The glyph, or empty for a name that has none (`empty`, which a text renderer draws as nothing).
[[nodiscard]] MORPH_API std::string_view iconGlyph(std::string_view name);

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
struct MORPH_API Rgba {
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
    [[nodiscard]] std::string hex() const;
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
[[nodiscard]] MORPH_API std::optional<double> numberOf(Json const& json);

/// @brief How many px a `rem` is: the default root font size, which the renderers do not change.
inline constexpr double kPixelsPerRem = 16.0;

/// @brief A DTCG `dimension` value in px.
/// @param json The value: an object of a number `value` and the unit `px` or `rem`.
/// @return Its length in px, or nothing when it is not a dimension.
[[nodiscard]] MORPH_API std::optional<double> pixelsOf(Json const& json);

/// @brief Reads `#rgb`, `#rrggbb` or `#rrggbbaa`.
/// @param text The text.
/// @return The colour, or nothing.
[[nodiscard]] MORPH_API std::optional<Rgba> hexColor(std::string_view text);

/// @brief A channel from 0 to 1, as 8 bits.
/// @param value The channel.
/// @return It, or nothing outside 0 to 1.
[[nodiscard]] MORPH_API std::optional<std::uint8_t> channelOf(double value);

/// @brief The member name that holds a group's own token.
inline constexpr std::string_view kRootMember = "$root";

/// @brief Whether a string is an alias, `{a.b}`.
/// @param text The text.
/// @return The path it names, or nothing. A trailing `.$root` is dropped: `{color.ok.$root}` names `color.ok`, the
///         group's own token.
[[nodiscard]] MORPH_API std::optional<std::string_view> aliasOf(std::string_view text);

/// @brief The types the client checks; a token of another `$type` the client still knows is accepted unchecked.
inline constexpr std::array<std::string_view, 14> kKnownTokenTypes = {
    "color",  "dimension",  "number", "fontFamily",  "fontWeight", "duration",   "cubicBezier",
    "shadow", "typography", "border", "strokeStyle", "gradient",   "transition", "keyword"};

/// @brief The named font weights of DTCG.
inline constexpr std::array<std::string_view, 18> kFontWeights = {
    "thin",       "hairline",   "extra-light", "ultra-light", "light",       "normal",
    "regular",    "book",       "medium",      "semi-bold",   "demi-bold",   "bold",
    "extra-bold", "ultra-bold", "black",       "heavy",       "extra-black", "ultra-black"};

}  // namespace detail

/// @brief Reads a colour value: `#rgb`, `#rrggbb`, `#rrggbbaa`, or the DTCG 2025.10 object, by its `hex` or by its
///        `srgb` `components` (0 to 1) and `alpha`.
/// @param json The value.
/// @return The colour, or nothing when it is none of those.
[[nodiscard]] MORPH_API std::optional<Rgba> colorOf(Json const& json);

namespace detail {

/// @brief The numeric weight of a checked `fontWeight` value.
/// @param value The value, checked by `checkFontWeight`: a number from 1 to 1000, or a DTCG weight name.
/// @return The weight, or nothing when it is neither.
[[nodiscard]] MORPH_API std::optional<double> weightOf(Json const& value);

/// @brief Checks a `shadow`: an object of a `color` and the dimensions `offsetX`, `offsetY`, `blur` and `spread`, or a
///        non-empty list of them.
/// @param value The value.
/// @return Why it is not one, or nothing.
[[nodiscard]] MORPH_API std::optional<std::string> checkShadow(Json const& value);

/// @brief Reads the tokens of one group into @p out.
/// @param group     The group's members.
/// @param prefix    The group's path, with a trailing `.`; empty at the top.
/// @param inherited The nearest enclosing `$type`.
/// @param out       Where the tokens go.
/// @return Why the file is refused, or nothing.
// NOLINTBEGIN(misc-no-recursion,bugprone-easily-swappable-parameters): groups nest, bounded by kMaxJsonDepth; the
// group's path, then the type it passes down.
[[nodiscard]] MORPH_API std::optional<std::string> readGroup(Json::object_t const& group, std::string const& prefix,
                                                             std::string const& inherited, TokenSet& out);
// NOLINTEND(misc-no-recursion,bugprone-easily-swappable-parameters)

}  // namespace detail

/// @brief Whether a theme file is a theme index rather than a token file: its top level has a string `vocab`.
/// @param text The file.
/// @return Whether it is an index; text that is not a JSON object is not.
[[nodiscard]] MORPH_API bool isThemeIndex(std::string_view text);

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
[[nodiscard]] MORPH_API std::expected<TokenSet, std::string> parseTokens(std::string_view text);

/// @brief Layers token files, later ones winning per path, and resolves every alias.
///
/// Refused: an alias cycle, an alias to a token no layer has, a chain of more than `kMaxAliasChain` aliases, an
/// alias whose declared type differs from its target's, and a resolved value that does not have its type's shape.
/// @param layers The files, `base` first.
/// @return The resolved tokens, or why the combination is refused.
[[nodiscard]] MORPH_API std::expected<TokenSet, std::string> resolveTokens(std::vector<TokenSet const*> const& layers);

/// @brief Parses a theme index: `{"vocab": "ui-theme/1", "base": id, "dark": id, …, "density": {...},
///        "surfaces": {...}, "icons": {...}}`.
///
/// Refused: not a JSON object, a `vocab` other than `ui-theme/1`, no `base`, a must-understand (`!`) key, a variant
/// that is not a file id, a `density` or `surfaces` that is not a map of name to file id, and more than
/// `kMaxThemeVariants` variants, densities or surfaces, and an `icons` that is not an object of icon name
/// (`isIconName`) to file id or holds more than `kMaxThemeIcons` names.
/// @param text The index.
/// @return The index, or why it is refused.
[[nodiscard]] MORPH_API std::expected<ThemeIndex, std::string> parseThemeIndex(std::string_view text);

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
[[nodiscard]] MORPH_API std::expected<TokenSet, std::string> resolveTheme(ThemeIndex const& index,
                                                                          TokenFileLookup const& files,
                                                                          std::string_view variant,
                                                                          std::string_view density,
                                                                          std::string_view surface);
// NOLINTEND(bugprone-easily-swappable-parameters)

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
inline constexpr std::array<KeywordSpec, 5> kKeywords = {{
    {.path = "shape.iconOnly", .names = {"default", "square"}},
    {.path = "font.overline.case", .names = {"none", "uppercase"}},
    {.path = "field.caption.case", .names = {"none", "uppercase"}},
    {.path = "collapsible.marker", .names = {"shown", "none"}},
    {.path = "list.selection", .names = {"accent", "fill"}},
}};

/// @brief The dimension tokens of the set that may not be negative: an inset has no meaning below zero.
inline constexpr std::array<std::string_view, 8> kNonNegativePaths = {
    "input.paddingX",     "head.paddingY",           "actions.gap",   "control.touchTextSize",
    "button.smallHeight", "button.smallTouchHeight", "tabs.paddingX", "tabs.gap"};

/// @brief Why a resolved theme names a keyword that is not drawn.
/// @param resolved The resolved tokens.
/// @return The reason, such as `shape.iconOnly: only default and square are drawn`, or nothing when every keyword
///         the theme gives is one its spec lists.
[[nodiscard]] MORPH_API std::optional<std::string> keywordProblem(TokenSet const& resolved);

/// @brief Why a resolved theme gives a dimension a negative length where the set takes none.
/// @param resolved The resolved tokens.
/// @return The reason, such as `input.paddingX: must not be negative`, or nothing when no such dimension is negative.
[[nodiscard]] MORPH_API std::optional<std::string> dimensionProblem(TokenSet const& resolved);

/// @brief The number tokens of the set that are opacities: below 0 or above 1 draws nothing a theme could mean.
inline constexpr std::array<std::string_view, 2> kOpacityPaths = {"opacity.stale", "opacity.muted"};

/// @brief Why a resolved theme gives an opacity outside 0 to 1.
/// @param resolved The resolved tokens.
/// @return The reason, such as `opacity.muted: must be between 0 and 1`, or nothing when every opacity of the set
///         is in range.
[[nodiscard]] MORPH_API std::optional<std::string> opacityProblem(TokenSet const& resolved);

/// @brief Why a resolved theme strokes an outline in a style that is not drawn.
/// @param resolved The resolved tokens.
/// @return The reason, or nothing when every stroke style of the set is `solid` or `dashed`.
[[nodiscard]] MORPH_API std::optional<std::string> strokeProblem(TokenSet const& resolved);

}  // namespace detail

namespace detail {}  // namespace detail

/// @brief Checks that every combination of a theme resolves: each variant (or none), density (or none) and surface
///        (or none) over `base`, and that none strokes an outline in a style that is not drawn.
/// @param index The theme index.
/// @param files The bundle's token files.
/// @return Why the theme is refused, naming the combination, or nothing.
[[nodiscard]] MORPH_API std::optional<std::string> checkTheme(ThemeIndex const& index, TokenFileLookup const& files);

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
inline constexpr auto kThemeTokens = std::to_array<ThemeTokenSpec>({
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
    // The rules inside a dialog and a drawer: under the head, above the footer. Empty: while a theme does not set it,
    // they draw `color.divider` of the card surface, and the token is absent from `ThemeValues`.
    {.path = "color.dialogRule", .kind = ThemeTokenKind::Color, .color = "", .number = 0.0},
    // The dim over the window behind a modal dialog or drawer.
    {.path = "color.scrim", .kind = ThemeTokenKind::Color, .color = "#0000004d", .number = 0.0},
    // A disabled button: its caption, its fill and its outline. Global, read by every surface: a disabled control
    // draws the same grey whatever it sits on.
    {.path = "color.disabled.text", .kind = ThemeTokenKind::Color, .color = "#8e8e93", .number = 0.0},
    {.path = "color.disabled.bg", .kind = ThemeTokenKind::Color, .color = "#f2f2f7", .number = 0.0},
    {.path = "color.disabled.border", .kind = ThemeTokenKind::Color, .color = "#e5e5ea", .number = 0.0},
    {.path = "color.selection", .kind = ThemeTokenKind::Color, .color = "#cfe3ff", .number = 0.0},
    // The text of a selected list item, read per surface. Empty: while a theme does not set it, a selected item's
    // text is the surface's `color.text`, which is the first colour to follow another; the token is absent from
    // `ThemeValues` until set.
    {.path = "color.selectionText", .kind = ThemeTokenKind::Color, .color = "", .number = 0.0},
    // The text an empty input shows, read per surface. Empty: while a theme does not set it, a placeholder draws the
    // surface's `color.muted`, and the token is absent from `ThemeValues`. The terminal always draws it dim.
    {.path = "color.placeholder", .kind = ThemeTokenKind::Color, .color = "", .number = 0.0},
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
    // The size of a control's own text while the window is compact: a text input's, a text area's, a date input's, a
    // select's and a push button's. 0 keeps the body size.
    {.path = "control.touchTextSize", .kind = ThemeTokenKind::Dimension, .color = "", .number = 0.0},
    // The height of a `size: small` button, which may be above or below the control height; 0 keeps the control
    // height. Its touch counterpart applies while the window is compact; 0 keeps the touch target.
    {.path = "button.smallHeight", .kind = ThemeTokenKind::Dimension, .color = "", .number = 0.0},
    {.path = "button.smallTouchHeight", .kind = ThemeTokenKind::Dimension, .color = "", .number = 0.0},
    {.path = "font.size.body", .kind = ThemeTokenKind::Dimension, .color = "", .number = 0.0},
    {.path = "font.size.heading", .kind = ThemeTokenKind::Dimension, .color = "", .number = 0.0},
    // The sizes below are derived from the body size while 0, as the two above are; none derives from another.
    // A tag's and a table header's size, a field's caption, the `mono` role's, the `display` role's and the `headline`
    // role's.
    {.path = "font.size.small", .kind = ThemeTokenKind::Dimension, .color = "", .number = 0.0},
    {.path = "font.size.caption", .kind = ThemeTokenKind::Dimension, .color = "", .number = 0.0},
    {.path = "font.size.mono", .kind = ThemeTokenKind::Dimension, .color = "", .number = 0.0},
    {.path = "font.size.display", .kind = ThemeTokenKind::Dimension, .color = "", .number = 0.0},
    {.path = "font.size.headline", .kind = ThemeTokenKind::Dimension, .color = "", .number = 0.0},
    // The weight of every strong font: a section, a header, a heading, a strong text.
    {.path = "font.weight.strong", .kind = ThemeTokenKind::FontWeight, .color = "", .number = 700.0},
    // The weight of a button's caption where it is drawn strong: a filled button, a small one, a square one.
    {.path = "font.weight.button", .kind = ThemeTokenKind::FontWeight, .color = "", .number = 700.0},
    // A table cell's and a header label's inset on each axis.
    {.path = "table.cellPaddingX", .kind = ThemeTokenKind::Dimension, .color = "", .number = 8.0},
    {.path = "table.cellPaddingY", .kind = ThemeTokenKind::Dimension, .color = "", .number = 4.0},
    // The inner inset of every input on each side: a text input's, a text area's, a date input's and a select's value.
    {.path = "input.paddingX", .kind = ThemeTokenKind::Dimension, .color = "", .number = 8.0},
    // The space above and below the line of the shell's expanded head bar; 0 keeps the bar's own gap, a spacing step.
    {.path = "head.paddingY", .kind = ThemeTokenKind::Dimension, .color = "", .number = 0.0},
    // The space between the actions of a row of actions (an overlay's footer, an `actionBar`); 0 keeps spacing step 2.
    {.path = "actions.gap", .kind = ThemeTokenKind::Dimension, .color = "", .number = 0.0},
    // A tab's inset on each side of its label, and the space between two tabs; 0 keeps spacing step 3 and no space.
    {.path = "tabs.paddingX", .kind = ThemeTokenKind::Dimension, .color = "", .number = 0.0},
    {.path = "tabs.gap", .kind = ThemeTokenKind::Dimension, .color = "", .number = 0.0},
    {.path = "font.family", .kind = ThemeTokenKind::FontFamily, .color = "", .number = 0.0},
    // The fixed-pitch family of the `mono` text role; none draws in the platform's fixed-pitch font. A sibling of
    // `font.family`: the set keeps one spelling per path.
    {.path = "font.mono", .kind = ThemeTokenKind::FontFamily, .color = "", .number = 0.0},
    {.path = "elevation.card",
     .kind = ThemeTokenKind::Shadow,
     .color = "",
     .number = 0.0,
     .shadow = {.color = "#00000014", .offsetX = 0.0, .offsetY = 2.0, .blur = 10.0, .spread = 0.0}},
    // The shadow under a dialog and a drawer. Empty: while a theme does not set it, a dialog and a drawer draw
    // `elevation.card`'s value, and the token is absent from `ThemeValues`.
    {.path = "elevation.dialog",
     .kind = ThemeTokenKind::Shadow,
     .color = "",
     .number = 0.0,
     .shadow = {.color = "", .offsetX = 0.0, .offsetY = 0.0, .blur = 0.0, .spread = 0.0}},
    // A popup's shadow: a menu, a dropdown, a date picker.
    {.path = "elevation.popup",
     .kind = ThemeTokenKind::Shadow,
     .color = "",
     .number = 0.0,
     .shadow = {.color = "#00000024", .offsetX = 0.0, .offsetY = 8.0, .blur = 24.0, .spread = 0.0}},
    // Transparent: an unset theme fills a table's header with nothing, matching today's appearance exactly.
    {.path = "color.table.header", .kind = ThemeTokenKind::Color, .color = "#00000000", .number = 0.0},
    // The 1 px rule under a table's header, in the header's last pixel row. Transparent: a theme that draws a header
    // band says how it closes.
    {.path = "color.table.headerRule", .kind = ThemeTokenKind::Color, .color = "#00000000", .number = 0.0},
    // A table row an action of this session is working on: the bar at its leading edge, after the selection's.
    {.path = "color.table.busy", .kind = ThemeTokenKind::Color, .color = "#1565c0", .number = 0.0},
    // A table row another user holds: the wash over its cells, translucent so the cells read through it.
    {.path = "color.table.locked", .kind = ThemeTokenKind::Color, .color = "#8e8e9338", .number = 0.0},
    // A table's summary row: the surface it is drawn on, translucent so the row's own background reads through it.
    {.path = "color.table.summary", .kind = ThemeTokenKind::Color, .color = "#8e8e931f", .number = 0.0},
    // The opacity of a table row kept in view although it no longer matches the filter, from 0 to 1.
    {.path = "opacity.muted", .kind = ThemeTokenKind::Number, .color = "", .number = 0.55},
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
    // Three looks that change every application's shell, sidebar or sections, off by default: an icon-only push button
    // or menu trigger drawn square, the `overline` text role drawn in small capitals, and a collapsible section
    // drawn without its chevron.
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
    // How a field's caption draws: as written in the caption font (`none`), or in small letter-spaced capitals like
    // an uppercase overline (`uppercase`).
    {.path = "field.caption.case",
     .kind = ThemeTokenKind::Keyword,
     .color = "",
     .number = 0.0,
     .shadow = {},
     .family = "",
     .stroke = "",
     .keyword = "none"},
    {.path = "collapsible.marker",
     .kind = ThemeTokenKind::Keyword,
     .color = "",
     .number = 0.0,
     .shadow = {},
     .family = "",
     .stroke = "",
     .keyword = "shown"},
    // How a selected list item is marked, read per surface: the selection colour with a 3 px accent bar at the
    // leading edge (`accent`), or the selection colour alone (`fill`). The item's text is `color.selectionText`
    // either way.
    {.path = "list.selection",
     .kind = ThemeTokenKind::Keyword,
     .color = "",
     .number = 0.0,
     .shadow = {},
     .family = "",
     .stroke = "",
     .keyword = "accent"},
});

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
[[nodiscard]] MORPH_API std::optional<ThemeShadow> shadowOf(Json const& json);

/// @brief A checked `fontFamily` value as its names, in order: one name, or a list of them.
/// @param json The value, checked by `checkFontFamily`.
/// @return The names; empty when it is neither.
[[nodiscard]] MORPH_API std::vector<std::string> familiesOf(Json const& json);

}  // namespace detail

namespace detail {}  // namespace detail

namespace detail {

/// @brief What @p read makes of a token's value, when the token is there and has the given type.
/// @tparam Read Callable taking the token's value.
/// @param token The token, or `nullptr` for none.
/// @param type  The `$type` the token must have.
/// @param read  Reads the value.
/// @return What @p read returns, or its empty value when the token is absent or of another type.
template <typename Read>
[[nodiscard]] auto typedValue(Token const* token, std::string_view type, Read read) -> decltype(read(token->value)) {
    if (token == nullptr || token->type != type) {
        return {};
    }
    return read(token->value);
}

}  // namespace detail

/// @brief Fills the closed `ui-theme/1` set from a resolved theme.
///
/// A path of the set takes the theme's value when the theme has it with the set's type, and its default otherwise; a
/// token outside the set is ignored.
/// @param resolved The resolved tokens; empty gives every default.
/// @return Every path of the set with its value.
[[nodiscard]] MORPH_API ThemeValues themeValues(TokenSet const& resolved = {});

namespace detail {

/// @brief The `$type`s the set's kinds are read as.
inline constexpr std::array<std::string_view, 8> kReadTokenTypes = {
    "color", "number", "dimension", "shadow", "fontFamily", "fontWeight", "strokeStyle", "keyword"};

}  // namespace detail

namespace detail {}  // namespace detail

/// @brief A token of a theme's files that nothing reads.
struct MORPH_API UnreadThemeToken {
    /// @brief The id of the first file that has it.
    std::string file;
    /// @brief The token's path.
    std::string path;
    /// @brief Its `$type` when ui-theme/1 reads no token of that type; empty when it reads the type and the path is
    ///        merely not in the set.
    std::string unreadType;

    /// @brief What the report says about it.
    /// @return The sentence, starting with the file.
    [[nodiscard]] std::string message() const;
};

/// @brief The tokens of a theme's files that nothing reads: neither a path of `kThemeTokens` nor reachable from one
///        through aliases, a plain alias or one inside a composite value, followed transitively.
///
/// Every file the index names is walked, and a path is followed through every file that has it, so a palette token
/// that any combination's set token refers to is read. A path is reported once, with the first file that has it.
/// @param index The theme index.
/// @param files The bundle's token files.
/// @return The unread tokens, in file order, then by path.
[[nodiscard]] MORPH_API std::vector<UnreadThemeToken> unreadThemeTokens(ThemeIndex const& index,
                                                                        TokenFileLookup const& files);

namespace detail {

/// @brief The WCAG 2 relative luminance of an opaque colour.
/// @param color The colour.
/// @return It, from 0 to 1.
[[nodiscard]] MORPH_API double luminance(Rgba color);

/// @brief The WCAG 2 contrast ratio of two opaque colours.
/// @param first  One colour.
/// @param second The other.
/// @return The ratio, 1 to 21.
[[nodiscard]] MORPH_API double contrastRatio(Rgba first, Rgba second);

}  // namespace detail

/// @brief The fills text lies on, and the text colours that must stay readable on them.
///
/// `color.text` must hold 4.5:1 and `color.muted` 3:1 on `color.hover`, `color.selection`, `color.track` and
/// `color.controlSurface`. A theme that sets `color.selectionText` must hold it 4.5:1 on `color.selection`. Muted gets
/// 3:1 because the framework's own defaults hold that and not 4.5:1; the check catches a fill that makes text
/// disappear, not one that merely restyles it. A translucent fill is composited over `color.surface` first.
/// @param values The theme's values.
/// @return One sentence per text colour and fill that fall short, such as `color.hover: color.text is 1.50:1 on it,
///         below 4.5:1`; empty when every pair holds.
[[nodiscard]] MORPH_API std::vector<std::string> themeContrastProblems(ThemeValues const& values);

/// @brief What is wrong with a theme that does not refuse it: its unread tokens, and the text colours that cannot be
///        read on a fill (`themeContrastProblems`), for the base and for each surface.
/// @param index The theme index.
/// @param files The bundle's token files.
/// @return The sentences, each starting with the token file or the `base` or `surface <name>` it concerns; empty for
///         a theme that is all read and legible.
[[nodiscard]] MORPH_API std::vector<std::string> themeProblems(ThemeIndex const& index, TokenFileLookup const& files);

}  // namespace morph::ui
