// SPDX-License-Identifier: Apache-2.0
//
// The accepted spelling. Every bidi control here is an escape, so the bytes on
// disk are ASCII and a reviewer reads exactly what the compiler reads. This is
// what src/qt/forms/tests/tst_i18n.qml does for all thirteen of its
// occurrences, and what morph#610 converted nine assertion lines to.
//
// Two things keep clang-tidy-diff green over a file every line of which
// is a changed line. `inline` gives each constant external linkage, so
// clang-diagnostic-unused-const-variable does not fire on a fixture
// nothing compiles. And misc-misleading-bidirectional still fires on two
// of the escapes below -- an escape produces the same bytes in the
// compiled string, so that check judges the string's content while this
// gate judges the source bytes. The two are complementary, and the
// per-line suppressions record where they disagree.

inline constexpr const char* kArabicLetterMark = "\u061C";
inline constexpr const char* kLeftToRightMark = "\u200E";
inline constexpr const char* kRightToLeftMark = "\u200F";
// RLO with no terminating PDF, and FSI with no terminating PDI: the
// content check reads the expanded literal, which is exactly the point
// of writing them as escapes.
// NOLINTNEXTLINE(misc-misleading-bidirectional)
inline constexpr const char* kRightToLeftOverride = "\u202E";
// NOLINTNEXTLINE(misc-misleading-bidirectional)
inline constexpr const char* kFirstStrongIsolate = "\u2068";
inline constexpr const char* kPopDirectionalIsolate = "\u2069";

// The brace form too, which is how a u8 literal often spells it.
inline constexpr const char* kBraced = "\u{200E}+\u{200E}";
