// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file render/locale_format.hpp
/// @brief Locale numeric-entry normalisation for the control edge.
///
/// A locale may render and accept e.g. `"1.050,25"`; the payload a
/// `morph::units::Quantity` field submits is always the canonical exact
/// `{num, den, dp}` regardless. This header is the one control-edge
/// conversion step between the two: the exact `Rational`/`Quantity` digit
/// routines stay entirely locale-free (they only ever see plain
/// `.`-decimal text), and a renderer calls `normalizeLocaleNumber` once, at
/// the point text leaves the control, before handing it to those routines.
///
/// @par Separators are strings, not characters
/// Both functions take their separators as `std::string_view`, because a
/// real locale's separator is not always one byte. fr-FR groups with U+202F
/// (narrow no-break space) and several locales use U+00A0 — three and two
/// UTF-8 bytes respectively. Typed as `char`, those cannot be expressed at
/// all: the caller can only pass some single byte that never matches, so a
/// perfectly valid `"1 050,25"` typed by a French user normalises to
/// `std::nullopt` and the entry is reported malformed. An empty view means
/// "this locale has no such separator" (the role `'\0'` used to play).
///
/// @par So is the negative sign
/// For the same reason, and measured rather than assumed: of the 711 locales
/// Qt 6.11.2 knows, 77 report a `negativeSign` that is not a bare ASCII `'-'`.
/// 23 use U+2212 MINUS SIGN (e.g. eu_ES); 54 more prefix the sign with a bidi
/// control -- U+061C ARABIC LETTER MARK, U+200E LEFT-TO-RIGHT MARK or U+200F
/// RIGHT-TO-LEFT MARK -- making it two or three code points, and ar_DZ does so
/// even though its sign is the ordinary hyphen. Matched as a single `char`,
/// none of those round-trips: the display edge emitted a sign the entry edge
/// then rejected. So `negativeSign` is a `std::string_view` matched as a whole
/// string too, defaulting to `"-"` so that every existing caller is unchanged
/// (morph#583).

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace morph::render {

namespace detail {

/// @brief Whether every group separator in @p text sits where a group
///        separator can legally sit.
///
/// The rule, in one place so it can be read on its own: a group separator is
/// preceded by one to three digits (the first group), or by exactly three
/// (every later one); it is followed by exactly three more digits, at each
/// group and at the end of the integer part; and it never appears after the
/// decimal separator. A locale with no grouping (@p groupSeparator empty) has
/// nothing to place, so it trivially passes.
///
/// This is a pass of its own rather than extra state inside the normalising
/// scan below: "the grouping is well placed" and "the digits convert" are two
/// separate statements about the entry, and reading them as one made neither
/// clear.
///
/// Characters this function does not recognise are simply not digits — the
/// normalising scan is what rejects them, and it rejects them whatever this
/// pass concludes.
/// @param text             The locale-formatted entry.
/// @param decimalSeparator The locale's decimal-point string; may be empty.
/// @param groupSeparator   The locale's digit-grouping string; empty means the
///                         locale does not group.
/// @return `true` when the grouping is well placed (or absent).
[[nodiscard]] inline bool groupingIsWellPlaced(std::string_view text, std::string_view decimalSeparator,
                                               std::string_view groupSeparator) {
    if (groupSeparator.empty()) {
        return true;
    }
    constexpr std::size_t kGroupSize = 3;
    std::size_t digits = 0;
    bool sawGroup = false;
    bool sawDecimal = false;

    for (std::size_t i = 0; i < text.size();) {
        const std::string_view rest = text.substr(i);
        if (rest.starts_with(groupSeparator)) {
            bool const opensAGroup = sawGroup ? digits == kGroupSize : (digits >= 1 && digits <= kGroupSize);
            if (sawDecimal || !opensAGroup) {
                return false;
            }
            sawGroup = true;
            digits = 0;
            i += groupSeparator.size();
            continue;
        }
        if (!decimalSeparator.empty() && rest.starts_with(decimalSeparator)) {
            if (sawGroup && digits != kGroupSize) {
                return false;  // the last group of the integer part is short
            }
            sawDecimal = true;
            digits = 0;
            i += decimalSeparator.size();
            continue;
        }
        // i is bounded by the loop condition.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        char const chr = text[i];
        digits = (chr >= '0' && chr <= '9') ? digits + 1 : 0;
        ++i;
    }

    // An ungrouped fractional part ends the number, so the trailing check only
    // applies when the integer part was the last thing scanned.
    return !sawGroup || sawDecimal || digits == kGroupSize;
}

/// @brief The length of the negative sign at the start of @p rest, in bytes, or
///        `0` when there is none there.
///
/// Two spellings count. @p negativeSign is the locale's own, matched as a whole
/// string so that U+2212 and the bidi-control-prefixed forms -- two and three
/// code points -- match at all; a single `char` could express none of them
/// (morph#583). A bare ASCII `'-'` counts as well, in every locale: U+2212 and
/// the bidi marks are on no keyboard, so matching only the locale's spelling
/// would reject the sign the user can actually type. The hyphen has no second
/// reading in a numeric entry, so this is not the kind of guess the grouping
/// rule forbids.
///
/// This is a function of its own so that the caller's scan reads as one
/// statement per character class; whichever spelling matched, the caller emits
/// the canonical `'-'`.
/// @param rest         The remainder of the entry, starting at the scan position.
/// @param negativeSign The locale's negative-sign string; empty matches nothing,
///                     leaving only the ASCII spelling.
/// @return The number of bytes the sign occupies, or `0`.
[[nodiscard]] inline std::size_t leadingSignLength(std::string_view rest, std::string_view negativeSign) {
    if (!negativeSign.empty() && rest.starts_with(negativeSign)) {
        return negativeSign.size();
    }
    return rest.starts_with('-') ? 1U : 0U;
}

}  // namespace detail

/// @brief Converts a locale-formatted numeric string to canonical
///        (`-?[0-9]+(\.[0-9]+)?`) text.
///
/// Drops @p groupSeparator where it is correctly placed, and replaces every
/// occurrence of @p decimalSeparator with `.`. Passing `decimalSeparator ==
/// "."` and an empty @p groupSeparator is the identity transform (the
/// locale-free behavior). Malformed input (a second decimal separator, a
/// sign anywhere but the leading position of the *output*, or any character that
/// is not a digit) yields `std::nullopt` rather than a best-effort guess. The
/// decimal point counts as output, so a sign placed straight after the separator
/// ("`,-5`" in a de-DE locale) is rejected -- matching the QML mirror in
/// `src/qt/forms/qml/DynamicForm.qml`, which has always rejected it (morph#497).
///
/// @par Grouping is validated, not stripped (morph#574)
/// A group separator is only dropped where a group separator can legally be:
/// preceded by one to three digits, followed by exactly three more, and never
/// after the decimal separator. Anything else is malformed and reported as
/// such. Stripping unconditionally instead is a wrong *value*, not a rejected
/// one: a de-DE user typing the US form `"1.5"` into a price field submitted
/// `15`, and nothing downstream could tell -- the result is a perfectly valid
/// number, ten times too large. `"1.50"` gave `150`, `"1.2.3.4"` gave `1234`,
/// and the en-US mirror image `"1,5"` gave `15`.
///
/// @par The two separators must differ
/// When @p groupSeparator is non-empty and equal to @p decimalSeparator the
/// entry is rejected: with one string in both roles there is no reading of
/// `"1.5"` the function could defend. This is a caller (locale-configuration)
/// error rather than a user one, but it is reported through the return value
/// like any other malformed entry, deliberately not through an assertion --
/// an assertion would make the two build configurations behave differently at
/// a control edge, and would be untestable in the one where it fires.
///
/// The result is `.`-decimal and digit-only, but is **not** narrowed to
/// `-?[0-9]+(\.[0-9]+)?`: a bare "`.`", a leading "`.5`" and a trailing "`5.`"
/// are passed through, exactly as that same QML mirror passes them. Tightening
/// one side alone would put the two control edges back out of step, so the shape
/// is documented here rather than changed.
///
/// Separators are matched as whole strings, so a multi-byte one (e.g. U+202F)
/// works; matching them before the per-byte digit scan is what keeps their
/// continuation bytes from being mistaken for stray non-digit characters.
///
/// @par The negative sign is matched as a whole string too (morph#583)
/// @p negativeSign is matched the same way, which is what lets a locale whose
/// sign is U+2212, or is prefixed by a bidi control mark, be entered at all --
/// 77 of the 711 locales Qt 6.11.2 knows. Before this the sign was the literal
/// byte `'-'`, so `formatCanonicalNumber` emitted a sign this function then
/// rejected, and the pair was not inverse for those locales.
///
/// @par ASCII `'-'` stays accepted whatever the locale
/// A bare `'-'` is accepted in the leading position in addition to
/// @p negativeSign. U+2212 and the bidi marks are on no keyboard, so matching
/// only the locale's own spelling would reject the sign the user can actually
/// type and leave them no way to enter a negative number at all. The hyphen has
/// no second reading in a numeric entry, so accepting it is not the kind of
/// guess morph#574 forbids -- that was about producing a wrong *value*, and
/// this produces the only value the input can mean.
///
/// @par An empty @p negativeSign means the ASCII default, not "no sign"
/// Unlike a group separator, there is no locale without a negative sign, so an
/// empty view leaves the ASCII `'-'` above as the only spelling rather than
/// meaning "this entry cannot be negative". `formatCanonicalNumber` reads it
/// the same way, and the display edge is why it must: a sign that formatted to
/// nothing would turn `-5` into `5` silently -- a wrong value, not a rejected
/// one.
///
/// @param text             The locale-formatted entry, e.g. `"1.050,25"`.
/// @param decimalSeparator The locale's decimal-point string, e.g. `","`.
/// @param groupSeparator   The locale's digit-grouping string, e.g. `"."`, or
///                         empty when the locale has none.
/// @param negativeSign     The locale's negative-sign string, e.g. `"\u2212"`;
///                         empty is read as the default `"-"`.
/// @return The canonical `.`-decimal text, or `std::nullopt` when malformed.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
// The three locale strings are one fixed order, mirrored by
// formatCanonicalNumber so the two inverses read alike; separating the sign
// from the separators to break the adjacency would put them out of step.
[[nodiscard]] inline std::optional<std::string> normalizeLocaleNumber(std::string_view text,
                                                                      std::string_view decimalSeparator,
                                                                      std::string_view groupSeparator,
                                                                      std::string_view negativeSign = "-") {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    if (!groupSeparator.empty() && groupSeparator == decimalSeparator) {
        return std::nullopt;  // one string cannot play both roles: see above
    }
    if (!detail::groupingIsWellPlaced(text, decimalSeparator, groupSeparator)) {
        return std::nullopt;  // a separator off a group boundary: see above
    }

    std::string canonical;
    canonical.reserve(text.size());
    bool sawDecimal = false;
    bool sawAnyOutput = false;

    for (std::size_t i = 0; i < text.size();) {
        const std::string_view rest = text.substr(i);
        if (!groupSeparator.empty() && rest.starts_with(groupSeparator)) {
            // Placement was settled above, so by here the separator is display
            // only and is never carried into the output.
            i += groupSeparator.size();
            continue;
        }
        if (!decimalSeparator.empty() && rest.starts_with(decimalSeparator)) {
            if (sawDecimal) {
                return std::nullopt;  // a second decimal separator: malformed
            }
            sawDecimal = true;
            canonical += '.';
            // The decimal point *is* output: without this, the `sawAnyOutput`
            // guard in the sign branch below still believes nothing has been
            // emitted, and a sign placed straight after the separator
            // ("`,-5`" in a de-DE locale) is accepted as if it were leading.
            // morph#497.
            sawAnyOutput = true;
            i += decimalSeparator.size();
            continue;
        }
        std::size_t const signLength = detail::leadingSignLength(rest, negativeSign);
        if (signLength != 0) {
            // Leading position of the *output*: a stripped group separator
            // before the sign would otherwise make an injected sign look
            // leading.
            if (sawAnyOutput) {
                return std::nullopt;  // sign injection past the leading position
            }
            canonical += '-';  // the canonical spelling, whatever the locale's is
            sawAnyOutput = true;
            i += signLength;
            continue;
        }
        // i is bounded by the loop condition.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        char const chr = text[i];
        if (chr < '0' || chr > '9') {
            return std::nullopt;  // any other character is malformed
        }
        canonical += chr;
        sawAnyOutput = true;
        ++i;
    }

    if (canonical.empty() || canonical == "-") {
        return std::nullopt;
    }
    return canonical;
}

/// @brief Converts canonical (`.`-decimal) numeric text to locale-formatted
///        display text, grouping the integer part in triples.
///
/// The display-direction inverse of `normalizeLocaleNumber`'s
/// decimal-separator substitution, plus thousands grouping that
/// `normalizeLocaleNumber` takes back: a grouped display round-trips, because
/// the entry direction *validates* the grouping rather than stripping it (see
/// "Grouping is validated, not stripped" on that function). Passing
/// `decimalSeparator == "."` and an empty @p groupSeparator is the identity
/// transform.
///
/// @par What this paragraph used to say, and why it was wrong (morph#597)
/// It claimed grouping was "never accepted back on entry" and that
/// `normalizeLocaleNumber` "strips it unconditionally" -- the pre-morph#574
/// behaviour, and false in two opposite directions at once. Measured on
/// `be64026a`: `normalizeLocaleNumber("1.050,25", ",", ".")` is `"1050.25"`,
/// so grouping *is* accepted back; `normalizeLocaleNumber("1.5", ",", ".")` is
/// `std::nullopt`, so it is *not* stripped unconditionally -- unconditional
/// stripping is exactly what would have made that entry `15`, the silent
/// ten-times-wrong value morph#574 exists to prevent. The spec
/// (`docs/spec/forms/forms.md`, "Grouping is validated, never merely
/// stripped") and the code already agreed; only this comment was stale.
///
/// The sign is emitted as @p negativeSign, matching what `normalizeLocaleNumber`
/// accepts back (morph#583); an empty view is read as `"-"` rather than as "no
/// sign", because formatting a negative to no sign at all is a silently wrong
/// value.
/// @param canonicalText    Canonical `-?[0-9]+(\.[0-9]+)?` text.
/// @param decimalSeparator The locale's decimal-point display string.
/// @param groupSeparator   The locale's digit-grouping display string, or empty
///                         to omit grouping.
/// @param negativeSign     The locale's negative-sign display string; empty is
///                         read as the default `"-"`.
/// @return The locale-formatted display text.
// Mirrors normalizeLocaleNumber's parameter order; the two are inverses, so
// diverging here would be the more confusing choice.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] inline std::string formatCanonicalNumber(std::string_view canonicalText,
                                                       std::string_view decimalSeparator,
                                                       std::string_view groupSeparator,
                                                       std::string_view negativeSign = "-") {
    bool const neg = !canonicalText.empty() && canonicalText.front() == '-';
    std::string_view const magnitude = neg ? canonicalText.substr(1) : canonicalText;
    auto const dot = magnitude.find('.');
    std::string_view const wholePart = dot == std::string_view::npos ? magnitude : magnitude.substr(0, dot);
    std::string_view const fracPart = dot == std::string_view::npos ? std::string_view{} : magnitude.substr(dot + 1);

    std::string grouped;
    grouped.reserve(wholePart.size() + ((wholePart.size() / 3) * groupSeparator.size()));
    for (std::size_t i = 0; i < wholePart.size(); ++i) {
        if (!groupSeparator.empty() && i != 0 && (wholePart.size() - i) % 3 == 0) {
            grouped += groupSeparator;
        }
        grouped += wholePart[i];
    }

    std::string out;
    if (neg) {
        out += negativeSign.empty() ? std::string_view{"-"} : negativeSign;
    }
    out += grouped;
    if (!fracPart.empty()) {
        out += decimalSeparator;
        out += fracPart;
    }
    return out;
}

}  // namespace morph::render
