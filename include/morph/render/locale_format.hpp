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

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace morph::render {

/// @brief Converts a locale-formatted numeric string to canonical
///        (`-?[0-9]+(\.[0-9]+)?`) text.
///
/// Strips every occurrence of @p groupSeparator, then replaces every
/// occurrence of @p decimalSeparator with `.`. Passing `decimalSeparator ==
/// '.'` and `groupSeparator == '\0'` is the identity transform (today's
/// locale-free behavior). Malformed input (a second decimal separator, a
/// sign anywhere but the leading position, or any character that is not a
/// digit) yields `std::nullopt` rather than a best-effort guess.
/// @param text             The locale-formatted entry, e.g. `"1.050,25"`.
/// @param decimalSeparator The locale's decimal-point character, e.g. `','`.
/// @param groupSeparator   The locale's digit-grouping character, e.g.
///                         `'.'`, or `'\0'` when the locale has none.
/// @return The canonical `.`-decimal text, or `std::nullopt` when malformed.
[[nodiscard]] inline std::optional<std::string> normalizeLocaleNumber(std::string_view text, char decimalSeparator,
                                                                      char groupSeparator) {
    std::string stripped;
    stripped.reserve(text.size());
    for (char const ch : text) {
        if (groupSeparator != '\0' && ch == groupSeparator) {
            continue;
        }
        stripped += ch;
    }

    std::string canonical;
    canonical.reserve(stripped.size());
    bool sawDecimal = false;
    for (std::size_t i = 0; i < stripped.size(); ++i) {
        char const ch = stripped[i];
        if (ch == decimalSeparator) {
            if (sawDecimal) {
                return std::nullopt;  // a second decimal separator: malformed
            }
            sawDecimal = true;
            canonical += '.';
        } else if (ch == '-') {
            if (i != 0) {
                return std::nullopt;  // sign injection past the leading position
            }
            canonical += ch;
        } else if (ch >= '0' && ch <= '9') {
            canonical += ch;
        } else {
            return std::nullopt;  // any other character is malformed
        }
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
/// strips it unconditionally). Passing `decimalSeparator == '.'` and
/// `groupSeparator == '\0'` is the identity transform.
/// @param canonicalText    Canonical `-?[0-9]+(\.[0-9]+)?` text.
/// @param decimalSeparator The locale's decimal-point display character.
/// @param groupSeparator   The locale's digit-grouping display character, or
///                         `'\0'` to omit grouping.
/// @return The locale-formatted display text.
[[nodiscard]] inline std::string formatCanonicalNumber(std::string_view canonicalText, char decimalSeparator,
                                                       char groupSeparator) {
    bool const neg = !canonicalText.empty() && canonicalText.front() == '-';
    std::string_view const magnitude = neg ? canonicalText.substr(1) : canonicalText;
    auto const dot = magnitude.find('.');
    std::string_view const wholePart = dot == std::string_view::npos ? magnitude : magnitude.substr(0, dot);
    std::string_view const fracPart = dot == std::string_view::npos ? std::string_view{} : magnitude.substr(dot + 1);

    std::string grouped;
    grouped.reserve(wholePart.size() + (wholePart.size() / 3));
    for (std::size_t i = 0; i < wholePart.size(); ++i) {
        if (groupSeparator != '\0' && i != 0 && (wholePart.size() - i) % 3 == 0) {
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
