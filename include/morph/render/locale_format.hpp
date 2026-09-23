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
/// @par One aggregate, not a row of swappable views
/// Both functions take a single `NumericLocale` rather than a row of positional
/// `std::string_view`s. Six adjacent views of the same type are silently
/// swappable with each other, which is what `bugprone-easily-swappable-parameters`
/// exists to catch; with the aggregate a call site names each fact
/// (`{.decimalSeparator = ",", .groupSeparator = "."}`) and no two parameters of
/// either function share a type. The next locale fact -- a percent sign, an
/// exponent separator -- is a new defaulted member rather than a seventh
/// parameter. The two edges taking the *same* type is the point as much as the
/// naming is: "these two must agree" is structural instead of a convention a
/// caller can get half right.
///
/// @par Separators are strings, not characters
/// The locale facts are `std::string_view`, because a real locale's separator
/// is not always one byte. fr-FR groups with U+202F (narrow no-break space) and
/// several locales use U+00A0 -- three and two UTF-8 bytes respectively. Typed
/// as `char`, those cannot be expressed at all: the caller can only pass some
/// single byte that never matches, so a perfectly valid `"1 050,25"` typed by a
/// French user normalises to `std::nullopt` and the entry is reported
/// malformed. An empty view means "this locale has no such separator".
///
/// @par So is the negative sign
/// For the same reason, and measured rather than assumed: of the 711 locales
/// Qt 6.11.2 knows, 77 report a `negativeSign` that is not a bare ASCII `'-'`.
/// 23 use U+2212 MINUS SIGN (e.g. eu_ES); 54 more prefix the sign with a bidi
/// control -- U+061C ARABIC LETTER MARK, U+200E LEFT-TO-RIGHT MARK or U+200F
/// RIGHT-TO-LEFT MARK -- making it two or three code points, and ar_DZ does so
/// even though its sign is the ordinary hyphen. Matched as a single `char`,
/// none of those round-trips as a single `char`: the display edge would emit a
/// sign the entry edge then rejected. So `negativeSign` is matched as a whole
/// string too, defaulting to the ASCII `"-"`.
///
/// @par And so is the positive sign, on the entry edge only
/// `normalizeLocaleNumber` reads `NumericLocale::positiveSign` and *drops* what
/// it matches, because canonical text has no `'+'` in it.
/// `formatCanonicalNumber` never emits one: a positive number displays unsigned
/// in every locale, and changing that would alter every positive number the
/// product shows. So the two functions are inverse across the decimal
/// separator, the grouping, the digits and the negative sign, but deliberately
/// not across a positive sign -- entry accepts a spelling display never
/// produces.
///
/// @par The digits are locale data too
/// `NumericLocale::zeroDigit` is the locale's DIGIT ZERO, and the ten digits
/// are the ten code points contiguous from it. One base is sufficient rather
/// than a ten-element table because a Unicode decimal digit set *is* ten
/// contiguous code points: UAX #44 assigns `Nd` with `Numeric_Value` 0 through
/// 9 in code point order. Measured over the same 711 locales under Qt 6.11.2,
/// 76 report a `zeroDigit` other than ASCII `'0'`, across eleven distinct sets:
/// U+0660 (26 locales), U+06F0 (19), U+1E950 Adlam (12), U+0966 (8), U+09E6
/// (4), U+11136 Chakma (2), and one each of U+07C0, U+0F20, U+1040, U+1C50 and
/// U+ABF0. Two of those sets are astral, so a digit is one to four UTF-8 bytes
/// and the scan decodes a code point rather than comparing a byte.
///
/// @par Entry accepts more digit spellings than display emits
/// `normalizeLocaleNumber` accepts a digit in `[zeroDigit, zeroDigit + 9]` *or*
/// in `['0', '9']`; `formatCanonicalNumber` emits only the former. That is the
/// same asymmetry the signs have, applied to digits: an ASCII `'+'` is
/// accepted in every locale because the locale's own spelling is
/// on no keyboard, and an ASCII `'5'` is accepted in an `ar_EG` locale for
/// exactly the same reason. A user with an ASCII keyboard in a native-digit
/// locale would otherwise be unable to enter a number at all. As with the
/// positive sign, accepting a spelling display never produces costs nothing:
/// the canonical output spells every digit in ASCII whatever the input spelled
/// it, so no *value* can differ. What is not accepted is the two families in
/// one entry -- see "Digit families do not mix" on `normalizeLocaleNumber`.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace morph::render {

/// @brief The locale facts both control-edge conversions need, in one
///        designated-initialisable aggregate.
///
/// Every member is defaulted to its `"C"`-locale spelling, so a
/// default-constructed `NumericLocale` is the identity transform in both
/// directions and a caller naming only the members it cares about gets the
/// `"C"` spelling for the rest.
struct NumericLocale {
    /// The locale's decimal-point string, e.g. `","`. Empty means the locale
    /// has no decimal separator (an integer-only entry).
    std::string_view decimalSeparator = ".";
    /// The locale's digit-grouping string, e.g. `"."` or U+202F. Empty means
    /// the locale does not group.
    std::string_view groupSeparator;
    /// The locale's negative-sign string, e.g. `"\u2212"`. Empty is read as the
    /// ASCII `"-"`, not as "this entry cannot be negative".
    std::string_view negativeSign = "-";
    /// The locale's positive-sign string, e.g. `"\u061c+"`. Entry-edge only:
    /// it is accepted and dropped, never emitted. Empty leaves the ASCII `"+"`
    /// as the only accepted spelling.
    std::string_view positiveSign = "+";
    /// The locale's DIGIT ZERO, e.g. `"\u0660"`. The ten digits are the ten
    /// code points contiguous from it. Empty, or not a well-formed UTF-8 code
    /// point, is read as the ASCII `"0"`.
    std::string_view zeroDigit = "0";
};

namespace detail {

/// @brief One decoded UTF-8 code point: its value and how many bytes it took.
struct CodePoint {
    /// The decoded scalar value; meaningless when @ref length is `0`.
    char32_t value = 0;
    /// The number of bytes consumed, or `0` when the input does not begin with
    /// a well-formed UTF-8 sequence.
    std::size_t length = 0;
};

/// @brief Decodes the UTF-8 sequence at the start of @p text.
///
/// Strict: an overlong encoding, a surrogate code point, a truncated sequence
/// and a stray continuation byte all report a `CodePoint::length` of `0`
/// rather than some salvaged value. That strictness is load-bearing rather than
/// pedantic. The digit test below is a *range* test on the decoded value, so a
/// lenient decoder that let the overlong `C0 B5` through would read it as
/// U+0035 and accept it as the digit `'5'` -- including in the default ASCII
/// locale, where that input is not a digit at all.
/// @param text The remainder of the entry, starting at the scan position.
/// @return The decoded code point, or a `length` of `0` when @p text does not
///         start with a well-formed sequence.
[[nodiscard]] inline CodePoint decodeUtf8(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    auto const lead = static_cast<unsigned char>(text.front());
    if (lead < 0x80U) {
        return {.value = lead, .length = 1};
    }

    std::size_t length = 0;
    char32_t value = 0;
    char32_t least = 0;  // the smallest value this length may legally encode
    if ((lead & 0xE0U) == 0xC0U) {
        length = 2;
        value = lead & 0x1FU;
        least = 0x80;
    } else if ((lead & 0xF0U) == 0xE0U) {
        length = 3;
        value = lead & 0x0FU;
        least = 0x800;
    } else if ((lead & 0xF8U) == 0xF0U) {
        length = 4;
        value = lead & 0x07U;
        least = 0x10000;
    } else {
        return {};  // a continuation byte or a 5+ byte lead: not a lead byte
    }
    if (text.size() < length) {
        return {};  // truncated
    }
    for (std::size_t k = 1; k < length; ++k) {
        // k is bounded by the size check above.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        auto const cont = static_cast<unsigned char>(text[k]);
        if ((cont & 0xC0U) != 0x80U) {
            return {};  // not a continuation byte
        }
        value = (value << 6U) | (cont & 0x3FU);
    }
    constexpr char32_t kMaxScalar = 0x10FFFF;
    constexpr char32_t kSurrogateFirst = 0xD800;
    constexpr char32_t kSurrogateLast = 0xDFFF;
    if (value < least || value > kMaxScalar || (value >= kSurrogateFirst && value <= kSurrogateLast)) {
        return {};  // overlong, out of range, or a surrogate
    }
    return {.value = value, .length = length};
}

/// @brief Appends @p value to @p out as UTF-8.
/// @param out   The string to append to.
/// @param value A scalar value; callers here only ever pass a digit derived
///              from a `zeroDigit` that `decodeUtf8` already validated.
inline void appendUtf8(std::string& out, char32_t value) {
    constexpr char32_t kContMask = 0x3F;
    constexpr unsigned kContShift = 6;
    auto const byte = [&out](char32_t bits) { out += static_cast<char>(bits); };
    if (value < 0x80) {
        byte(value);
    } else if (value < 0x800) {
        byte(0xC0U | (value >> kContShift));
        byte(0x80U | (value & kContMask));
    } else if (value < 0x10000) {
        byte(0xE0U | (value >> (2 * kContShift)));
        byte(0x80U | ((value >> kContShift) & kContMask));
        byte(0x80U | (value & kContMask));
    } else {
        byte(0xF0U | (value >> (3 * kContShift)));
        byte(0x80U | ((value >> (2 * kContShift)) & kContMask));
        byte(0x80U | ((value >> kContShift) & kContMask));
        byte(0x80U | (value & kContMask));
    }
}

/// @brief Appends one character of canonical text to a display string, with a
///        digit rewritten into the locale's set.
///
/// Anything that is not an ASCII digit is copied through verbatim. That arm is
/// not dead code for a caller that honours the contract, but it is what bounds
/// the damage for one that does not: a `"12.34.56"` handed to the display edge
/// keeps its second `'.'` rather than becoming some code point below the digit
/// base. Only digits are rewritten; text that has none passes through.
/// @param out  The display string to append to.
/// @param base The locale's DIGIT ZERO code point (see `digitBase`).
/// @param chr  One byte of canonical text.
inline void appendDisplayDigit(std::string& out, char32_t base, char chr) {
    if (chr >= '0' && chr <= '9') {
        appendUtf8(out, base + static_cast<char32_t>(chr - '0'));
    } else {
        out += chr;
    }
}

/// @brief How many bytes @p value occupies in UTF-8.
/// @param value A scalar value.
/// @return `1`, `2`, `3` or `4`.
[[nodiscard]] inline std::size_t utf8Length(char32_t value) {
    if (value < 0x80) {
        return 1;
    }
    if (value < 0x800) {
        return 2;
    }
    if (value < 0x10000) {
        return 3;
    }
    return 4;
}

/// @brief The code point of @p zeroDigit, or `U'0'` when it is empty or not a
///        well-formed UTF-8 code point.
///
/// Empty reads as the ASCII `'0'` for the reason an empty `negativeSign` reads
/// as `'-'`: there is no locale without digits, so empty cannot mean absence,
/// and a base of "nothing" would reject every entry the locale can produce.
/// Only the first code point is read; anything after it is locale data this
/// function has no use for.
/// @param zeroDigit The locale's DIGIT ZERO spelling.
/// @return The base code point of the locale's digit set.
[[nodiscard]] inline char32_t digitBase(std::string_view zeroDigit) {
    CodePoint const decoded = decodeUtf8(zeroDigit);
    return decoded.length == 0 ? U'0' : decoded.value;
}

/// @brief A digit matched at the start of an entry.
struct DigitMatch {
    /// The number of bytes the digit occupies; `0` when there is no digit there.
    std::size_t length = 0;
    /// The canonical ASCII spelling of the digit's value, `'0'`-`'9'`.
    char canonical = '0';
    /// Whether the match came from the locale's own digit set rather than from
    /// the ASCII set every locale additionally accepts. When @p base is `U'0'`
    /// the two sets coincide and this is always `true`.
    bool native = false;
};

/// @brief Matches a digit at the start of @p rest, in the locale's set or in
///        ASCII.
///
/// A function of its own for the reason `leadingSign` is: the normalising scan
/// reads as one statement per character class, and folding the two digit
/// families plus the UTF-8 decode into it took it over clang-tidy's cognitive
/// complexity threshold.
/// @param rest The remainder of the entry, starting at the scan position.
/// @param base The locale's DIGIT ZERO code point (see `digitBase`).
/// @return The match, or a `length` of `0` when @p rest starts with no digit.
[[nodiscard]] inline DigitMatch leadingDigit(std::string_view rest, char32_t base) {
    CodePoint const decoded = decodeUtf8(rest);
    if (decoded.length == 0) {
        return {};
    }
    if (decoded.value >= base && decoded.value < base + 10) {
        return {
            .length = decoded.length, .canonical = static_cast<char>(U'0' + (decoded.value - base)), .native = true};
    }
    if (decoded.value >= U'0' && decoded.value <= U'9') {
        // The ASCII spelling, accepted in every locale for the reason the ASCII
        // '-' and '+' are: the locale's own digits are on the user's keyboard
        // only if their keyboard has them.
        return {.length = decoded.length, .canonical = static_cast<char>(decoded.value), .native = false};
    }
    return {};
}

/// @brief Whether every group separator in @p text sits where a group
///        separator can legally sit.
///
/// The rule, in one place so it can be read on its own: a group separator is
/// preceded by one to three digits (the first group), or by exactly three
/// (every later one); it is followed by exactly three more digits, at each
/// group and at the end of the integer part; and it never appears after the
/// decimal separator. A locale with no grouping (an empty
/// `NumericLocale::groupSeparator`) has nothing to place, so it trivially
/// passes.
///
/// This is a pass of its own rather than extra state inside the normalising
/// scan below: "the grouping is well placed" and "the digits convert" are two
/// separate statements about the entry, and reading them as one made neither
/// clear.
///
/// It counts *digits*, not bytes, in the locale's set as well as in ASCII. A
/// byte-by-byte scan is only correct while a digit is one byte: a two-byte
/// U+0665 would reset the run-length counter on its own continuation byte, so
/// `"\u0661\u066c\u0660\u0665\u0660"` -- what `formatCanonicalNumber` emits
/// for `1050` in an `ar_EG` locale -- would be rejected as badly grouped and the
/// pair would not round-trip.
///
/// Characters this function does not recognise are simply not digits — the
/// normalising scan is what rejects them, and it rejects them whatever this
/// pass concludes.
/// @param text The locale-formatted entry.
/// @param loc  The locale facts; only the two separators and the digit base are
///             read.
/// @return `true` when the grouping is well placed (or absent).
[[nodiscard]] inline bool groupingIsWellPlaced(std::string_view text, NumericLocale const& loc) {
    if (loc.groupSeparator.empty()) {
        return true;
    }
    constexpr std::size_t kGroupSize = 3;
    char32_t const base = digitBase(loc.zeroDigit);
    std::size_t digits = 0;
    bool sawGroup = false;
    bool sawDecimal = false;

    for (std::size_t i = 0; i < text.size();) {
        const std::string_view rest = text.substr(i);
        if (rest.starts_with(loc.groupSeparator)) {
            bool const opensAGroup = sawGroup ? digits == kGroupSize : (digits >= 1 && digits <= kGroupSize);
            if (sawDecimal || !opensAGroup) {
                return false;
            }
            sawGroup = true;
            digits = 0;
            i += loc.groupSeparator.size();
            continue;
        }
        if (!loc.decimalSeparator.empty() && rest.starts_with(loc.decimalSeparator)) {
            if (sawGroup && digits != kGroupSize) {
                return false;  // the last group of the integer part is short
            }
            sawDecimal = true;
            digits = 0;
            i += loc.decimalSeparator.size();
            continue;
        }
        DigitMatch const digit = leadingDigit(rest, base);
        digits = digit.length != 0 ? digits + 1 : 0;
        i += digit.length != 0 ? digit.length : 1;
    }

    // An ungrouped fractional part ends the number, so the trailing check only
    // applies when the integer part was the last thing scanned.
    return !sawGroup || sawDecimal || digits == kGroupSize;
}

/// @brief The length of @p separator at the start of @p rest, in bytes, or `0`
///        when it is not there -- or is empty.
///
/// The empty case is why this is a function and not an inline `starts_with`:
/// `rest.starts_with("")` is `true` at every index, so an empty separator
/// matched inline would swallow the whole entry one zero-length step at a time.
/// Spelling the guard as `!sep.empty() && ...` at each call site instead adds
/// two conjunctions to `normalizeLocaleNumber`, which is enough to put it over
/// clang-tidy's cognitive-complexity threshold.
/// @param rest      The remainder of the entry, starting at the scan position.
/// @param separator The locale's spelling of this separator; empty means the
///                  locale has none, and matches nothing.
/// @return The number of bytes the separator occupies, or `0`.
[[nodiscard]] inline std::size_t leadingSeparatorLength(std::string_view rest, std::string_view separator) {
    return !separator.empty() && rest.starts_with(separator) ? separator.size() : 0U;
}

/// @brief The length of a sign at the start of @p rest, in bytes, or `0` when
///        there is none there.
///
/// Two spellings count. @p localeSign is the locale's own, matched as a whole
/// string so that U+2212 and the bidi-control-prefixed forms -- two and three
/// code points -- match at all; a single `char` can express none of them.
/// @p asciiSign counts as well, in every locale: U+2212
/// and the bidi marks are on no keyboard, so matching only the locale's
/// spelling would reject the sign the user can actually type. Neither `'-'` nor
/// `'+'` has a second reading in a numeric entry, so this is not the kind of
/// guess the grouping rule forbids.
///
/// This is a function of its own so that the caller's scan reads as one
/// statement per character class. It is shared by both signs and says nothing
/// about what the caller then emits, which differs between them: a negative is
/// emitted as the canonical `'-'`, a positive is dropped (see
/// `normalizeLocaleNumber`).
/// @param rest       The remainder of the entry, starting at the scan position.
/// @param localeSign The locale's spelling of this sign; empty matches nothing,
///                   leaving only the ASCII spelling.
/// @param asciiSign  The ASCII spelling accepted in every locale: `'-'` or `'+'`.
/// @return The number of bytes the sign occupies, or `0`.
[[nodiscard]] inline std::size_t leadingSignLength(std::string_view rest, std::string_view localeSign,
                                                   char asciiSign) {
    if (!localeSign.empty() && rest.starts_with(localeSign)) {
        return localeSign.size();
    }
    return rest.starts_with(asciiSign) ? 1U : 0U;
}

/// @brief A sign matched at the start of an entry: how much of the text it
///        occupies, and what it contributes to the canonical output.
struct SignMatch {
    /// The number of bytes the sign occupies; `0` when there is no sign there.
    std::size_t length = 0;
    /// What the canonical text gains: `"-"` for a negative, empty for a
    /// positive, which is accepted and dropped.
    std::string_view emits;
};

/// @brief Matches either sign at the start of @p rest.
///
/// Both signs in one function, and the emitted text carried back with the
/// length, so the normalising scan below has a *single* sign branch with no
/// inner "which sign was it" test. That is not only tidier: two branches with
/// two inner tests each put `normalizeLocaleNumber` at a cognitive complexity
/// of 28, over clang-tidy's threshold of 25; with the single branch it sits at
/// 23. The asymmetry between the two signs lives here, in the one place that
/// decides it, rather than in the scan.
///
/// The negative sign is tried first. The order is not load-bearing for any
/// locale Qt 6.11.2 reports -- `starts_with` is an exact prefix match and no
/// locale spells one sign as a prefix of the other -- but it is fixed here so
/// that it cannot vary.
/// @param rest The remainder of the entry, starting at the scan position.
/// @param loc  The locale facts; only the two signs are read. An empty
///             `negativeSign` leaves `'-'`, an empty `positiveSign` leaves
///             `'+'`.
/// @return The match, or a `length` of `0` when @p rest starts with no sign.
[[nodiscard]] inline SignMatch leadingSign(std::string_view rest, NumericLocale const& loc) {
    std::size_t const negativeLength = leadingSignLength(rest, loc.negativeSign, '-');
    if (negativeLength != 0) {
        // The canonical spelling, whatever the locale's is.
        return {.length = negativeLength, .emits = "-"};
    }
    // Dropped, never emitted: canonical text is `-?[0-9]+(\.[0-9]+)?` and has
    // no `+` in it.
    return {.length = leadingSignLength(rest, loc.positiveSign, '+'), .emits = ""};
}

}  // namespace detail

/// @brief Converts a locale-formatted numeric string to canonical
///        (`-?[0-9]+(\.[0-9]+)?`) text.
///
/// Drops `NumericLocale::groupSeparator` where it is correctly placed, replaces
/// every occurrence of `NumericLocale::decimalSeparator` with `.`, and rewrites
/// the locale's digits as ASCII ones. A default-constructed `NumericLocale` is
/// the identity transform (the locale-free behavior). Malformed input (a second
/// decimal separator, a sign anywhere but the leading position of the *output*,
/// two digit families in one entry, or any character that is not a digit)
/// yields `std::nullopt` rather than a best-effort guess. The decimal point
/// counts as output, so a sign placed straight after the separator ("`,-5`" in
/// a de-DE locale) is rejected -- matching the QML mirror in
/// `src/qt/forms/qml/DynamicForm.qml`, which rejects it too.
///
/// @par Grouping is validated, not stripped
/// A group separator is only dropped where a group separator can legally be:
/// preceded by one to three digits, followed by exactly three more, and never
/// after the decimal separator. Anything else is malformed and reported as
/// such. Stripping unconditionally instead yields a wrong *value*, not a
/// rejected one, and nothing downstream can tell: a de-DE user typing the US
/// form `"1.5"` into a price field would submit `15` -- a perfectly valid
/// number, ten times too large. `"1.50"` would give `150`, `"1.2.3.4"` would
/// give `1234`, and the en-US mirror image `"1,5"` would give `15`.
///
/// @par The two separators must differ
/// When `groupSeparator` is non-empty and equal to `decimalSeparator` the entry
/// is rejected: with one string in both roles there is no reading of `"1.5"`
/// the function could defend. This is a caller (locale-configuration) error
/// rather than a user one, but it is reported through the return value like any
/// other malformed entry, deliberately not through an assertion -- an assertion
/// would make the two build configurations behave differently at a control
/// edge, and would be untestable in the one where it fires.
///
/// The result is `.`-decimal and digit-only, but is **not** narrowed to
/// `-?[0-9]+(\.[0-9]+)?`: a bare "`.`", a leading "`.5`" and a trailing "`5.`"
/// are passed through, exactly as that same QML mirror passes them. Tightening
/// one side alone would put the two control edges out of step, so the shape is
/// documented here rather than narrowed.
///
/// Separators are matched as whole strings, so a multi-byte one (e.g. U+202F)
/// works; matching them before the digit scan is what keeps their continuation
/// bytes from being mistaken for stray non-digit characters.
///
/// @par The negative sign is matched as a whole string too
/// `negativeSign` is matched the same way, which is what lets a locale whose
/// sign is U+2212, or is prefixed by a bidi control mark, be entered at all --
/// 77 of the 711 locales Qt 6.11.2 knows. Matched as a literal byte `'-'`
/// instead, `formatCanonicalNumber` would emit a sign this function rejected
/// and the pair would not be inverse for those locales.
///
/// @par ASCII `'-'` stays accepted whatever the locale
/// A bare `'-'` is accepted in the leading position in addition to
/// `negativeSign`. U+2212 and the bidi marks are on no keyboard, so matching
/// only the locale's own spelling would reject the sign the user can actually
/// type and leave them no way to enter a negative number at all. The hyphen has
/// no second reading in a numeric entry, so accepting it is not the kind of
/// guess the grouping rule forbids -- that one is about producing a wrong
/// *value*, and this produces the only value the input can mean.
///
/// @par An empty `negativeSign` means the ASCII default, not "no sign"
/// Unlike a group separator, there is no locale without a negative sign, so an
/// empty view leaves the ASCII `'-'` above as the only spelling rather than
/// meaning "this entry cannot be negative". `formatCanonicalNumber` reads it
/// the same way, and the display edge is why it must: a sign that formatted to
/// nothing would turn `-5` into `5` silently -- a wrong value, not a rejected
/// one.
///
/// @par A leading positive sign is accepted and dropped
/// `positiveSign` is matched exactly like `negativeSign` -- the locale's own
/// spelling as a whole string, plus a bare ASCII `'+'` in every locale. Of the
/// 711 locales Qt 6.11.2 knows, 54 spell it as more than one code point
/// (U+061C, U+200E or U+200F before the `'+'`, e.g. `ar_EG`, `ar_DZ`, `az_IR`,
/// `ckb_IQ`); the other 657 use the bare `'+'`. Unlike the negative side there
/// is no U+2212 analogue, so *every* non-ASCII spelling here is multi-code-point
/// and whole-string matching is the only thing that can match any of them.
/// Without this branch a leading `'+'` falls through to the "any other
/// character is malformed" arm, and an explicitly-positive entry is rejected in
/// every locale, `"C"` included.
///
/// @par The sign is **dropped**, and `formatCanonicalNumber` never emits one
/// This is a deliberate asymmetry with the negative sign, not an oversight.
/// Canonical text is `-?[0-9]+(\.[0-9]+)?`: there is no `'+'` in it, so `"+5"`
/// yields `"5"` and not `"+5"`. The display edge reads no `positiveSign` at
/// all, because emitting one would change what every positive number in every
/// form looks like -- `5` would become `+5` on screen. So the two functions are
/// *not* strict inverses across a positive sign: entry accepts a spelling
/// display never produces. That is the only shape that adds acceptance without
/// changing a single rendered value. Written down in `docs/spec/forms/forms.md`
/// as well, under "Locale data formatting".
///
/// @par An empty `positiveSign` leaves the ASCII `'+'`
/// Here empty really can mean "match nothing extra", because there is no
/// display edge to get wrong: the worst an unmatched positive sign can do is
/// reject an entry, never produce a value of the wrong sign. The bare ASCII
/// `'+'` stays accepted regardless.
///
/// @par The locale's own digits are accepted, and so are ASCII ones
/// A digit is accepted when its code point is in
/// `[zeroDigit, zeroDigit + 9]` -- a Unicode decimal digit set is ten
/// contiguous code points by definition (UAX #44) -- *or* in `['0', '9']`. 76
/// of the 711 locales Qt 6.11.2 knows use a non-ASCII `zeroDigit`; without the
/// first acceptance their users could not enter a number at all, because a scan
/// comparing a single byte against the ASCII range fails on the very first byte
/// of U+0665. The second acceptance is the same rule as the ASCII `'-'` and
/// `'+'` above, for the same reason: a user with an ASCII keyboard in an
/// `ar_EG` locale has to be able to type `5`. It costs nothing, because the
/// canonical output spells every digit in ASCII whatever the input spelled it,
/// so no two accepted spellings can produce different *values*.
///
/// @par Digit families do not mix
/// `"\u06655"` -- one Arabic-Indic digit and one ASCII digit -- is malformed,
/// not `"55"`. The two families are each accepted whole; interleaving them is
/// not a spelling any keyboard or any display edge produces, and rejecting it
/// matches the strictness about a sign anywhere but the leading position. It is
/// a choice rather than a consequence, so it is stated here and pinned by a
/// test on both edges. When `zeroDigit` is the ASCII `"0"` the two families are
/// the same set, so nothing can mix and the rule is invisible.
///
/// @param text The locale-formatted entry, e.g. `"1.050,25"`.
/// @param loc  The locale facts. Designated initialisers are the intended
///             spelling: `{.decimalSeparator = ",", .groupSeparator = "."}`.
/// @return The canonical `.`-decimal text, or `std::nullopt` when malformed.
[[nodiscard]] inline std::optional<std::string> normalizeLocaleNumber(std::string_view text,
                                                                      NumericLocale const& loc) {
    if (!loc.groupSeparator.empty() && loc.groupSeparator == loc.decimalSeparator) {
        return std::nullopt;  // one string cannot play both roles: see above
    }
    if (!detail::groupingIsWellPlaced(text, loc)) {
        return std::nullopt;  // a separator off a group boundary: see above
    }

    char32_t const base = detail::digitBase(loc.zeroDigit);
    std::string canonical;
    canonical.reserve(text.size());
    bool sawDecimal = false;
    bool sawAnyOutput = false;
    bool sawNativeDigit = false;
    bool sawAsciiDigit = false;

    for (std::size_t i = 0; i < text.size();) {
        const std::string_view rest = text.substr(i);
        if (std::size_t const group = detail::leadingSeparatorLength(rest, loc.groupSeparator); group != 0) {
            // Placement was settled above, so by here the separator is display
            // only and is never carried into the output.
            i += group;
            continue;
        }
        if (std::size_t const point = detail::leadingSeparatorLength(rest, loc.decimalSeparator); point != 0) {
            if (sawDecimal) {
                return std::nullopt;  // a second decimal separator: malformed
            }
            sawDecimal = true;
            canonical += '.';
            // The decimal point *is* output: without this, the `sawAnyOutput`
            // guard in the sign branch below still believes nothing has been
            // emitted, and a sign placed straight after the separator
            // ("`,-5`" in a de-DE locale) is accepted as if it were leading.
            sawAnyOutput = true;
            i += point;
            continue;
        }
        detail::SignMatch const sign = detail::leadingSign(rest, loc);
        if (sign.length != 0) {
            // Leading position of the *output*: a stripped group separator
            // before the sign would otherwise make an injected sign look
            // leading. The same rule for both signs, which is why they share a
            // branch.
            if (sawAnyOutput) {
                return std::nullopt;  // sign injection past the leading position
            }
            // Empty for a positive sign, which is dropped rather than
            // carried. `sawAnyOutput` is set either way, so "+-5", "++5" and
            // "1+2" stay malformed: consuming a sign counts as output even when
            // it contributes no character.
            canonical += sign.emits;
            sawAnyOutput = true;
            i += sign.length;
            continue;
        }
        detail::DigitMatch const digit = detail::leadingDigit(rest, base);
        if (digit.length == 0) {
            return std::nullopt;  // any other character is malformed
        }
        // Which family each digit came from is recorded here and judged once,
        // below the loop: an entry that mixes them is malformed wherever the
        // second family appears, so there is nothing an early return would
        // decide differently, and the scan stays one statement per character
        // class.
        (digit.native ? sawNativeDigit : sawAsciiDigit) = true;
        canonical += digit.canonical;
        sawAnyOutput = true;
        i += digit.length;
    }

    if (sawNativeDigit && sawAsciiDigit) {
        return std::nullopt;  // two digit families in one entry: see above
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
/// decimal-separator substitution and digit rewriting, plus thousands grouping
/// that `normalizeLocaleNumber` takes back: a grouped display round-trips,
/// because the entry direction *validates* the grouping rather than stripping
/// it (see "Grouping is validated, not stripped" on that function). A
/// default-constructed `NumericLocale` is the identity transform.
///
/// The sign is emitted as `NumericLocale::negativeSign`, matching what
/// `normalizeLocaleNumber` accepts back; an empty view is read as
/// `"-"` rather than as "no sign", because formatting a negative to no sign at
/// all is a silently wrong value.
///
/// @par The digits are emitted in the locale's set
/// Each canonical `'0'`-`'9'` is emitted as the code point that far above
/// `NumericLocale::zeroDigit`, so an `ar_EG` caller sees `"\u0665"` where the
/// canonical text said `'5'`. This edge has to match the entry edge: entry
/// accepts U+0665, so display emitting a plain `'5'` would leave the pair not
/// inverse, and `docs/spec/forms/forms.md` requires that round trip. With the
/// default `zeroDigit` of `"0"` the offset is zero and the canonical ASCII
/// bytes are copied out unchanged.
///
/// @par There is no positive-sign emission, deliberately
/// A positive number is displayed with no sign at all, in every locale, and
/// this function ignores `NumericLocale::positiveSign` entirely.
/// `normalizeLocaleNumber` *accepts* a leading positive sign and drops it, so
/// the pair is not a strict inverse across one: entry takes a spelling display
/// never produces. Emitting it is what would be the defect --
/// `QLocale::positiveSign()` is `'+'` in 657 of the 711 locales Qt 6.11.2
/// knows, so emitting it would turn every positive number in every form from
/// `5` into `+5`, a visible product change with no need behind it. The failure
/// this pair guards against is one edge producing text the other rejects;
/// emitting a sign nothing asks for would create exactly that.
/// @param canonicalText Canonical `-?[0-9]+(\.[0-9]+)?` text.
/// @param loc           The locale facts; `positiveSign` is not read.
/// @return The locale-formatted display text.
[[nodiscard]] inline std::string formatCanonicalNumber(std::string_view canonicalText, NumericLocale const& loc) {
    constexpr std::size_t kGroupSize = 3;
    char32_t const base = detail::digitBase(loc.zeroDigit);
    bool const neg = !canonicalText.empty() && canonicalText.front() == '-';
    std::string_view const magnitude = neg ? canonicalText.substr(1) : canonicalText;
    auto const dot = magnitude.find('.');
    std::string_view const wholePart = dot == std::string_view::npos ? magnitude : magnitude.substr(0, dot);
    std::string_view const fracPart = dot == std::string_view::npos ? std::string_view{} : magnitude.substr(dot + 1);

    // A digit of the locale's set is one to four UTF-8 bytes, so the reserve
    // hint scales with the base rather than assuming one byte per digit.
    std::size_t const perDigit = detail::utf8Length(base);
    std::string out;
    out.reserve(loc.negativeSign.size() + (magnitude.size() * perDigit) +
                ((wholePart.size() / kGroupSize) * loc.groupSeparator.size()) + loc.decimalSeparator.size());

    if (neg) {
        out += loc.negativeSign.empty() ? std::string_view{"-"} : loc.negativeSign;
    }
    std::size_t index = 0;
    for (char const chr : wholePart) {
        if (!loc.groupSeparator.empty() && index != 0 && (wholePart.size() - index) % kGroupSize == 0) {
            out += loc.groupSeparator;
        }
        detail::appendDisplayDigit(out, base, chr);
        ++index;
    }
    if (!fracPart.empty()) {
        out += loc.decimalSeparator;
        for (char const chr : fracPart) {
            detail::appendDisplayDigit(out, base, chr);
        }
    }
    return out;
}

}  // namespace morph::render
