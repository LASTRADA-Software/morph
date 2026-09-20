// SPDX-License-Identifier: Apache-2.0
//
// The accepted spelling. Every bidi control here is an escape, so the bytes on
// disk are ASCII and a reviewer reads exactly what the compiler reads. This is
// what src/qt/forms/tests/tst_i18n.qml does for all thirteen of its
// occurrences, and what morph#610 converted nine assertion lines to.

constexpr const char* kArabicLetterMark = "\u061C";
constexpr const char* kLeftToRightMark = "\u200E";
constexpr const char* kRightToLeftMark = "\u200F";
constexpr const char* kRightToLeftOverride = "\u202E";
constexpr const char* kFirstStrongIsolate = "\u2068";
constexpr const char* kPopDirectionalIsolate = "\u2069";

// The brace form too, which is how a u8 literal often spells it.
constexpr const char* kBraced = "\u{200E}+\u{200E}";
