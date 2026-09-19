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

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace morph::render {

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
/// @param text             The locale-formatted entry, e.g. `"1.050,25"`.
/// @param decimalSeparator The locale's decimal-point string, e.g. `","`.
/// @param groupSeparator   The locale's digit-grouping string, e.g. `"."`, or
///                         empty when the locale has none.
/// @return The canonical `.`-decimal text, or `std::nullopt` when malformed.
[[nodiscard]] inline std::optional<std::string> normalizeLocaleNumber(std::string_view text,
                                                                      std::string_view decimalSeparator,
                                                                      std::string_view groupSeparator) {
    if (!groupSeparator.empty() && groupSeparator == decimalSeparator) {
        return std::nullopt;  // one string cannot play both roles: see above
    }

    std::string canonical;
    canonical.reserve(text.size());
    bool sawDecimal = false;
    bool sawAnyOutput = false;
    // Grouping state: how many digits since the last group separator (or since
    // the start), and whether any group separator has been seen at all.
    std::size_t digitsInGroup = 0;
    bool sawGroup = false;
    constexpr std::size_t kGroupSize = 3;

    for (std::size_t i = 0; i < text.size();) {
        const std::string_view rest = text.substr(i);
        if (!groupSeparator.empty() && rest.starts_with(groupSeparator)) {
            if (sawDecimal) {
                return std::nullopt;  // grouping belongs to the integer part only
            }
            // The first group is one to three digits ("1.050", "12.050",
            // "123.050"); every later one is exactly three.
            bool const wellPlaced =
                sawGroup ? digitsInGroup == kGroupSize : (digitsInGroup >= 1 && digitsInGroup <= kGroupSize);
            if (!wellPlaced) {
                return std::nullopt;  // not a group boundary: malformed
            }
            sawGroup = true;
            digitsInGroup = 0;
            i += groupSeparator.size();
            continue;  // grouping is display-only; never carried into the output
        }
        if (!decimalSeparator.empty() && rest.starts_with(decimalSeparator)) {
            if (sawDecimal) {
                return std::nullopt;  // a second decimal separator: malformed
            }
            if (sawGroup && digitsInGroup != kGroupSize) {
                return std::nullopt;  // the last group is short: "1.5" in de-DE
            }
            sawDecimal = true;
            canonical += '.';
            // The decimal point *is* output: without this, the `chr == '-'`
            // guard below still believes nothing has been emitted, and a sign
            // placed straight after the separator ("`,-5`" in a de-DE locale)
            // is accepted as if it were leading. morph#497.
            sawAnyOutput = true;
            i += decimalSeparator.size();
            continue;
        }
        // i is bounded by the loop condition.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        char const chr = text[i];
        if (chr == '-') {
            // Leading position of the *output*: a stripped group separator
            // before the sign would otherwise make an injected sign look
            // leading.
            if (sawAnyOutput) {
                return std::nullopt;  // sign injection past the leading position
            }
            canonical += chr;
        } else if (chr >= '0' && chr <= '9') {
            canonical += chr;
            ++digitsInGroup;
        } else {
            return std::nullopt;  // any other character is malformed
        }
        sawAnyOutput = true;
        ++i;
    }

    // A grouped integer part has to end on a group boundary too: "1.05" and
    // "1.050." are as malformed as "1.5" is.
    if (sawGroup && !sawDecimal && digitsInGroup != kGroupSize) {
        return std::nullopt;
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
/// decimal-separator substitution, plus display-only thousands grouping
/// (grouping is never accepted back on entry — `normalizeLocaleNumber`
/// strips it unconditionally). Passing `decimalSeparator == "."` and an empty
/// @p groupSeparator is the identity transform.
/// @param canonicalText    Canonical `-?[0-9]+(\.[0-9]+)?` text.
/// @param decimalSeparator The locale's decimal-point display string.
/// @param groupSeparator   The locale's digit-grouping display string, or empty
///                         to omit grouping.
/// @return The locale-formatted display text.
// Mirrors normalizeLocaleNumber's parameter order; the two are inverses, so
// diverging here would be the more confusing choice.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] inline std::string formatCanonicalNumber(std::string_view canonicalText,
                                                       std::string_view decimalSeparator,
                                                       std::string_view groupSeparator) {
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
        out += '-';
    }
    out += grouped;
    if (!fracPart.empty()) {
        out += decimalSeparator;
        out += fracPart;
    }
    return out;
}

}  // namespace morph::render
